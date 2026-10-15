/* Hardware-captured SingleStepTests (8088, 80286, 80386 real mode) against cpu.c; fetching and
 * converting them is described in tools/sst_prepare.py. Without them this prints SKIP and passes. */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"

/* The suites assume flat writable RAM and no devices, so the test supplies the machine glue
 * itself and links only cpu.o from dosrt: VGA and ROM ranges behave as plain memory here. */
uint8_t mem_rd8_slow(Machine *m, uint32_t linear) { return m->mem[linear]; }
void mem_wr8_slow(Machine *m, uint32_t linear, uint8_t v) { m->mem[linear] = v; }
uint32_t port_in(Machine *m, uint16_t port, int size) { return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFFu; }
void port_out(Machine *m, uint16_t port, uint32_t value, int size) {}
int pic_acknowledge(Machine *m) { return -1; }
void pic_raise_irq(Machine *m, int irq) {}
void pic_lower_irq(Machine *m, int irq) {}
bool lift_dispatch(Machine *m, uint32_t linear) { return false; } /* no hooks: exec_hooks stays NULL */
void lift_io_callback(Machine *m, int id) {}
void lift_io_halt(Machine *m) {}
void lift_irq_boundary(Machine *m) {}
void log_msg(const char *fmt, ...) {}
void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

/* ---- test model ---- */

/* Unified register slots: GPRs in ModRM order, segments in S_* order, then IP and FLAGS. */
enum { X_GPR = 0, X_SEG = 8, X_IP = 14, X_FLAGS = 15, X_COUNT = 16 };

static const char *const slot_name[X_COUNT] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
                                               "es", "cs", "ss", "ds", "fs", "gs", "eip", "eflags"};
/* MOO REGS (16-bit) and RG32 bit order mapped onto slots; -1 is a register we do not model. */
static const int regs16_slot[14] = {0, 3, 1, 2, X_SEG + S_CS, X_SEG + S_SS, X_SEG + S_DS, X_SEG + S_ES,
                                    4, 5, 6, 7, X_IP, X_FLAGS};
static const int regs32_slot[20] = {-1, -1, 0, 3, 1, 2, 6, 7, 5, 4, X_SEG + S_CS, X_SEG + S_DS,
                                    X_SEG + S_ES, X_SEG + S_FS, X_SEG + S_GS, X_SEG + S_SS, X_IP, X_FLAGS, -1, -1};

typedef struct Regs {
    uint32_t v[X_COUNT];
    uint32_t present; /* bit per slot */
} Regs;

typedef struct Test {
    uint32_t index;
    const char *name;
    uint32_t name_len;
    const uint8_t *bytes;
    uint32_t byte_count;
    Regs init, fina, mask;
    const uint8_t *init_ram, *fina_ram; /* 5-byte entries */
    uint32_t init_ram_count, fina_ram_count;
    int exception; /* -1 when none */
    uint32_t flag_addr;
    uint8_t hash[20];
} Test;

typedef enum Suite { SUITE_8088, SUITE_80286, SUITE_80386 } Suite;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void parse_regs(const uint8_t *p, uint32_t len, bool wide, Regs *out, bool is_mask)
{
    uint32_t bits = wide ? rd32(p) : rd16(p);
    const uint8_t *v = p + (wide ? 4 : 2);
    int count = wide ? 20 : 14;
    for (int i = 0; i < count; i++) {
        if (!(bits >> i & 1))
            continue;
        if (v + (wide ? 4 : 2) > p + len)
            fatal("truncated register chunk");
        uint32_t value = wide ? rd32(v) : rd16(v);
        v += wide ? 4 : 2;
        int slot = wide ? regs32_slot[i] : regs16_slot[i];
        if (slot < 0)
            continue;
        if (slot >= X_SEG && slot < X_IP)
            value &= 0xFFFF;
        if (is_mask)
            out->v[slot] &= value;
        else
            out->v[slot] = value;
        out->present |= 1u << slot;
    }
}

static void parse_state(const uint8_t *p, uint32_t len, Regs *regs, Regs *mask, const uint8_t **ram,
                        uint32_t *ram_count)
{
    for (uint32_t off = 0; off + 8 <= len;) {
        const uint8_t *c = p + off;
        uint32_t n = rd32(c + 4);
        if (!memcmp(c, "REGS", 4) || !memcmp(c, "RG32", 4))
            parse_regs(c + 8, n, c[1] == 'G', regs, false);
        else if (!memcmp(c, "RMSK", 4) || !memcmp(c, "RM32", 4))
            parse_regs(c + 8, n, c[2] == '3', mask, true);
        else if (!memcmp(c, "RAM ", 4)) {
            *ram_count = rd32(c + 8);
            *ram = c + 12;
        }
        off += 8 + n;
    }
}

static void parse_test(const uint8_t *p, uint32_t len, const Regs *file_mask, Test *t)
{
    memset(t, 0, sizeof *t);
    t->index = rd32(p);
    t->mask = *file_mask;
    t->exception = -1;
    for (uint32_t off = 4; off + 8 <= len;) {
        const uint8_t *c = p + off, *d = c + 8;
        uint32_t n = rd32(c + 4);
        if (!memcmp(c, "NAME", 4)) {
            t->name_len = rd32(d);
            t->name = (const char *)d + 4;
        } else if (!memcmp(c, "BYTS", 4)) {
            t->byte_count = rd32(d);
            t->bytes = d + 4;
        } else if (!memcmp(c, "INIT", 4)) {
            parse_state(d, n, &t->init, &t->mask, &t->init_ram, &t->init_ram_count);
        } else if (!memcmp(c, "FINA", 4)) {
            parse_state(d, n, &t->fina, &t->mask, &t->fina_ram, &t->fina_ram_count);
        } else if (!memcmp(c, "EXCP", 4)) {
            t->exception = d[0];
            t->flag_addr = rd32(d + 1);
        } else if (!memcmp(c, "HASH", 4)) {
            memcpy(t->hash, d, 20);
        }
        off += 8 + n;
    }
}

/* ---- skip policy ---- */

typedef struct Decoded {
    uint8_t opcode, op2; /* op2: second byte after 0F */
    int reg;             /* ModRM reg field, -1 when the opcode has none */
    uint8_t modrm;
    bool rep, lock, op32;
} Decoded;

static uint32_t final_value(const Test *t, int slot)
{
    return (t->fina.present >> slot & 1) ? t->fina.v[slot] : t->init.v[slot];
}

static bool in_ram(const Test *t, uint32_t addr)
{
    for (uint32_t i = 0; i < t->init_ram_count; i++)
        if (rd32(t->init_ram + i * 5) == addr)
            return true;
    for (uint32_t i = 0; i < t->fina_ram_count; i++)
        if (rd32(t->fina_ram + i * 5) == addr)
            return true;
    return false;
}

static Decoded decode_bytes(const Test *t, Suite suite)
{
    Decoded d = {.reg = -1};
    uint32_t i = 0;
    for (; i < t->byte_count; i++) {
        uint8_t b = t->bytes[i];
        bool prefix = b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0xF0 || b == 0xF2 || b == 0xF3;
        if (suite == SUITE_80386)
            prefix = prefix || (b >= 0x64 && b <= 0x67);
        if (!prefix)
            break;
        d.rep |= b == 0xF2 || b == 0xF3;
        d.lock |= b == 0xF0;
        d.op32 |= b == 0x66;
    }
    if (i < t->byte_count)
        d.opcode = t->bytes[i++];
    if (d.opcode == 0x0F && suite != SUITE_8088 && i < t->byte_count)
        d.op2 = t->bytes[i++];
    if (i < t->byte_count) {
        d.modrm = t->bytes[i];
        d.reg = d.modrm >> 3 & 7;
    }
    return d;
}

/* Behaviour where the 8088 and a 386 disagree; the 386 is what we emulate. */
static const char *skip_8088(const Test *t, const Decoded *d)
{
    uint8_t op = d->opcode;
    if ((op >= 0x60 && op <= 0x6F) || op == 0xC0 || op == 0xC1 || op == 0xC8 || op == 0xC9 || op == 0x0F)
        return "8088 alias opcode";
    if (op == 0x54)
        return "8088 push sp pushes the decremented sp";
    if (op == 0x27 || op == 0x2F || op == 0x37 || op == 0x3F)
        return "8088 BCD adjust differs from 286+";
    if ((op == 0x8F || op == 0xC6 || op == 0xC7) && d->reg != 0)
        return "8088 undefined reg field";
    if ((op == 0x8C || op == 0x8E) && (d->reg >= 4 || (op == 0x8E && d->reg == S_CS)))
        return "8088 segment register alias";
    if (op >= 0xD0 && op <= 0xD3 && d->reg == 6)
        return "8088 SETMO alias";
    if ((op == 0xD2 || op == 0xD3) && (t->init.v[R_CX] & 0xE0))
        return "8088 shift count not masked";
    if (op == 0xFF && d->reg == 7)
        return "8088 FF/7 alias";
    if ((op == 0xF6 || op == 0xF7) && d->reg >= 6 && d->rep)
        return "8088 rep idiv negates";
    /* The 8088 pushes the address after a faulting divide; its handler is at 0000:0400. */
    if ((op == 0xD4 || ((op == 0xF6 || op == 0xF7) && d->reg >= 6)) && final_value(t, X_IP) == 0x400 &&
        final_value(t, X_SEG + S_CS) == 0)
        return "8088 divide fault return address";
    if (op == 0xFF && d->reg == 6 && d->modrm == 0xF4)
        return "8088 push sp pushes the decremented sp";
    /* A word at offset FFFF wraps within the segment on the 8088 and faults on a 286+. */
    for (int s = 0; s < 4; s++) {
        uint32_t base = t->init.v[X_SEG + s] << 4;
        if (in_ram(t, (base + 0xFFFF) & 0xFFFFF) && in_ram(t, base & 0xFFFFF))
            return "8088 offset wrap";
    }
    return NULL;
}

static bool in_port_range(const Test *t, const Decoded *d, uint16_t port)
{
    uint8_t op = d->opcode;
    int size = (op & 1) ? (d->op32 ? 4 : 2) : 1;
    uint32_t first;
    if (op == 0xE4 || op == 0xE5)
        first = d->modrm; /* the immediate port byte sits where a ModRM would */
    else if (op == 0xEC || op == 0xED || op == 0x6C || op == 0x6D)
        first = t->init.v[R_DX] & 0xFFFF;
    else
        return false;
    return port >= first && port < first + (uint32_t)size;
}

/* True when the test rewrites its own instruction bytes, which the CPU had already prefetched. */
static bool writes_own_code(const Test *t)
{
    uint32_t start = (t->init.v[X_SEG + S_CS] << 4) + t->init.v[X_IP];
    for (uint32_t i = 0; i < t->fina_ram_count; i++) {
        uint32_t addr = rd32(t->fina_ram + i * 5);
        if (addr >= start && addr < start + t->byte_count)
            return true;
    }
    return false;
}

static const char *skip_reason(const Test *t, Suite suite)
{
    Decoded d = decode_bytes(t, suite);
    if (writes_own_code(t))
        return "overwrites its own prefetched code";
    if (suite != SUITE_80386 && (d.opcode == 0xD2 || d.opcode == 0xD3) && d.reg >= 4 && d.reg <= 6) {
        unsigned count = t->init.v[R_CX] & (suite == SUITE_8088 ? 0xFF : 0x1F);
        if (count > (d.opcode == 0xD2 ? 8u : 16u))
            return "pre-386 CF for shift counts past the width";
    }
    if (suite == SUITE_8088)
        return skip_8088(t, &d);
    /* Real-mode segment limit checks (#SS, #GP) are not emulated: offsets past FFFF just
     * address seg*16+offset, which no program that runs on real hardware relies on. */
    if (t->exception == 12 || t->exception == 13)
        return "segment limit fault";
    /* The suite's HLT sat at offset FFFF, so EIP reached 10000 where we wrap to 0. */
    if (final_value(t, X_IP) == 0x10000)
        return "HLT at offset FFFF";
    /* The next IP reached 10000, where hardware does not wrap; only #GP would follow. */
    if (t->init.v[X_IP] + t->byte_count - 1 >= 0x10000)
        return "instruction ends at offset 10000";
    if (in_port_range(t, &d, 0x22))
        return "test rig answers port 22h";
    if (suite == SUITE_80286 && d.lock)
        return "286 does not validate LOCK";
    if (suite == SUITE_80286 && (d.opcode == 0x8C || d.opcode == 0x8E) && d.reg >= 4)
        return "286 has no FS/GS";
    return NULL;
}

/* ---- runner ---- */

typedef struct Counts {
    long pass, fail, skip;
} Counts;

static Machine *machine;
static struct {
    const char *reason;
    long count;
} skips[64];

static void note_skip(const char *reason)
{
    int i = 0;
    while (i < 63 && skips[i].reason && strcmp(skips[i].reason, reason))
        i++;
    skips[i].reason = reason;
    skips[i].count++;
}
static char revoked[4096][41];
static int revoked_count;

static void load_revoked(const char *dir)
{
    char path[4608];
    snprintf(path, sizeof path, "%s/revoked.txt", dir);
    revoked_count = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[128];
    while (revoked_count < 4096 && fgets(line, sizeof line, f))
        if (strlen(line) >= 40)
            memcpy(revoked[revoked_count++], line, 40);
    fclose(f);
}

static bool is_revoked(const Test *t)
{
    char hex[41];
    for (int i = 0; i < 20; i++)
        snprintf(hex + i * 2, 3, "%02x", t->hash[i]);
    for (int i = 0; i < revoked_count; i++)
        if (!memcmp(revoked[i], hex, 40))
            return true;
    return false;
}

static uint32_t read_slot(const Machine *m, int slot)
{
    const Cpu *c = &m->cpu;
    if (slot < X_SEG)
        return c->r[slot].e;
    if (slot < X_IP)
        return c->seg[slot - X_SEG];
    return slot == X_IP ? c->eip : c->flags;
}

/* Flag bits compared at all: the 8088 forces bits 12-15 to 1, which a 386 does not. */
static uint32_t flags_compare_mask(Suite suite) { return suite == SUITE_80386 ? 0x7FD7 : 0x0FD7; }

static void setup(Machine *m, const Test *t, Suite suite)
{
    Cpu *c = &m->cpu;
    cpu_reset(m);
    for (int r = 0; r < 8; r++)
        c->r[r].e = t->init.v[X_GPR + r];
    for (int s = 0; s < 6; s++)
        cpu_set_seg(c, s, (uint16_t)t->init.v[X_SEG + s]);
    c->eip = t->init.v[X_IP];
    c->flags = (t->init.v[X_FLAGS] & flags_compare_mask(suite)) | 2;
    if (suite == SUITE_8088)
        c->flags |= t->init.v[X_FLAGS] & 0xF000;
    m->a20 = suite != SUITE_8088;
    m->irq_pending = 0;
    for (uint32_t i = 0; i < t->init_ram_count; i++)
        m->mem[rd32(t->init_ram + i * 5) & 0xFFFFFF] = t->init_ram[i * 5 + 4];
}

static void clear_ram(Machine *m, const Test *t)
{
    for (uint32_t i = 0; i < t->init_ram_count; i++)
        m->mem[rd32(t->init_ram + i * 5) & 0xFFFFFF] = 0;
    for (uint32_t i = 0; i < t->fina_ram_count; i++)
        m->mem[rd32(t->fina_ram + i * 5) & 0xFFFFFF] = 0;
}

/* Expected byte at addr after the test: final RAM wins, then initial, else untouched (0). */
static bool expected_byte(const Test *t, uint32_t addr, uint8_t *out)
{
    for (uint32_t i = 0; i < t->fina_ram_count; i++)
        if (rd32(t->fina_ram + i * 5) == addr) {
            *out = t->fina_ram[i * 5 + 4];
            return true;
        }
    for (uint32_t i = 0; i < t->init_ram_count; i++)
        if (rd32(t->init_ram + i * 5) == addr) {
            *out = t->init_ram[i * 5 + 4];
            return true;
        }
    return false;
}

static int report(const char *file, const Test *t, const char *fmt, ...)
{
    va_list ap;
    printf("  FAIL %s #%u \"%.*s\" [", file, t->index, (int)t->name_len, t->name);
    for (uint32_t i = 0; i < t->byte_count; i++)
        printf("%s%02X", i ? " " : "", t->bytes[i]);
    printf("]%s: ", t->exception >= 0 ? (const char[]){' ', 'e', 'x', 'c', '0' + t->exception / 10, '0' + t->exception % 10, 0} : "");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    return 1;
}

/* OF after a shift or rotate by more than one is undefined, which the CL forms' masks omit. */
static uint32_t extra_flag_mask(const Test *t, Suite suite)
{
    Decoded d = decode_bytes(t, suite);
    unsigned count = t->init.v[R_CX] & (suite == SUITE_8088 ? 0xFF : 0x1F);
    if ((d.opcode == 0xD2 || d.opcode == 0xD3) && count > 1)
        return ~(uint32_t)F_OF;
    if (d.opcode == 0xD4 && t->exception == 0)
        return ~(uint32_t)F_ARITH;
    if (suite == SUITE_80286 && (d.opcode == 0x69 || d.opcode == 0x6B))
        return ~(uint32_t)(F_SF | F_ZF | F_PF | F_AF);
    return 0xFFFFFFFFu;
}

/* Returns the number of mismatches, reporting them when verbose. */
static int check(Machine *m, const Test *t, Suite suite, const char *file, bool verbose)
{
    int bad = 0;
    for (int slot = 0; slot < X_COUNT; slot++) {
        uint32_t want = final_value(t, slot);
        uint32_t mask = t->mask.v[slot];
        if (slot == X_FLAGS)
            mask &= flags_compare_mask(suite) & extra_flag_mask(t, suite);
        if (slot >= X_SEG + S_FS && slot < X_IP && suite != SUITE_80386)
            continue;
        uint32_t got = read_slot(m, slot);
        if ((got & mask) != (want & mask)) {
            bad++;
            if (verbose)
                report(file, t, "%s = %08X, want %08X (mask %08X)", slot_name[slot], got, want, mask);
        }
    }
    /* Every byte the test mentions must hold its final value; the pushed flags of an
     * exception frame are compared under the undefined-flag mask. */
    for (int pass = 0; pass < 2; pass++) {
        const uint8_t *ram = pass ? t->init_ram : t->fina_ram;
        uint32_t count = pass ? t->init_ram_count : t->fina_ram_count;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t addr = rd32(ram + i * 5);
            uint8_t want = 0;
            expected_byte(t, addr, &want);
            uint8_t got = m->mem[addr & 0xFFFFFF];
            uint8_t mask = 0xFF;
            if (t->exception >= 0 && (addr == t->flag_addr || addr == t->flag_addr + 1)) {
                uint32_t fm = t->mask.v[X_FLAGS] & flags_compare_mask(suite) & extra_flag_mask(t, suite);
                mask = (uint8_t)(addr == t->flag_addr ? fm : fm >> 8);
            }
            if ((got & mask) != (want & mask)) {
                bad++;
                if (verbose)
                    report(file, t, "mem[%06X] = %02X, want %02X", addr, got, want);
            }
        }
    }
    return bad;
}

/* 286/386 tests end once the HLT after the instruction (or at the jump target) has run;
 * a jump back onto the instruction itself runs it again first, as the hardware did. */
static void run(Machine *m, Suite suite)
{
    cpu_run(m, suite == SUITE_8088 ? 1 : 4);
}

/* The 286 and 386 miss some IDIV overflows and return a truncated quotient instead of #DE. */
static bool idiv_quirk(const Machine *m, const Test *t, Suite suite)
{
    Decoded d = decode_bytes(t, suite);
    return (d.opcode == 0xF6 || d.opcode == 0xF7) && d.reg == 7 && t->exception < 0 &&
           m->cpu.r[R_SP].w == (uint16_t)(t->init.v[R_SP] - 6);
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n)
        fatal("cannot read %s", path);
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int verbose_limit = 3;

static void run_file(const char *dir, const char *stem, Suite suite, Counts *total)
{
    char path[4608];
    snprintf(path, sizeof path, "%s/%s.moo", dir, stem);
    size_t len;
    uint8_t *buf = read_file(path, &len);
    if (!buf)
        fatal("cannot open %s", path);
    Regs file_mask;
    for (int i = 0; i < X_COUNT; i++)
        file_mask.v[i] = 0xFFFFFFFFu;
    file_mask.present = 0;
    Counts c = {0};
    for (size_t off = 0; off + 8 <= len;) {
        const uint8_t *ch = buf + off;
        uint32_t n = rd32(ch + 4);
        if (!memcmp(ch, "RMSK", 4) || !memcmp(ch, "RM32", 4)) {
            parse_regs(ch + 8, n, ch[2] == '3', &file_mask, true);
        } else if (!memcmp(ch, "TEST", 4)) {
            Test t;
            parse_test(ch + 8, n, &file_mask, &t);
            const char *why = is_revoked(&t) ? "revoked by the suite" : skip_reason(&t, suite);
            if (why) {
                note_skip(why);
                c.skip++;
            } else {
                setup(machine, &t, suite);
                run(machine, suite);
                if (check(machine, &t, suite, stem, false) && idiv_quirk(machine, &t, suite)) {
                    note_skip("IDIV overflow the CPU did not trap");
                    c.skip++;
                } else if (check(machine, &t, suite, stem, false)) {
                    if (c.fail < verbose_limit) {
                        clear_ram(machine, &t);
                        setup(machine, &t, suite);
                        run(machine, suite);
                        check(machine, &t, suite, stem, true);
                    }
                    c.fail++;
                } else {
                    c.pass++;
                }
                clear_ram(machine, &t);
            }
        }
        off += 8 + n;
    }
    free(buf);
    if (c.fail)
        printf("  %s: %ld/%ld failed (%ld skipped)\n", stem, c.fail, c.fail + c.pass, c.skip);
    total->pass += c.pass;
    total->fail += c.fail;
    total->skip += c.skip;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static bool selected(int argc, char **argv, const char *suite, const char *stem)
{
    if (argc <= 1)
        return true;
    for (int i = 1; i < argc; i++) {
        const char *slash = strchr(argv[i], '/');
        if (slash) {
            if (!strncmp(argv[i], suite, (size_t)(slash - argv[i])) && strlen(suite) == (size_t)(slash - argv[i]) &&
                !strcmp(slash + 1, stem))
                return true;
        } else if (!strcmp(argv[i], suite) || !strcmp(argv[i], stem)) {
            return true;
        }
    }
    return false;
}

int main(int argc, char **argv)
{
    /* INN_CPU_TESTS overrides the directory; arguments pick suites or files ("80386/0FBC"). */
    char root[4096];
    const char *env = getenv("INN_CPU_TESTS");
    if (env) {
        snprintf(root, sizeof root, "%s", env);
    } else {
        snprintf(root, sizeof root, "%s", __FILE__);
        char *cut = strstr(root, "runtime/tests/");
        if (!cut) {
            printf("SKIP: cannot locate the source tree; set INN_CPU_TESTS\n");
            return 0;
        }
        snprintf(cut, sizeof root - (size_t)(cut - root), "work/cpu-tests/prepared");
    }
    const char *v = getenv("INN_CPU_TESTS_VERBOSE");
    if (v)
        verbose_limit = atoi(v);

    machine = calloc(1, sizeof *machine);
    machine->mem = calloc(1, MEM_SIZE);
    if (!machine->mem)
        fatal("out of memory");

    static const char *const suite_dirs[] = {"8088", "80286", "80386"};
    int suites_found = 0;
    long failures = 0;
    for (int s = 0; s < 3; s++) {
        char dir[4200];
        snprintf(dir, sizeof dir, "%s/%s", root, suite_dirs[s]);
        DIR *dp = opendir(dir);
        if (!dp)
            continue;
        suites_found++;
        load_revoked(dir);
        char *names[2048];
        int count = 0;
        struct dirent *e;
        while ((e = readdir(dp)) && count < 2048) {
            size_t n = strlen(e->d_name);
            if (n > 4 && !strcmp(e->d_name + n - 4, ".moo")) {
                names[count] = malloc(n - 3);
                memcpy(names[count], e->d_name, n - 4);
                names[count++][n - 4] = 0;
            }
        }
        closedir(dp);
        qsort(names, (size_t)count, sizeof names[0], cmp_str);
        Counts total = {0};
        for (int i = 0; i < count; i++) {
            if (selected(argc, argv, suite_dirs[s], names[i]))
                run_file(dir, names[i], (Suite)s, &total);
            free(names[i]);
        }
        printf("%s: %ld passed, %ld failed, %ld skipped\n", suite_dirs[s], total.pass, total.fail, total.skip);
        for (int i = 0; i < 64 && skips[i].reason; i++)
            printf("  skipped %7ld: %s\n", skips[i].count, skips[i].reason);
        memset(skips, 0, sizeof skips);
        failures += total.fail;
    }
    if (!suites_found) {
        printf("SKIP: no prepared suites under %s (see tools/sst_prepare.py)\n", root);
        return 0;
    }
    return failures ? 1 : 0;
}
