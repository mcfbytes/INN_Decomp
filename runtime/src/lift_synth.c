/* Synthetic calls (docs/lifting.md, "Synthetic calls"): call lists name lifted entries and the
 * arguments and registers of one call each. When a trigger (the n-th entry of some reached function)
 * fires, each call is set up on top of the machine's state there, shadow-verified as a live call is,
 * and the machine is put back exactly as it was at the trigger. Buffers live in the HMA. */
#include <ctype.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "dos.h"
#include "lift_internal.h"

enum {
    SNAPSHOT_BYTES = MEM_ONE_MEG + 0x10000,
    HMA_SEG = 0xFFFF,
    HMA_LO = MEM_ONE_MEG,           /* FFFF:0010 */
    HMA_HI = MEM_ONE_MEG + 0xFFF0,  /* FFFF:FFFF + 1 */
    BUF_GAP = 16,                   /* zero bytes between buffers, compared like the buffers */
    MAX_NAMES = 16, MAX_PUSH = 128, MAX_REGS = 16, NAME_LEN = 64,
};
static const uint64_t SYNTH_BUDGET_INSNS = 50000000;

typedef struct SynthCall {
    char label[96];  /* file:line, for reports */
    char module[32], target[NAME_LEN];
    char *items;     /* arguments and registers, evaluated when the trigger fires */
} SynthCall;

typedef struct SynthTrigger {
    char label[96];
    char module[32], target[NAME_LEN];
    bool near;        /* for arg(n) when the target is an address */
    int nth, count;
    bool resolved, fired;
    uint32_t linear;
    int first_call, call_count;
} SynthTrigger;

typedef struct Synth {
    SynthTrigger *triggers;
    int trigger_count;
    SynthCall *calls;
    int call_count;
    struct { char module[32]; uint16_t seg; } dgroups[16];
    int dgroup_count;
    bool check, log;
    /* the machine at the trigger */
    Cpu cpu;
    uint8_t *mem, *vga, *dos, *hooks;
    uint64_t virtual_us, slice_left, slice_start_insn, c_io_ns, irq_pending;
    bool a20, exit_requested;
    uint8_t *vga_after, *dos_after;
} Synth;

static Synth *synth(Machine *m) { return lift_state(m)->synth; }

/* ---- looking up entries ---- */

static bool parse_seg_off(const char *s, uint16_t *seg, uint16_t *off)
{
    unsigned a, b;
    char tail;
    if (sscanf(s, "%x:%x%c", &a, &b, &tail) != 2 || a > 0xFFFF || b > 0xFFFF)
        return false;
    *seg = (uint16_t)a;
    *off = (uint16_t)b;
    return true;
}

/* The table row of a lifted entry named by name or seg:off, with its stats. */
static const LiftedEntry *find_entry(LiftState *s, const char *module, const char *target, HookStats **stats)
{
    uint16_t seg, off;
    bool by_addr = parse_seg_off(target, &seg, &off);
    for (int t = 0; t < s->table_count; t++)
        for (int i = 0; i < s->tables[t].count; i++) {
            const LiftedEntry *e = &s->tables[t].entries[i];
            if (strcmp(e->module, module) != 0)
                continue;
            if (by_addr ? e->seg == seg && e->off == off : !strcmp(e->name, target)) {
                *stats = &s->tables[t].stats[i];
                return e;
            }
        }
    return NULL;
}

/* ---- loading ---- */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = 0;
    return s;
}

/* Cuts a comment that starts outside a string. */
static void cut_comment(char *s)
{
    bool quoted = false;
    for (char *p = s; *p; p++) {
        if (quoted && *p == '\\' && p[1])
            p++;
        else if (*p == '"')
            quoted = !quoted;
        else if (*p == '#' && !quoted && (p == s || p[-1] == ' ' || p[-1] == '\t')) {
            *p = 0;
            return;
        }
    }
}

static void load_file(Synth *y, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        fatal("INN_SYNTH: cannot open %s", path);
    char line[4096];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        cut_comment(line);
        char *p = trim(line), word[16], module[32], target[NAME_LEN];
        int used = 0;
        if (!*p)
            continue;
        char label[96];
        snprintf(label, sizeof label, "%s:%d", strrchr(path, '/') ? strrchr(path, '/') + 1 : path, lineno);
        if (sscanf(p, "%15s %31s %63s %n", word, module, target, &used) < 3)
            fatal("INN_SYNTH %s: expected 'dgroup|after|call <module> <entry> ...'", label);
        const char *rest = p + used;
        if (!strcmp(word, "dgroup")) {
            unsigned seg;
            if (sscanf(target, "%x", &seg) != 1 || y->dgroup_count == 16)
                fatal("INN_SYNTH %s: dgroup <module> <hex segment>", label);
            snprintf(y->dgroups[y->dgroup_count].module, sizeof y->dgroups[0].module, "%s", module);
            y->dgroups[y->dgroup_count++].seg = (uint16_t)seg;
        } else if (!strcmp(word, "after")) {
            y->triggers = realloc(y->triggers, (size_t)(y->trigger_count + 1) * sizeof *y->triggers);
            if (!y->triggers)
                fatal("out of memory");
            SynthTrigger *t = &y->triggers[y->trigger_count++];
            *t = (SynthTrigger){ .nth = 1, .first_call = y->call_count };
            snprintf(t->label, sizeof t->label, "%s", label);
            snprintf(t->module, sizeof t->module, "%s", module);
            snprintf(t->target, sizeof t->target, "%s", target);
            char opt[16];
            int n;
            while (sscanf(rest, "%15s %n", opt, &n) == 1) {
                rest += n;
                if (!strcmp(opt, "near"))
                    t->near = true;
                else if (!strcmp(opt, "far"))
                    t->near = false;
                else if (atoi(opt) > 0)
                    t->nth = atoi(opt);
                else
                    fatal("INN_SYNTH %s: after <module> <entry> [n] [near|far]", label);
            }
        } else if (!strcmp(word, "call")) {
            if (y->trigger_count == 0)
                fatal("INN_SYNTH %s: a call needs an 'after' line before it", label);
            y->calls = realloc(y->calls, (size_t)(y->call_count + 1) * sizeof *y->calls);
            if (!y->calls)
                fatal("out of memory");
            SynthCall *c = &y->calls[y->call_count++];
            *c = (SynthCall){ 0 };
            snprintf(c->label, sizeof c->label, "%s", label);
            snprintf(c->module, sizeof c->module, "%s", module);
            snprintf(c->target, sizeof c->target, "%s", target);
            if (!(c->items = strdup(rest)))
                fatal("out of memory");
            y->triggers[y->trigger_count - 1].call_count++;
        } else {
            fatal("INN_SYNTH %s: unknown directive '%s'", label, word);
        }
    }
    fclose(f);
}

static bool validate(Machine *m, Synth *y);

void lift_synth_load(Machine *m, const char *paths)
{
    LiftState *s = lift_state(m);
    Synth *y = calloc(1, sizeof *y);
    if (!y)
        fatal("out of memory");
    char *list = strdup(paths);
    if (!list)
        fatal("out of memory");
    for (char *save = NULL, *p = strtok_r(list, ",", &save); p; p = strtok_r(NULL, ",", &save))
        load_file(y, trim(p));
    free(list);
    const char *check = getenv("INN_SYNTH_CHECK"), *verbose = getenv("INN_SYNTH_LOG");
    y->check = check && *check && *check != '0';
    y->log = verbose && *verbose && *verbose != '0';
    y->mem = malloc(SNAPSHOT_BYTES);
    y->vga = malloc(vga_snapshot_size());
    y->dos = malloc(dos_snapshot_size());
    y->vga_after = malloc(vga_snapshot_size());
    y->dos_after = malloc(dos_snapshot_size());
    y->hooks = malloc(HOOK_BITMAP_BYTES);
    if (!y->mem || !y->vga || !y->dos || !y->vga_after || !y->dos_after || !y->hooks)
        fatal("out of memory");
    if (!validate(m, y))
        fatal("INN_SYNTH: call list errors");
    s->synth = y;
    if (!s->verify)
        lift_verify_enable(m, 0); /* synthetic calls only; live calls run unverified */
    log_msg("synth: %d calls under %d triggers", y->call_count, y->trigger_count);
}

/* ---- triggers ---- */

void lift_synth_module(Machine *m, const char *module, uint16_t load_seg)
{
    LiftState *s = lift_state(m);
    Synth *y = s ? s->synth : NULL;
    if (!y)
        return;
    for (int i = 0; i < y->trigger_count; i++) {
        SynthTrigger *t = &y->triggers[i];
        if (t->fired || strcmp(t->module, module) != 0)
            continue;
        uint16_t seg, off;
        if (!parse_seg_off(t->target, &seg, &off)) {
            HookStats *stats;
            const LiftedEntry *e = find_entry(s, module, t->target, &stats);
            if (!e)
                continue;
            seg = e->seg;
            off = e->off;
            t->near = e->near_return;
        }
        t->linear = real_addr((uint16_t)(load_seg + seg), off);
        t->resolved = true;
        lift_set_hook_bit(m, t->linear, true);
    }
}

static bool hook_at(LiftState *s, uint32_t linear)
{
    for (int i = 0; i < s->active_count; i++)
        if (s->active[i].linear == linear)
            return true;
    return false;
}

void lift_synth_unloaded(Machine *m, uint32_t lo, uint32_t hi)
{
    LiftState *s = lift_state(m);
    Synth *y = s->synth;
    if (!y)
        return;
    for (int i = 0; i < y->trigger_count; i++) {
        SynthTrigger *t = &y->triggers[i];
        if (t->resolved && t->linear >= lo && t->linear < hi) {
            t->resolved = false;
            if (!hook_at(s, t->linear))
                lift_set_hook_bit(m, t->linear, false);
        }
    }
}

/* ---- evaluating a call's items ---- */

typedef enum { V_NUM, V_LONG, V_PTR } VKind;
typedef struct Val { VKind kind; int64_t n; uint16_t seg; } Val; /* V_PTR: n is the offset */

typedef struct Eval {
    Machine *m;      /* NULL: syntax check only, nothing is read or written */
    const char *p;
    jmp_buf fail;
    char err[160];
    uint16_t dgroup;
    bool trigger_near;
    uint32_t hma_next;
    struct { char name[24]; Val v; } names[MAX_NAMES];
    int name_count;
    uint16_t push[MAX_PUSH];
    int push_count;
    struct { int reg; uint16_t value; } regs[MAX_REGS]; /* reg: R_* for general, 8 + S_* for segments */
    int reg_count;
} Eval;

static _Noreturn void fail(Eval *ev, const char *msg)
{
    snprintf(ev->err, sizeof ev->err, "%s at '%.24s'", msg, ev->p);
    longjmp(ev->fail, 1);
}

static void eval_init(Eval *ev, Machine *m, const char *items, uint16_t dgroup, bool trigger_near)
{
    memset(ev, 0, sizeof *ev);
    ev->m = m;
    ev->p = items;
    ev->dgroup = dgroup;
    ev->trigger_near = trigger_near;
    ev->hma_next = HMA_LO;
}

static void skip_space(Eval *ev)
{
    while (isspace((unsigned char)*ev->p))
        ev->p++;
}

static bool accept(Eval *ev, char c)
{
    skip_space(ev);
    if (*ev->p != c)
        return false;
    ev->p++;
    return true;
}

static void expect(Eval *ev, char c)
{
    if (!accept(ev, c)) {
        char msg[24];
        snprintf(msg, sizeof msg, "expected '%c'", c);
        fail(ev, msg);
    }
}

static int ident(Eval *ev, char *out, size_t cap)
{
    size_t n = 0;
    while (isalnum((unsigned char)ev->p[n]) || ev->p[n] == '_')
        n++;
    if (n == 0 || n >= cap || isdigit((unsigned char)ev->p[0]))
        return 0;
    memcpy(out, ev->p, n);
    out[n] = 0;
    return (int)n;
}

static uint16_t word_of(Eval *ev, Val v)
{
    if (v.kind != V_NUM || v.n < -32768 || v.n > 0xFFFF)
        fail(ev, v.kind == V_PTR ? "a pointer where a word is expected" : "value does not fit a word (long()?)");
    return (uint16_t)v.n;
}

static uint8_t rd8(Eval *ev, uint32_t linear) { return ev->m ? mem_rd8(ev->m, linear) : 0; }
static uint16_t rd16(Eval *ev, uint32_t linear) { return (uint16_t)(rd8(ev, linear) | rd8(ev, linear + 1) << 8); }

static uint16_t cpu_seg(Eval *ev, int s) { return ev->m ? ev->m->cpu.seg[s] : 0; }

/* A buffer of `bytes` bytes in the HMA, zero-filled, 16-byte aligned with a gap after it. */
static Val alloc_buf(Eval *ev, uint32_t bytes)
{
    uint32_t at = ev->hma_next;
    if (bytes > HMA_HI - at)
        fail(ev, "buffers exceed the HMA (65520 bytes)");
    ev->hma_next = (at + bytes + BUF_GAP + 15) & ~15u;
    if (ev->hma_next > HMA_HI)
        ev->hma_next = HMA_HI;
    return (Val){ V_PTR, (int64_t)(at - real_addr(HMA_SEG, 0)), HMA_SEG };
}

static void put8(Eval *ev, Val buf, uint32_t i, uint8_t b)
{
    if (ev->m)
        ev->m->mem[real_addr(buf.seg, 0) + (uint32_t)buf.n + i] = b;
}

/* A string literal's bytes, escapes decoded; returns the length. */
static size_t string_lit(Eval *ev, uint8_t *out, size_t cap)
{
    expect(ev, '"');
    size_t n = 0;
    while (*ev->p && *ev->p != '"') {
        int c = (unsigned char)*ev->p++;
        if (c == '\\') {
            c = (unsigned char)*ev->p++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case '0': c = 0; break;
            case 'x': {
                char hex[3] = { ev->p[0], ev->p[0] ? ev->p[1] : 0, 0 };
                char *end;
                c = (int)strtol(hex, &end, 16);
                if (end != hex + 2)
                    fail(ev, "\\x needs two hex digits");
                ev->p += 2;
                break;
            }
            case 0: fail(ev, "unterminated string");
            default: break; /* \\ \" and any other character stand for themselves */
            }
        }
        if (n == cap)
            fail(ev, "string too long");
        out[n++] = (uint8_t)c;
    }
    expect(ev, '"');
    return n;
}

static Val expr(Eval *ev);

static int64_t number_arg_value(Eval *ev, Val v)
{
    if (v.kind == V_PTR)
        fail(ev, "a pointer where a number is expected");
    return v.n;
}

static int64_t number_arg(Eval *ev)
{
    Val v = expr(ev);
    if (v.kind == V_PTR)
        fail(ev, "a pointer where a number is expected");
    return v.n;
}

static uint16_t segment_name(Eval *ev)
{
    char id[16];
    skip_space(ev);
    int n = ident(ev, id, sizeof id);
    static const char *const SEGS[] = { "ES", "CS", "SS", "DS" };
    for (int s = 0; n && s < 4; s++)
        if (!strcmp(id, SEGS[s])) {
            ev->p += n;
            return cpu_seg(ev, s);
        }
    if (n && !strcmp(id, "DGROUP")) {
        ev->p += n;
        return ev->dgroup;
    }
    Val v = expr(ev);
    return word_of(ev, v);
}

/* [SEG:off] and its typed forms read memory as it is at the trigger. */
static uint32_t memory_ref(Eval *ev)
{
    expect(ev, '[');
    uint16_t seg = segment_name(ev);
    expect(ev, ':');
    uint16_t off = word_of(ev, expr(ev));
    expect(ev, ']');
    return real_addr(seg, off);
}

static Val call_fn(Eval *ev, const char *fn)
{
    if (!strcmp(fn, "buf")) { /* buf(size [, "text" | byte | fill=byte]...) */
        expect(ev, '(');
        int64_t size = number_arg(ev);
        if (size <= 0 || size > 0xFFF0)
            fail(ev, "buffer size must be 1..65520");
        Val b = alloc_buf(ev, (uint32_t)size);
        uint32_t at = 0;
        int fill = -1;
        while (accept(ev, ',')) {
            skip_space(ev);
            if (*ev->p == '"') {
                uint8_t text[1024];
                size_t n = string_lit(ev, text, sizeof text);
                for (size_t i = 0; i < n; i++, at++)
                    if (at < size)
                        put8(ev, b, at, text[i]);
                    else
                        fail(ev, "initial bytes exceed the buffer");
            } else if (!strncmp(ev->p, "fill=", 5)) {
                ev->p += 5;
                fill = (uint8_t)number_arg(ev);
            } else {
                if (at >= size)
                    fail(ev, "initial bytes exceed the buffer");
                put8(ev, b, at++, (uint8_t)number_arg(ev));
            }
        }
        expect(ev, ')');
        for (; fill >= 0 && at < size; at++)
            put8(ev, b, at, (uint8_t)fill);
        return b;
    }
    if (!strcmp(fn, "words") || !strcmp(fn, "bytes")) { /* words(a, b, ...): a buffer holding them */
        bool words = fn[0] == 'w';
        expect(ev, '(');
        uint8_t data[512];
        size_t n = 0;
        if (!accept(ev, ')')) {
            do {
                Val v = expr(ev);
                uint16_t parts[2] = { 0 };
                int count = 1;
                if (v.kind == V_PTR && words) {
                    parts[0] = (uint16_t)v.n;
                    parts[1] = v.seg;
                    count = 2;
                } else {
                    parts[0] = words ? word_of(ev, v) : (uint16_t)(uint8_t)number_arg_value(ev, v);
                }
                for (int i = 0; i < count; i++) {
                    if (n + 2 > sizeof data)
                        fail(ev, "too many elements");
                    data[n++] = (uint8_t)parts[i];
                    if (words)
                        data[n++] = (uint8_t)(parts[i] >> 8);
                }
            } while (accept(ev, ','));
            expect(ev, ')');
        }
        Val b = alloc_buf(ev, n ? (uint32_t)n : 1);
        for (size_t i = 0; i < n; i++)
            put8(ev, b, (uint32_t)i, data[i]);
        return b;
    }
    if (!strcmp(fn, "long")) {
        expect(ev, '(');
        int64_t v = number_arg(ev);
        expect(ev, ')');
        if (v < -(int64_t)0x80000000 || v > 0xFFFFFFFF)
            fail(ev, "value does not fit 32 bits");
        return (Val){ V_LONG, (int64_t)(uint32_t)v, 0 };
    }
    if (!strcmp(fn, "far")) { /* far(SEG, off) */
        expect(ev, '(');
        uint16_t seg = segment_name(ev);
        expect(ev, ',');
        uint16_t off = word_of(ev, expr(ev));
        expect(ev, ')');
        return (Val){ V_PTR, off, seg };
    }
    if (!strcmp(fn, "off") || !strcmp(fn, "seg")) {
        expect(ev, '(');
        Val v = expr(ev);
        expect(ev, ')');
        if (v.kind != V_PTR)
            fail(ev, "off()/seg() need a pointer");
        return (Val){ V_NUM, fn[0] == 'o' ? (uint16_t)v.n : v.seg, 0 };
    }
    if (!strcmp(fn, "arg") || !strcmp(fn, "argptr")) { /* the trigger's own arguments, by word */
        expect(ev, '(');
        int64_t i = number_arg(ev);
        expect(ev, ')');
        uint16_t sp = ev->m ? ev->m->cpu.r[R_SP].w : 0;
        uint32_t at = real_addr(cpu_seg(ev, S_SS), (uint16_t)(sp + (ev->trigger_near ? 2 : 4) + 2 * i));
        if (fn[3] == 0)
            return (Val){ V_NUM, rd16(ev, at), 0 };
        return (Val){ V_PTR, rd16(ev, at), rd16(ev, at + 2) };
    }
    fail(ev, "unknown function");
}

static Val primary(Eval *ev)
{
    skip_space(ev);
    const char *start = ev->p;
    if (*ev->p == '"') { /* a fresh NUL-terminated copy */
        uint8_t text[1024];
        size_t n = string_lit(ev, text, sizeof text - 1);
        Val b = alloc_buf(ev, (uint32_t)n + 1);
        for (size_t i = 0; i < n; i++)
            put8(ev, b, (uint32_t)i, text[i]);
        return b;
    }
    if (*ev->p == '\'') {
        ev->p++;
        int c = (unsigned char)*ev->p++;
        if (c == '\\')
            c = (unsigned char)*ev->p++;
        if (*ev->p++ != '\'')
            fail(ev, "bad character literal");
        return (Val){ V_NUM, c, 0 };
    }
    if (*ev->p == '(') {
        ev->p++;
        Val v = expr(ev);
        expect(ev, ')');
        return v;
    }
    if (*ev->p == '[')
        return (Val){ V_NUM, rd16(ev, memory_ref(ev)), 0 };
    if (isdigit((unsigned char)*ev->p)) {
        char *end;
        long long v = strtoll(ev->p, &end, 0);
        ev->p = end;
        return (Val){ V_NUM, v, 0 };
    }
    char id[24];
    int n = ident(ev, id, sizeof id);
    if (!n)
        fail(ev, "expected a value");
    ev->p += n;
    if (!strcmp(id, "farat") || !strcmp(id, "longat")) { /* farat[SEG:off], longat[SEG:off] */
        uint32_t at = memory_ref(ev);
        if (id[0] == 'f')
            return (Val){ V_PTR, rd16(ev, at), rd16(ev, at + 2) };
        return (Val){ V_LONG, (int64_t)((uint32_t)rd16(ev, at) | (uint32_t)rd16(ev, at + 2) << 16), 0 };
    }
    if (*ev->p == '(')
        return call_fn(ev, id);
    if (!strcmp(id, "null"))
        return (Val){ V_PTR, 0, 0 };
    if (!strcmp(id, "DGROUP"))
        return (Val){ V_NUM, ev->dgroup, 0 };
    for (int i = 0; i < ev->name_count; i++)
        if (!strcmp(ev->names[i].name, id))
            return ev->names[i].v;
    ev->p = start;
    fail(ev, "unknown name");
}

static Val unary(Eval *ev)
{
    skip_space(ev);
    if (*ev->p == '-') {
        ev->p++;
        Val v = unary(ev);
        if (v.kind == V_PTR)
            fail(ev, "cannot negate a pointer");
        v.n = v.kind == V_LONG ? (int64_t)(uint32_t)-(uint32_t)v.n : -v.n;
        return v;
    }
    return primary(ev);
}

/* + and - bind only without a space before them, so items stay separated by spaces. */
static Val expr(Eval *ev)
{
    Val v = unary(ev);
    while (*ev->p == '+' || *ev->p == '-') {
        bool minus = *ev->p++ == '-';
        Val r = unary(ev);
        if (r.kind == V_PTR)
            fail(ev, "pointer on the right of + or -");
        int64_t d = minus ? -r.n : r.n;
        if (v.kind == V_PTR)
            v.n = (uint16_t)(v.n + d);
        else if (v.kind == V_LONG || r.kind == V_LONG)
            v = (Val){ V_LONG, (int64_t)(uint32_t)(v.n + d), 0 };
        else
            v.n += d;
    }
    return v;
}

static void push_val(Eval *ev, Val v)
{
    if (ev->push_count + 2 > MAX_PUSH)
        fail(ev, "too many arguments");
    if (v.kind == V_PTR) {
        ev->push[ev->push_count++] = (uint16_t)v.n;
        ev->push[ev->push_count++] = v.seg;
    } else if (v.kind == V_LONG) {
        ev->push[ev->push_count++] = (uint16_t)v.n;
        ev->push[ev->push_count++] = (uint16_t)((uint64_t)v.n >> 16);
    } else {
        ev->push[ev->push_count++] = word_of(ev, v);
    }
}

static void set_reg(Eval *ev, int reg, uint16_t value)
{
    if (ev->reg_count == MAX_REGS)
        fail(ev, "too many registers");
    ev->regs[ev->reg_count].reg = reg;
    ev->regs[ev->reg_count++].value = value;
}

/* REG=, PAIR= (high:low, a pointer or a long), let name=, name= (bind and push), or a value. */
static bool register_item(Eval *ev)
{
    static const struct { const char *name; int hi, lo; } PAIRS[] = {
        { "DX:AX", R_DX, R_AX }, { "ES:DI", 8 + S_ES, R_DI }, { "ES:SI", 8 + S_ES, R_SI },
        { "ES:BX", 8 + S_ES, R_BX }, { "DS:SI", 8 + S_DS, R_SI }, { "DS:DI", 8 + S_DS, R_DI },
        { "DS:DX", 8 + S_DS, R_DX }, { "DS:BX", 8 + S_DS, R_BX }, { "CX:BX", R_CX, R_BX },
    };
    static const struct { const char *name; int reg; } REGS[] = {
        { "AX", R_AX }, { "BX", R_BX }, { "CX", R_CX }, { "DX", R_DX }, { "SI", R_SI }, { "DI", R_DI },
        { "BP", R_BP }, { "DS", 8 + S_DS }, { "ES", 8 + S_ES },
    };
    for (size_t i = 0; i < sizeof PAIRS / sizeof PAIRS[0]; i++)
        if (!strncmp(ev->p, PAIRS[i].name, 5) && ev->p[5] == '=') {
            ev->p += 6;
            Val v = expr(ev);
            uint16_t hi, lo;
            if (v.kind == V_PTR) {
                hi = v.seg;
                lo = (uint16_t)v.n;
            } else {
                if (v.n < -(int64_t)0x80000000 || v.n > 0xFFFFFFFF)
                    fail(ev, "value does not fit 32 bits");
                hi = (uint16_t)((uint64_t)v.n >> 16);
                lo = (uint16_t)v.n;
            }
            set_reg(ev, PAIRS[i].hi, hi);
            set_reg(ev, PAIRS[i].lo, lo);
            return true;
        }
    for (size_t i = 0; i < sizeof REGS / sizeof REGS[0]; i++)
        if (!strncmp(ev->p, REGS[i].name, 2) && ev->p[2] == '=') {
            ev->p += 3;
            if (REGS[i].reg >= 8) {
                set_reg(ev, REGS[i].reg, segment_name(ev));
                return true;
            }
            Val v = expr(ev);
            if (v.kind == V_LONG && v.n <= 0xFFFF)
                v.kind = V_NUM;
            set_reg(ev, REGS[i].reg, word_of(ev, v));
            return true;
        }
    return false;
}

static void item(Eval *ev)
{
    if (register_item(ev))
        return;
    bool let = !strncmp(ev->p, "let ", 4);
    if (let) {
        ev->p += 4;
        skip_space(ev);
    }
    char id[24];
    int n = ident(ev, id, sizeof id);
    if (n && ev->p[n] == '=') {
        ev->p += n + 1;
        if (ev->name_count == MAX_NAMES)
            fail(ev, "too many names");
        Val v = expr(ev);
        snprintf(ev->names[ev->name_count].name, sizeof ev->names[0].name, "%s", id);
        ev->names[ev->name_count++].v = v;
        if (!let)
            push_val(ev, v);
        return;
    }
    if (let)
        fail(ev, "let needs name=value");
    push_val(ev, expr(ev));
}

static bool eval_items(Eval *ev, char *err, size_t cap)
{
    if (setjmp(ev->fail)) {
        snprintf(err, cap, "%s", ev->err);
        return false;
    }
    for (skip_space(ev); *ev->p; skip_space(ev)) {
        item(ev);
        if (*ev->p && !isspace((unsigned char)*ev->p))
            fail(ev, "expected a space between items");
    }
    return true;
}

static uint16_t dgroup_of(Machine *m, Synth *y, const char *module)
{
    for (int i = 0; i < y->dgroup_count; i++)
        if (!strcmp(y->dgroups[i].module, module))
            return (uint16_t)((m ? lift_module_base(m, module) : 0) + y->dgroups[i].seg);
    return m ? m->cpu.seg[S_DS] : 0; /* the trigger's DS */
}

/* Syntax and names of every call, before the run starts. */
static bool validate(Machine *m, Synth *y)
{
    LiftState *s = lift_state(m);
    bool ok = true;
    Eval *ev = malloc(sizeof *ev);
    if (!ev)
        fatal("out of memory");
    for (int i = 0; i < y->call_count; i++) {
        SynthCall *c = &y->calls[i];
        HookStats *stats;
        const LiftedEntry *e = find_entry(s, c->module, c->target, &stats);
        char err[200];
        if (!e) {
            log_msg("synth: %s: no lifted entry %s %s in this build", c->label, c->module, c->target);
            continue; /* not fatal: a call list may cover entries of another build */
        }
        if (e->region || e->no_shadow) {
            log_msg("synth: %s: %s cannot be called synthetically (region or not shadowed)", c->label, e->name);
            ok = false;
            continue;
        }
        eval_init(ev, NULL, c->items, 0, false);
        if (!eval_items(ev, err, sizeof err)) {
            log_msg("synth: %s: %s", c->label, err);
            ok = false;
        }
    }
    free(ev);
    return ok;
}

/* ---- firing ---- */

static void save_trigger(Machine *m, Synth *y)
{
    y->cpu = m->cpu;
    memcpy(y->mem, m->mem, SNAPSHOT_BYTES);
    vga_snapshot_save(m, y->vga);
    dos_snapshot_save(m, y->dos);
    memcpy(y->hooks, m->exec_hooks, HOOK_BITMAP_BYTES);
    y->virtual_us = m->virtual_us;
    y->slice_left = m->slice_left;
    y->slice_start_insn = m->slice_start_insn;
    y->c_io_ns = m->c_io_ns;
    y->irq_pending = m->irq_pending;
    y->a20 = m->a20;
    y->exit_requested = m->exit_requested;
}

static void restore_trigger(Machine *m, Synth *y)
{
    m->cpu = y->cpu;
    memcpy(m->mem, y->mem, SNAPSHOT_BYTES);
    vga_snapshot_load(m, y->vga);
    dos_snapshot_load(m, y->dos);
    m->virtual_us = y->virtual_us;
    m->slice_left = y->slice_left;
    m->slice_start_insn = y->slice_start_insn;
    m->c_io_ns = y->c_io_ns;
    m->irq_pending = y->irq_pending;
    m->a20 = y->a20;
    m->exit_requested = y->exit_requested;
}

/* INN_SYNTH_CHECK: everything the trigger state holds, compared after the restore. */
static void check_restored(Machine *m, Synth *y, const char *label)
{
    const char *what = NULL;
    vga_snapshot_save(m, y->vga_after);
    dos_snapshot_save(m, y->dos_after);
    if (memcmp(&m->cpu, &y->cpu, sizeof m->cpu) != 0)
        what = "CPU";
    else if (memcmp(m->mem, y->mem, SNAPSHOT_BYTES) != 0)
        what = "memory";
    else if (memcmp(y->vga_after, y->vga, vga_snapshot_size()) != 0)
        what = "VGA";
    else if (memcmp(y->dos_after, y->dos, dos_snapshot_size()) != 0)
        what = "DOS state";
    else if (memcmp(m->exec_hooks, y->hooks, HOOK_BITMAP_BYTES) != 0)
        what = "lifted hooks (an overlay or module was loaded or freed)";
    else if (m->virtual_us != y->virtual_us || m->irq_pending != y->irq_pending || m->a20 != y->a20 ||
             m->io_mode != IO_LIVE || m->irq_hold)
        what = "machine timing, interrupts, A20 or I/O mode";
    if (what)
        fatal("synth: %s: the machine after the call differs from the trigger state: %s", label, what);
}

/* Pushes the arguments (first item at the lowest address), the return address (the entry itself,
 * so the original's return is caught there) and loads the registers. */
static bool setup_call(Machine *m, Synth *y, const SynthCall *c, const ActiveHook *h, bool trigger_near, Eval *ev)
{
    char err[200];
    memset(m->mem + HMA_LO, 0, HMA_HI - HMA_LO);
    eval_init(ev, m, c->items, dgroup_of(m, y, c->module), trigger_near);
    if (!eval_items(ev, err, sizeof err)) {
        log_msg("synth: %s: %s", c->label, err);
        return false;
    }
    m->a20 = true; /* buffers live at FFFF:0010 and up */
    for (int i = ev->push_count - 1; i >= 0; i--)
        cpu_push16(m, ev->push[i]);
    uint16_t cs = (uint16_t)(h->load_seg + h->entry->seg), ip = h->entry->off;
    if (!h->entry->near_return)
        cpu_push16(m, cs);
    cpu_push16(m, ip);
    for (int i = 0; i < ev->reg_count; i++) {
        int r = ev->regs[i].reg;
        if (r >= 8)
            cpu_set_seg(&m->cpu, r - 8, ev->regs[i].value);
        else
            m->cpu.r[r].w = ev->regs[i].value;
    }
    cpu_set_seg(&m->cpu, S_CS, cs);
    m->cpu.eip = ip;
    m->cpu.halted = false;
    return true;
}

/* The hook of a call's target: the armed one, or one made from its module's load segment. */
static bool target_hook(Machine *m, const SynthCall *c, ActiveHook *out)
{
    LiftState *s = lift_state(m);
    HookStats *stats;
    const LiftedEntry *e = find_entry(s, c->module, c->target, &stats);
    if (!e)
        return false;
    for (int i = 0; i < s->active_count; i++)
        if (s->active[i].entry == e) {
            *out = s->active[i];
            return true;
        }
    uint16_t base = lift_module_base(m, e->module);
    if (!base)
        return false;
    *out = (ActiveHook){ .linear = real_addr((uint16_t)(base + e->seg), e->off), .load_seg = base,
                         .reloc_base = base, .entry = e, .stats = stats };
    return true;
}

static void fire(Machine *m, Synth *y, SynthTrigger *t)
{
    void *coverage = m->coverage;
    m->coverage = NULL; /* synthetic calls do not count as reached */
    Eval *ev = malloc(sizeof *ev);
    if (!ev)
        fatal("out of memory");
    save_trigger(m, y);
    int checked = 0, mismatched = 0;
    for (int i = t->first_call; i < t->first_call + t->call_count; i++) {
        const SynthCall *c = &y->calls[i];
        ActiveHook h;
        if (!target_hook(m, c, &h)) {
            log_msg("synth: %s: %s %s is not loaded here; skipped", c->label, c->module, c->target);
            continue;
        }
        if (!lift_entry_bytes_match(m, &h)) {
            log_msg("synth: %s: %s bytes differ at the entry; skipped", c->label, h.entry->name);
            continue;
        }
        if (setup_call(m, y, c, &h, t->near, ev)) {
            m->slice_left = 2 * SYNTH_BUDGET_INSNS; /* no slice ends: devices stand still */
            SynthResult r = lift_verify_synthetic(m, &h, c->label, SYNTH_BUDGET_INSNS);
            checked++;
            mismatched += r.mismatch;
            if (y->log) {
                char text[sizeof r.at_result * 4 + 8] = "";
                size_t n = 0;
                for (size_t k = 0; r.has_bytes && k < sizeof r.at_result && r.at_result[k]; k++)
                    n += (size_t)snprintf(text + n, sizeof text - n, r.at_result[k] >= 0x20 && r.at_result[k] < 0x7F
                                          ? "%c" : "\\x%02X", r.at_result[k]);
                log_msg("synth: %s %s %s: original DX:AX %04X:%04X%s%s%s%s", c->label, c->module, h.entry->name,
                        r.dx, r.ax, r.has_bytes ? " -> \"" : "", text, r.has_bytes ? "\"" : "",
                        r.mismatch ? " MISMATCH" : "");
            }
        }
        restore_trigger(m, y);
        if (y->check)
            check_restored(m, y, c->label);
    }
    free(ev);
    m->coverage = coverage;
    log_msg("synth: %s after %s %s %d fired at %04X:%04X SS:SP %04X:%04X DS %04X: %d calls checked, %d mismatched",
            t->label, t->module, t->target, t->nth, y->cpu.seg[S_CS], (unsigned)(y->cpu.eip & 0xFFFF),
            y->cpu.seg[S_SS], y->cpu.r[R_SP].w, y->cpu.seg[S_DS], checked, mismatched);
}

void lift_synth_reached(Machine *m, uint32_t linear)
{
    Synth *y = synth(m);
    for (int i = 0; i < y->trigger_count; i++) {
        SynthTrigger *t = &y->triggers[i];
        if (!t->resolved || t->fired || t->linear != linear)
            continue;
        if (++t->count == t->nth) {
            t->fired = true;
            fire(m, y, t);
        }
    }
    for (int i = 0; i < y->trigger_count; i++)
        if (y->triggers[i].resolved && !y->triggers[i].fired && y->triggers[i].linear == linear)
            return;
    if (!hook_at(lift_state(m), linear)) /* no trigger left at this address */
        lift_set_hook_bit(m, linear, false);
}

void lift_synth_shutdown(Machine *m)
{
    Synth *y = synth(m);
    if (!y)
        return;
    for (int i = 0; i < y->trigger_count; i++)
        if (!y->triggers[i].fired)
            log_msg("synth: %s after %s %s %d never fired (entered %d times); %d calls not checked",
                    y->triggers[i].label, y->triggers[i].module, y->triggers[i].target, y->triggers[i].nth,
                    y->triggers[i].count, y->triggers[i].call_count);
    for (int i = 0; i < y->call_count; i++)
        free(y->calls[i].items);
    free(y->calls);
    free(y->triggers);
    free(y->mem);
    free(y->vga);
    free(y->dos);
    free(y->vga_after);
    free(y->dos_after);
    free(y->hooks);
    free(y);
    lift_state(m)->synth = NULL;
}
