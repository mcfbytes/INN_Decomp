/* Shadow verification of lifted functions: the original runs first (live, I/O recorded), then the
 * lifted C runs from the same snapshot with I/O replayed, and the two end states are compared. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devices.h"
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
    Cpu cpu_before, cpu_original;
    IoEvent log[IO_LOG_MAX];
    int log_len, replay_pos;
    bool io_diverged;
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
        if (!s->verify->mem_before || !s->verify->mem_original || !s->verify->vga_before ||
            !s->verify->vga_original)
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
    const IoEvent *e = next_replay(m, port, size, false);
    return e ? e->value : 0xFFFFFFFFu;
}

void lift_io_replay_out(Machine *m, uint16_t port, uint32_t value, int size)
{
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
    s->bypass_linear = h->linear;
    m->io_mode = IO_RECORD;
    lift_run_nested(m, ret, m->cpu.r[R_SP].w, true);
    m->io_mode = IO_LIVE;
    if (!was_hooked)
        lift_set_hook_bit(m, ret, false);
}

static void run_lifted(Machine *m, ActiveHook *h)
{
    VerifyState *v = verify(m);
    v->replay_pos = 0;
    m->io_mode = IO_REPLAY;
    h->entry->fn(m);
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
    h->shadow_mismatches++;
    verify(m)->mismatches++;
    log_msg("verify: %s %s call %d: %s", h->entry->module, h->entry->name, h->shadow_calls, why);
}

static void compare(Machine *m, ActiveHook *h, uint16_t entry_sp)
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
    uint32_t stack_top = v->cpu_before.seg_base[S_SS] + entry_sp;
    uint32_t dead_lo = stack_top > DEAD_STACK_BYTES ? stack_top - DEAD_STACK_BYTES : 0;
    long at = first_memory_difference(m->mem, v->mem_original, dead_lo, stack_top);
    if (at >= 0) {
        snprintf(why, sizeof why, "memory %05lX lifted %02X original %02X", (unsigned long)at,
                 m->mem[at], v->mem_original[at]);
        report(m, h, why);
        return;
    }
    vga_snapshot_save(m, v->vga_before);
    if (memcmp(v->vga_before, v->vga_original, vga_snapshot_size()) != 0)
        report(m, h, "VGA state (registers, DAC or planes) differs");
}

/* ---- one verified call ---- */

static void restore(Machine *m, const Cpu *cpu, const uint8_t *mem, const uint8_t *vga)
{
    m->cpu = *cpu;
    memcpy(m->mem, mem, SNAPSHOT_BYTES);
    vga_snapshot_load(m, vga);
}

bool lift_verify_call(Machine *m, ActiveHook *h)
{
    LiftState *s = lift_state(m);
    VerifyState *v = s->verify;
    if (!v || s->in_shadow || h->shadow_calls >= v->calls_per_entry)
        return false;
    h->shadow_calls++;
    s->in_shadow = true;
    m->irq_hold = true;
    uint16_t entry_sp = m->cpu.r[R_SP].w;
    v->cpu_before = m->cpu;
    memcpy(v->mem_before, m->mem, SNAPSHOT_BYTES);
    vga_snapshot_save(m, v->vga_before);
    v->log_len = 0;
    v->io_diverged = false;

    run_original(m, h);
    v->cpu_original = m->cpu;
    memcpy(v->mem_original, m->mem, SNAPSHOT_BYTES);
    vga_snapshot_save(m, v->vga_original);

    restore(m, &v->cpu_before, v->mem_before, v->vga_before);
    run_lifted(m, h);
    compare(m, h, entry_sp);

    restore(m, &v->cpu_original, v->mem_original, v->vga_original);
    m->cpu.stop = false;
    m->irq_hold = false;
    s->in_shadow = false;
    return true;
}
