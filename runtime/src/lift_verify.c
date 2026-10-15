/* Shadow verification of lifted functions: the original runs first (live, I/O recorded), then the
 * lifted C runs from the same snapshot with I/O replayed, and the two end states are compared. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "dos.h"
#include "lift_internal.h"

enum {
    SNAPSHOT_BYTES = MEM_ONE_MEG + 0x10000,
    IO_LOG_MAX = 65536,
    DEAD_STACK_BYTES = 0x2000, /* below the entry SP: locals of the call, dead after return */
    MAX_REPORTED_BYTES = 16,
};

typedef struct IoEvent { uint16_t port; uint8_t size; bool is_out; uint32_t value; } IoEvent;

typedef struct VerifyState {
    int calls_per_entry, mismatches;
    uint8_t *mem_before, *mem_original;
    uint8_t *vga_before, *vga_original; /* video memory lives in the VGA device, not m->mem */
    uint8_t *dos_before, *dos_original, *dos_lifted; /* open files and positions live host-side */
    Cpu cpu_before, cpu_original, cpu_lifted;
    uint8_t *mem_lifted, *vga_lifted, *dos_lifted_end; /* region mode: the lifted run comes first */
    IoEvent log[IO_LOG_MAX];
    int log_len, replay_pos;
    bool io_diverged;
    const char *synth_label; /* set while a synthetic call (lift_synth.c) is verified: its call list line */
} VerifyState;

static VerifyState *verify(Machine *m) { return lift_state(m)->verify; }

void lift_verify_enable(Machine *m, int calls_per_entry)
{
    LiftState *s = lift_state(m);
    if (!s->verify) {
        s->verify = calloc(1, sizeof(VerifyState));
        if (!s->verify)
            fatal("out of memory");
        s->verify->mem_before = malloc(SNAPSHOT_BYTES);
        s->verify->mem_original = malloc(SNAPSHOT_BYTES);
        s->verify->vga_before = malloc(vga_snapshot_size());
        s->verify->vga_original = malloc(vga_snapshot_size());
        s->verify->dos_before = malloc(dos_snapshot_size());
        s->verify->dos_original = malloc(dos_snapshot_size());
        s->verify->dos_lifted = malloc(dos_snapshot_size());
        s->verify->mem_lifted = malloc(SNAPSHOT_BYTES);
        s->verify->vga_lifted = malloc(vga_snapshot_size());
        s->verify->dos_lifted_end = malloc(dos_snapshot_size());
        if (!s->verify->mem_before || !s->verify->mem_original || !s->verify->vga_before ||
            !s->verify->vga_original || !s->verify->dos_before || !s->verify->dos_original ||
            !s->verify->dos_lifted)
            fatal("out of memory");
    }
    s->verify->calls_per_entry = calls_per_entry;
}

int lift_verify_mismatches(const Machine *m)
{
    const LiftState *s = m->lift;
    return s && s->verify ? s->verify->mismatches : 0;
}

/* ---- I/O record and replay ---- */

void lift_io_record(Machine *m, uint16_t port, int size, uint32_t value, bool is_out)
{
    if (lift_replay_active(m)) {
        lift_replay_io_record(m, port, size, value, is_out);
        return;
    }
    VerifyState *v = verify(m);
    if (v->log_len < IO_LOG_MAX)
        v->log[v->log_len++] = (IoEvent){ port, (uint8_t)size, is_out, value };
    else
        v->io_diverged = true;
}

static const IoEvent *next_replay(Machine *m, uint16_t port, int size, bool is_out)
{
    VerifyState *v = verify(m);
    if (v->replay_pos >= v->log_len) {
        v->io_diverged = true;
        return NULL;
    }
    const IoEvent *e = &v->log[v->replay_pos++];
    if (e->port != port || e->size != size || e->is_out != is_out)
        v->io_diverged = true;
    return e;
}

uint32_t lift_io_replay_in(Machine *m, uint16_t port, int size)
{
    if (lift_replay_active(m))
        return lift_replay_io_in(m, port, size);
    const IoEvent *e = next_replay(m, port, size, false);
    return e ? e->value : 0xFFFFFFFFu;
}

void lift_io_replay_out(Machine *m, uint16_t port, uint32_t value, int size)
{
    if (lift_replay_active(m)) {
        lift_replay_io_out(m, port, value, size);
        return;
    }
    const IoEvent *e = next_replay(m, port, size, true);
    if (e && e->value != value)
        verify(m)->io_diverged = true;
}

/* ---- the two runs ---- */

static uint32_t return_linear(Machine *m, const LiftedEntry *e)
{
    uint16_t ip = stack_arg16(m, 0);
    uint16_t cs = e->near_return ? m->cpu.seg[S_CS] : stack_arg16(m, 2);
    return real_addr(cs, ip);
}

/* Interprets the original until it returns; a temporary hook marks the return address. */
static void run_original(Machine *m, ActiveHook *h)
{
    LiftState *s = lift_state(m);
    uint32_t ret = return_linear(m, h->entry);
    bool was_hooked = lift_hooked(m, ret);
    lift_set_hook_bit(m, ret, true);
    s->hooks_off = true; /* lifted callees run original too, so the whole call tree is compared */
    m->io_mode = IO_RECORD;
    lift_run_nested(m, ret, m->cpu.r[R_SP].w, true);
    m->io_mode = IO_LIVE;
    s->hooks_off = false;
    if (!was_hooked)
        lift_set_hook_bit(m, ret, false);
}

static void run_lifted(Machine *m, ActiveHook *h)
{
    VerifyState *v = verify(m);
    v->replay_pos = 0;
    m->io_mode = IO_REPLAY;
    lift_call_c(m, h->entry->fn);
    m->io_mode = IO_LIVE;
    if (v->replay_pos != v->log_len)
        v->io_diverged = true;
}

/* ---- comparison ---- */

static bool registers_match(const Cpu *a, const Cpu *b, uint16_t scratch, char *why, size_t cap)
{
    static const char *const GP[8] = { "AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI" };
    static const char *const SEG[6] = { "ES", "CS", "SS", "DS", "FS", "GS" };
    for (int r = 0; r < 8; r++)
        if (!(scratch >> r & 1) && a->r[r].w != b->r[r].w) {
            snprintf(why, cap, "%s lifted %04X original %04X", GP[r], a->r[r].w, b->r[r].w);
            return false;
        }
    for (int s = 0; s < 6; s++)
        if (!(scratch >> (8 + s) & 1) && a->seg[s] != b->seg[s]) {
            snprintf(why, cap, "%s lifted %04X original %04X", SEG[s], a->seg[s], b->seg[s]);
            return false;
        }
    if ((a->eip & 0xFFFF) != (b->eip & 0xFFFF)) {
        snprintf(why, cap, "IP lifted %04X original %04X", a->eip & 0xFFFF, b->eip & 0xFFFF);
        return false;
    }
    if (!(scratch & LIFT_REG_FLAGS) && (a->flags & F_ARITH) != (b->flags & F_ARITH)) {
        snprintf(why, cap, "flags lifted %04X original %04X", a->flags & F_ARITH, b->flags & F_ARITH);
        return false;
    }
    return true;
}

/* First differing byte outside the dead stack below the entry SP, or -1. */
static long first_memory_difference(const uint8_t *a, const uint8_t *b, uint32_t dead_lo, uint32_t dead_hi)
{
    for (uint32_t i = 0; i < SNAPSHOT_BYTES; i++)
        if (a[i] != b[i] && (i < dead_lo || i >= dead_hi))
            return (long)i;
    return -1;
}

static void report(Machine *m, ActiveHook *h, const char *why)
{
    VerifyState *v = verify(m);
    v->mismatches++;
    if (v->synth_label) {
        h->stats->synth_mismatches++;
        log_msg("verify: %s %s synthetic call %d (%s): %s", h->entry->module, h->entry->name,
                h->stats->synth_calls, v->synth_label, why);
        return;
    }
    h->stats->shadow_mismatches++;
    log_msg("verify: %s %s call %d: %s", h->entry->module, h->entry->name, h->stats->shadow_calls, why);
}

/* AX and DX are scratch for C functions, but they also carry the return value, so a scratch
 * difference is counted (not a mismatch: a void function leaves whatever it last computed there)
 * and the first one per entry is logged. */
static void note_result_difference(Machine *m, ActiveHook *h)
{
    const Cpu *a = &m->cpu, *b = &verify(m)->cpu_original;
    uint16_t scratch = h->entry->scratch_regs;
    bool ax = (scratch & LIFT_REG_AX) && a->r[R_AX].w != b->r[R_AX].w;
    bool dx = (scratch & LIFT_REG_DX) && a->r[R_DX].w != b->r[R_DX].w;
    if (!ax && !dx)
        return;
    if (verify(m)->synth_label) {
        if (h->stats->synth_result_diffs++ == 0)
            log_msg("verify: %s %s synthetic call %d (%s): result DX:AX lifted %04X:%04X original %04X:%04X "
                    "(scratch)", h->entry->module, h->entry->name, h->stats->synth_calls, verify(m)->synth_label,
                    a->r[R_DX].w, a->r[R_AX].w, b->r[R_DX].w, b->r[R_AX].w);
        return;
    }
    if (h->stats->result_diffs++ == 0)
        log_msg("verify: %s %s call %d: result DX:AX lifted %04X:%04X original %04X:%04X (scratch)",
                h->entry->module, h->entry->name, h->stats->shadow_calls, a->r[R_DX].w, a->r[R_AX].w, b->r[R_DX].w,
                b->r[R_AX].w);
}

/* Memory below hi_sp, down to 0x2000 below lo_sp but not below SS:0, is dead stack: a lift whose
 * locals wrap below the stack segment (into static data of a small DGROUP stack) is caught. */
static void compare(Machine *m, ActiveHook *h, uint16_t lo_sp, uint16_t hi_sp)
{
    VerifyState *v = verify(m);
    char why[160];
    if (v->io_diverged) {
        report(m, h, "port I/O sequence differs");
        return;
    }
    if (!registers_match(&m->cpu, &v->cpu_original, h->entry->scratch_regs, why, sizeof why)) {
        report(m, h, why);
        return;
    }
    uint32_t stack_top = v->cpu_before.seg_base[S_SS] + hi_sp, stack_low = v->cpu_before.seg_base[S_SS] + lo_sp;
    uint32_t ss_base = v->cpu_before.seg_base[S_SS];
    uint32_t dead_lo = stack_low > ss_base + DEAD_STACK_BYTES ? stack_low - DEAD_STACK_BYTES : ss_base;
    long at = first_memory_difference(m->mem, v->mem_original, dead_lo, stack_top);
    if (at >= 0) {
        snprintf(why, sizeof why, "memory %05lX lifted %02X original %02X", (unsigned long)at,
                 m->mem[at], v->mem_original[at]);
        report(m, h, why);
        return;
    }
    vga_snapshot_save(m, v->vga_before);
    if (memcmp(v->vga_before, v->vga_original, vga_snapshot_size()) != 0) {
        report(m, h, "VGA state (registers, DAC or planes) differs");
        return;
    }
    dos_snapshot_save(m, v->dos_lifted);
    if (memcmp(v->dos_lifted, v->dos_original, dos_snapshot_size()) != 0) {
        report(m, h, "DOS state (open files, positions, current directory) differs");
        return;
    }
    note_result_difference(m, h);
}

/* ---- one verified call ---- */

static void restore(Machine *m, const Cpu *cpu, const uint8_t *mem, const uint8_t *vga, const uint8_t *dos)
{
    m->cpu = *cpu;
    memcpy(m->mem, mem, SNAPSHOT_BYTES);
    vga_snapshot_load(m, vga);
    dos_snapshot_load(m, dos);
}

/* ---- region mode ----
 * A LIFT_REGION entry hands control back at an instruction of its choosing instead of returning.
 * The lifted run goes first (I/O recorded) and fixes the exit CS:IP and SP; the original then runs
 * from the same snapshot with every hook off until it first reaches that CS:IP with that SP. */

enum { REGION_BUDGET_INSNS = 400000000 };

static bool run_original_to(Machine *m, const Cpu *exit)
{
    LiftState *s = lift_state(m);
    VerifyState *v = verify(m);
    uint32_t at = exit->seg_base[S_CS] + (exit->eip & 0xFFFF);
    bool was_hooked = lift_hooked(m, at);
    lift_set_hook_bit(m, at, true);
    s->hooks_off = true;
    v->replay_pos = 0;
    m->io_mode = IO_REPLAY;
    NestedCall *n = &s->nested[s->nesting++];
    *n = (NestedCall){ .return_linear = at, .return_sp = exit->r[R_SP].w };
    uint64_t start = m->cpu.instructions;
    while (!n->returned && !m->exit_requested && m->cpu.instructions - start < REGION_BUDGET_INSNS)
        machine_step(m);
    bool reached = n->returned;
    s->nesting--;
    m->io_mode = IO_LIVE;
    s->hooks_off = false;
    if (!was_hooked)
        lift_set_hook_bit(m, at, false);
    if (v->replay_pos != v->log_len)
        v->io_diverged = true;
    return reached;
}

static void save(Machine *m, Cpu *cpu, uint8_t *mem, uint8_t *vga, uint8_t *dos)
{
    *cpu = m->cpu;
    memcpy(mem, m->mem, SNAPSHOT_BYTES);
    vga_snapshot_save(m, vga);
    dos_snapshot_save(m, dos);
}

static void restore(Machine *m, const Cpu *cpu, const uint8_t *mem, const uint8_t *vga, const uint8_t *dos);
static void compare(Machine *m, ActiveHook *h, uint16_t lo_sp, uint16_t hi_sp);

static void verify_region(Machine *m, ActiveHook *h)
{
    VerifyState *v = verify(m);
    uint16_t entry_sp = m->cpu.r[R_SP].w;
    m->io_mode = IO_RECORD;
    lift_call_c(m, h->entry->fn);
    m->io_mode = IO_LIVE;
    save(m, &v->cpu_lifted, v->mem_lifted, v->vga_lifted, v->dos_lifted_end);
    if (m->exit_requested)
        return;
    restore(m, &v->cpu_before, v->mem_before, v->vga_before, v->dos_before);
    dos_journal_undo(m);
    bool reached = run_original_to(m, &v->cpu_lifted);
    save(m, &v->cpu_original, v->mem_original, v->vga_original, v->dos_original);
    restore(m, &v->cpu_lifted, v->mem_lifted, v->vga_lifted, v->dos_lifted_end);
    m->cpu.stop = false;
    if (!reached) {
        char why[160];
        snprintf(why, sizeof why, "original never reached the lifted exit %04X:%04X SP %04X",
                 v->cpu_lifted.seg[S_CS], v->cpu_lifted.eip & 0xFFFF, v->cpu_lifted.r[R_SP].w);
        report(m, h, why);
        return;
    }
    uint16_t exit_sp = v->cpu_lifted.r[R_SP].w;
    int before = v->mismatches;
    compare(m, h, exit_sp < entry_sp ? exit_sp : entry_sp, exit_sp);
    if (v->mismatches != before)
        log_msg("verify:   region entry %04X:%04X SP %04X -> exit %04X:%04X SP %04X",
                v->cpu_before.seg[S_CS], v->cpu_before.eip & 0xFFFF, entry_sp,
                v->cpu_lifted.seg[S_CS], v->cpu_lifted.eip & 0xFFFF, exit_sp);
}

/* ---- device replay: the original runs with time and interrupts flowing (lift_replay.c) ---- */

static void verify_replayed(Machine *m, ActiveHook *h, uint16_t entry_sp)
{
    VerifyState *v = verify(m);
    h->stats->replay_calls++;
    lift_replay_record_begin(m);
    run_original(m, h);
    lift_replay_record_end(m);
    save(m, &v->cpu_original, v->mem_original, v->vga_original, v->dos_original);
    restore(m, &v->cpu_before, v->mem_before, v->vga_before, v->dos_before);
    dos_journal_undo(m);
    m->io_mode = IO_REPLAY;
    bool same_events = lift_replay_play(m, h->entry->fn);
    m->io_mode = IO_LIVE;
    int before = v->mismatches;
    if (!same_events) {
        char why[200];
        snprintf(why, sizeof why, "device replay: %s", lift_replay_why(m));
        report(m, h, why);
    } else {
        compare(m, h, entry_sp, entry_sp);
    }
    if (v->mismatches != before) {
        int irqs, late;
        size_t entries;
        lift_replay_counts(m, &irqs, &late, &entries);
        log_msg("verify:   replay log %zu entries, %d interrupts (%d taken without a preceding event)", entries,
                irqs, late);
    }
    restore(m, &v->cpu_original, v->mem_original, v->vga_original, v->dos_original);
}

static bool replayed(const LiftState *s, const LiftedEntry *e)
{
    return e->replay || (s->verify_replay && lift_listed(s->verify_replay, e->name));
}

bool lift_verify_call(Machine *m, ActiveHook *h)
{
    LiftState *s = lift_state(m);
    VerifyState *v = s->verify;
    if (!v || s->in_shadow || h->stats->shadow_calls >= v->calls_per_entry)
        return false;
    if (h->entry->no_shadow && !replayed(s, h->entry))
        return false;
    if (s->verify_skip && lift_listed(s->verify_skip, h->entry->name))
        return false;
    h->stats->shadow_calls++;
    /* Loading an overlay during the runs re-sorts the active hooks under h. */
    ActiveHook hook = *h;
    h = &hook;
    s->in_shadow = true;
    s->shadow_of = h;
    m->irq_hold = true;
    uint16_t entry_sp = m->cpu.r[R_SP].w;
    v->cpu_before = m->cpu;
    memcpy(v->mem_before, m->mem, SNAPSHOT_BYTES);
    vga_snapshot_save(m, v->vga_before);
    dos_snapshot_save(m, v->dos_before);
    dos_journal_begin(m);
    v->log_len = 0;
    v->io_diverged = false;

    if (h->entry->region) {
        verify_region(m, h);
        dos_journal_end(m);
        m->cpu.stop = false;
        m->irq_hold = false;
        s->in_shadow = false;
        s->shadow_of = NULL;
        return true;
    }
    if (replayed(s, h->entry)) {
        verify_replayed(m, h, entry_sp);
        dos_journal_end(m);
        m->cpu.stop = false;
        m->irq_hold = false;
        s->in_shadow = false;
        s->shadow_of = NULL;
        return true;
    }
    run_original(m, h);
    v->cpu_original = m->cpu;
    memcpy(v->mem_original, m->mem, SNAPSHOT_BYTES);
    vga_snapshot_save(m, v->vga_original);
    dos_snapshot_save(m, v->dos_original);

    restore(m, &v->cpu_before, v->mem_before, v->vga_before, v->dos_before);
    dos_journal_undo(m);
    run_lifted(m, h);
    compare(m, h, entry_sp, entry_sp);

    restore(m, &v->cpu_original, v->mem_original, v->vga_original, v->dos_original);
    dos_journal_end(m);
    m->cpu.stop = false;
    m->irq_hold = false;
    s->in_shadow = false;
    s->shadow_of = NULL;
    return true;
}

/* A synthetic call (lift_synth.c): the machine is set up with CS:IP at h's entry and the return
 * address pushed. Both runs start from that state, are compared as a live call is, and the machine
 * is left in that start state again, host file changes undone. */
SynthResult lift_verify_synthetic(Machine *m, ActiveHook *h, const char *label, uint64_t budget)
{
    LiftState *s = lift_state(m);
    VerifyState *v = s->verify;
    SynthResult r = { 0 };
    h->stats->synth_calls++;
    v->synth_label = label;
    s->in_shadow = true;
    s->shadow_of = h;
    m->irq_hold = true;
    uint16_t entry_sp = m->cpu.r[R_SP].w;
    save(m, &v->cpu_before, v->mem_before, v->vga_before, v->dos_before);
    dos_journal_begin(m);
    v->log_len = 0;
    v->io_diverged = false;
    int before = v->mismatches;

    s->nested_budget = m->cpu.instructions + budget;
    run_original(m, h);
    s->nested_budget = 0;
    bool ended = m->exit_requested, returned = !s->nested_abort && !ended;
    s->nested_abort = false;
    save(m, &v->cpu_original, v->mem_original, v->vga_original, v->dos_original);
    r.ax = v->cpu_original.r[R_AX].w;
    r.dx = v->cpu_original.r[R_DX].w;
    uint32_t at = real_addr(r.dx, r.ax);
    if (r.dx == 0xFFFF && at >= MEM_ONE_MEG && at + sizeof r.at_result <= SNAPSHOT_BYTES) {
        memcpy(r.at_result, v->mem_original + at, sizeof r.at_result);
        r.has_bytes = true;
    }
    restore(m, &v->cpu_before, v->mem_before, v->vga_before, v->dos_before);
    dos_journal_undo(m);
    m->exit_requested = false;
    if (returned) {
        run_lifted(m, h);
        compare(m, h, entry_sp, entry_sp);
    } else {
        char why[96];
        snprintf(why, sizeof why, "the original did not return (%s)",
                 ended ? "the process ended" : "instruction budget spent");
        report(m, h, why);
    }
    r.returned = returned;
    r.mismatch = v->mismatches != before;
    m->exit_requested = false;
    restore(m, &v->cpu_before, v->mem_before, v->vga_before, v->dos_before);
    dos_journal_undo(m);
    dos_journal_end(m);
    m->cpu.stop = false;
    m->irq_hold = false;
    s->in_shadow = false;
    s->shadow_of = NULL;
    v->synth_label = NULL;
    return r;
}
