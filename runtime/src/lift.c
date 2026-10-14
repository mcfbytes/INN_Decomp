/* Lifted-function registry and dispatch. Hooks are armed per loaded module and checked against
 * the original bytes at the entry, so a different build of the same module is never patched. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lift_internal.h"

void lift_set_hook_bit(Machine *m, uint32_t linear, bool on)
{
    if (linear >= MEM_ONE_MEG)
        return;
    if (on)
        m->exec_hooks[linear >> 3] |= (uint8_t)(1u << (linear & 7));
    else
        m->exec_hooks[linear >> 3] &= (uint8_t)~(1u << (linear & 7));
}

/* The far-return sentinel: guest code called by lifted code returns into this stub. */
static CallbackResult far_return_reached(Machine *m)
{
    LiftState *s = lift_state(m);
    if (s->nesting > 0)
        s->nested[s->nesting - 1].returned = true;
    m->cpu.stop = true;
    return CB_DONE;
}

void lift_init(Machine *m)
{
    m->lift = calloc(1, sizeof(LiftState));
    m->exec_hooks = calloc(1, HOOK_BITMAP_BYTES);
    if (!m->lift || !m->exec_hooks)
        fatal("out of memory");
    lift_state(m)->return_callback = callback_register(m, "lift far return", far_return_reached, -1);
    lifted_register_all(m);
    lift_state(m)->only = getenv("INN_LIFT_ONLY");
    const char *verify_calls = getenv("INN_VERIFY_LIFTED");
    if (verify_calls && atoi(verify_calls) > 0)
        lift_verify_enable(m, atoi(verify_calls));
}

static void log_verify_line(const ActiveHook *h)
{
    if (h->entry->no_shadow) {
        log_msg("verify: %s %s: not shadowed (LIFT_NOSHADOW)", h->entry->module, h->entry->name);
        return;
    }
    log_msg("verify: %s %s: %d calls checked, %d mismatches", h->entry->module, h->entry->name,
            h->shadow_calls, h->shadow_mismatches);
}

static void log_verify_summary(LiftState *s)
{
    for (int i = 0; i < s->active_count; i++)
        log_verify_line(&s->active[i]);
}

void lift_shutdown(Machine *m)
{
    if (m->lift && lift_state(m)->verify)
        log_verify_summary(lift_state(m));
    if (m->lift)
        free(lift_state(m)->verify);
    free(m->lift);
    free(m->exec_hooks);
    m->lift = NULL;
    m->exec_hooks = NULL;
}

void lift_add_table(Machine *m, const LiftedEntry *entries, int count)
{
    LiftState *s = lift_state(m);
    if (s->table_count >= MAX_TABLES)
        fatal("too many lifted-function tables");
    s->tables[s->table_count].entries = entries;
    s->tables[s->table_count].count = count;
    s->table_count++;
}

static int by_linear(const void *a, const void *b)
{
    const ActiveHook *x = a, *y = b;
    return x->linear < y->linear ? -1 : x->linear > y->linear;
}

/* Whether name is one of the comma-separated names in list. */
static bool listed(const char *list, const char *name)
{
    size_t len = strlen(name);
    for (const char *p = list; (p = strstr(p, name)); p += len)
        if ((p == list || p[-1] == ',') && (p[len] == ',' || p[len] == 0))
            return true;
    return false;
}

static void arm(Machine *m, const LiftedEntry *e, uint16_t load_seg)
{
    LiftState *s = lift_state(m);
    if (s->only && !listed(s->only, e->name))
        return;
    if (s->active_count >= MAX_ACTIVE)
        fatal("too many active lifted functions");
    uint32_t linear = real_addr((uint16_t)(load_seg + e->seg), e->off);
    s->active[s->active_count++] = (ActiveHook){ .linear = linear, .load_seg = load_seg, .entry = e };
    lift_set_hook_bit(m, linear, true);
}

static void remember_module(LiftState *s, const char *module, uint16_t load_seg)
{
    int i = 0;
    while (i < s->module_count && strcmp(s->modules[i].name, module) != 0)
        i++;
    if (i == s->module_count) {
        if (s->module_count == (int)(sizeof s->modules / sizeof s->modules[0]))
            return;
        s->module_count++;
        snprintf(s->modules[i].name, sizeof s->modules[i].name, "%s", module);
    }
    s->modules[i].load_seg = load_seg;
}

uint16_t lift_module_base(Machine *m, const char *module)
{
    LiftState *s = lift_state(m);
    for (int i = 0; s && i < s->module_count; i++)
        if (!strcmp(s->modules[i].name, module))
            return s->modules[i].load_seg;
    return 0;
}

void lift_module_loaded(Machine *m, const char *module, uint16_t load_seg, uint32_t image_bytes)
{
    LiftState *s = lift_state(m);
    if (!s)
        return;
    remember_module(s, module, load_seg);
    if (m->trace)
        log_msg("lift: module %s at %04X (%u bytes)", module, load_seg, image_bytes);
    lift_module_unloaded(m, load_seg, (image_bytes + 15) / 16);
    for (int t = 0; t < s->table_count; t++)
        for (int i = 0; i < s->tables[t].count; i++)
            if (!strcmp(s->tables[t].entries[i].module, module))
                arm(m, &s->tables[t].entries[i], load_seg);
    qsort(s->active, (size_t)s->active_count, sizeof s->active[0], by_linear);
}

void lift_module_unloaded(Machine *m, uint16_t load_seg, uint32_t paragraphs)
{
    LiftState *s = lift_state(m);
    if (!s)
        return;
    uint32_t lo = real_addr(load_seg, 0), hi = lo + paragraphs * 16;
    int kept = 0;
    for (int i = 0; i < s->active_count; i++) {
        ActiveHook *h = &s->active[i];
        if (h->linear >= lo && h->linear < hi)
            lift_set_hook_bit(m, h->linear, false);
        else
            s->active[kept++] = *h;
    }
    s->active_count = kept;
}

static ActiveHook *find_hook(LiftState *s, uint32_t linear)
{
    ActiveHook key = { .linear = linear };
    return bsearch(&key, s->active, (size_t)s->active_count, sizeof s->active[0], by_linear);
}

/* The signature as it sits in memory once the loader added the load segment to relocated words. */
static void expected_bytes(const ActiveHook *h, uint8_t out[LIFT_SIGNATURE_LEN], bool skip[LIFT_SIGNATURE_LEN])
{
    const LiftedEntry *e = h->entry;
    memcpy(out, e->signature, LIFT_SIGNATURE_LEN);
    memset(skip, 0, LIFT_SIGNATURE_LEN);
    skip[0] = (e->reloc_mask & LIFT_RELOC_SPLIT_HEAD) != 0;
    for (int i = 0; i < LIFT_SIGNATURE_LEN; i++) {
        if (!(e->reloc_mask >> i & 1))
            continue;
        uint16_t word = (uint16_t)(e->signature[i] | (i + 1 < LIFT_SIGNATURE_LEN ? e->signature[i + 1] << 8 : 0));
        word = (uint16_t)(word + h->load_seg);
        out[i] = (uint8_t)word;
        if (i + 1 < LIFT_SIGNATURE_LEN)
            out[i + 1] = (uint8_t)(word >> 8);
    }
}

static bool signature_matches(Machine *m, ActiveHook *h)
{
    if (!h->verified) {
        h->verified = true;
        uint8_t expected[LIFT_SIGNATURE_LEN];
        bool skip[LIFT_SIGNATURE_LEN];
        expected_bytes(h, expected, skip);
        for (int i = 0; i < LIFT_SIGNATURE_LEN; i++)
            if (!skip[i] && mem_rd8(m, h->linear + (uint32_t)i) != expected[i])
                h->rejected = true;
        if (h->rejected)
            log_msg("lift: %s %s bytes differ at the entry; running the original", h->entry->module,
                    h->entry->name);
    }
    return !h->rejected;
}

/* A near return into a hooked entry with the expected SP ends the innermost nested call. */
static bool nested_return_reached(Machine *m, LiftState *s, uint32_t linear)
{
    if (s->nesting == 0)
        return false;
    NestedCall *n = &s->nested[s->nesting - 1];
    uint16_t sp = m->cpu.r[R_SP].w;
    if (n->return_linear != linear || (n->sp_above ? sp <= n->return_sp : sp != n->return_sp))
        return false;
    n->returned = true;
    m->cpu.stop = true;
    return true;
}

bool lift_dispatch(Machine *m, uint32_t linear)
{
    LiftState *s = lift_state(m);
    if (nested_return_reached(m, s, linear))
        return true;
    if (s->hooks_off)
        return false;
    ActiveHook *h = find_hook(s, linear);
    if (!h || !signature_matches(m, h))
        return false;
    if (!lift_verify_call(m, h))
        h->entry->fn(m);
    return true;
}

void lift_run_nested(Machine *m, uint32_t return_linear, uint16_t return_sp, bool sp_above)
{
    LiftState *s = lift_state(m);
    if (s->nesting >= MAX_NESTING)
        fatal("lifted code nested too deeply");
    NestedCall *n = &s->nested[s->nesting++];
    *n = (NestedCall){ .return_linear = return_linear, .return_sp = return_sp, .sp_above = sp_above };
    uint64_t start = m->cpu.instructions, stall_report = start + SHADOW_STALL_INSNS;
    while (!n->returned && !m->exit_requested) {
        machine_step(m);
        if (s->shadow_of && m->cpu.instructions >= stall_report) {
            /* Interrupts are held during a shadow run, so code that waits on a timer tick spins. */
            log_msg("verify: shadow run of %s still running after %llu instructions, at %04X:%04X",
                    s->shadow_of->entry->name, (unsigned long long)(m->cpu.instructions - start),
                    m->cpu.seg[S_CS], (unsigned)(m->cpu.eip & 0xFFFF));
            stall_report += SHADOW_STALL_INSNS;
        }
    }
    s->nesting--;
}

void guest_call_far(Machine *m, uint16_t seg, uint16_t off)
{
    LiftState *s = lift_state(m);
    uint16_t saved_cs = m->cpu.seg[S_CS], saved_ip = (uint16_t)m->cpu.eip;
    uint32_t stub = callback_stub_addr(s->return_callback);
    cpu_push16(m, CALLBACK_SEG);
    cpu_push16(m, (uint16_t)(stub - real_addr(CALLBACK_SEG, 0)));
    uint16_t sp_after_return = (uint16_t)(m->cpu.r[R_SP].w + 4);
    cpu_set_seg(&m->cpu, S_CS, seg);
    m->cpu.eip = off;
    lift_run_nested(m, UINT32_MAX, sp_after_return, false); /* the sentinel callback ends it */
    cpu_set_seg(&m->cpu, S_CS, saved_cs);
    m->cpu.eip = saved_ip;
}

/* The return address pushed is the lifted function's own entry, which is already hooked. */
void guest_call_near(Machine *m, uint16_t off)
{
    uint16_t entry_ip = (uint16_t)m->cpu.eip;
    uint32_t entry_linear = m->cpu.seg_base[S_CS] + entry_ip;
    cpu_push16(m, entry_ip);
    uint16_t sp_after_return = (uint16_t)(m->cpu.r[R_SP].w + 2);
    m->cpu.eip = off;
    lift_run_nested(m, entry_linear, sp_after_return, false);
    m->cpu.eip = entry_ip;
}
