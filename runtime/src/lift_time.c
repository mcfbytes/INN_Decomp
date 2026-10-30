/* Exact emulated time for lifted C (lift_time.h). */
#include "lift_time.h"

#include "lift.h"
#include "lift_internal.h"

/* The scheduler state the CPU would show at this point of the original's path. */
static void sync(Machine *m, const LiftTime *t)
{
    m->slice_start_insn = m->cpu.instructions - t->pos;
    m->slice_left = t->slice - t->pos;
}

static bool irq_due(const Machine *m)
{
    return (m->cpu.flags & F_IF) && m->irq_pending && !m->irq_hold && !m->irq_gate && !m->ticks_frozen &&
           !m->native && m->io_mode == IO_LIVE;
}

/* An instruction boundary: interrupts that are due run their handlers, one after another as the CPU
 * takes the next one right after an IRET. */
static void take_irqs(Machine *m, LiftTime *t)
{
    while (!t->absorb && irq_due(m)) {
        int vector = pic_acknowledge(m);
        if (vector < 0)
            return;
        guest_int(m, (uint8_t)vector);
        if (!t->exact)
            continue;
        uint64_t pos = m->cpu.instructions - m->slice_start_insn;
        if (pos == 0) {
            /* The sentinel's slot ended the slice; the original's next instruction would have. */
            t->absorb = true;
            t->pos = 0;
        } else {
            t->pos = (uint32_t)(pos - 1);
        }
        sync(m, t);
    }
}

bool lift_time_begin(Machine *m, LiftTime *t)
{
    *t = (LiftTime){ 0 };
    const LiftState *s = m->lift;
    if (m->native || !m->ips || m->io_mode != IO_LIVE || m->ticks_frozen || !s || s->c_depth != 1)
        return false;
    uint64_t slice = m->ips * TICK_SLICE_US / 1000000;
    uint64_t pos = m->cpu.instructions - m->slice_start_insn;
    uint64_t outer = m->cpu.instructions - m->step_start_insn;
    /* Called from a hook: cpu_run stopped before its budget, and the slice accounting is in step. */
    if (!slice || slice > UINT32_MAX || pos >= slice || outer >= m->slice_left || outer > pos ||
        pos - outer + m->slice_left != slice)
        return false;
    t->exact = true;
    t->slice = (uint32_t)slice;
    t->pos = (uint32_t)pos;
    t->outer = outer;
    t->saved_io = m->c_io_ns;
    m->c_io_ns = 0;
    lift_time_insns(m, t, 1, true); /* the hooked instruction */
    return true;
}

static void end_of_slice(Machine *m, LiftTime *t, bool irqs)
{
    t->pos = 0;
    sync(m, t);
    machine_end_slice(m);
    if (irqs)
        take_irqs(m, t);
}

void lift_time_insns(Machine *m, LiftTime *t, uint32_t n, bool irqs)
{
    if (!t->exact) {
        if (n && m->ips && m->io_mode == IO_LIVE && !m->native)
            machine_charge_c(m, (uint32_t)((uint64_t)n * 1000000000u / m->ips));
        if (irqs)
            take_irqs(m, t);
        return;
    }
    /* Bus time charged since the last call belongs to the first instruction (the IN or OUT). */
    uint64_t stall = m->c_io_ns ? m->ips * m->c_io_ns / 1000000000u : 0;
    m->c_io_ns = 0;
    bool first = true;
    while (n) {
        uint64_t unit;
        if (first) {
            unit = 1 + stall; /* one instruction with its bus cycles: what it takes past the slice is lost */
            n--;
        } else {
            unit = n < t->slice - t->pos ? n : t->slice - t->pos; /* one-slot instructions up to the end */
            n -= (uint32_t)unit;
        }
        if (t->absorb) {
            t->absorb = false; /* the instruction the sentinel stood in for */
            if (first)
                unit = 0;
            else
                unit--;
        }
        if (t->pos + unit >= t->slice) {
            end_of_slice(m, t, irqs);
        } else {
            t->pos += (uint32_t)unit;
            sync(m, t);
            if (first && irqs)
                take_irqs(m, t);
        }
        first = false;
    }
}

void lift_time_end(Machine *m, LiftTime *t)
{
    if (!t->exact)
        return;
    /* machine_step adds the hooked instruction's slot after the call and subtracts what cpu_run
     * reported and that slot from slice_left; the hooked instruction was counted at the entry. */
    m->slice_start_insn = m->cpu.instructions + 1 - t->pos;
    m->slice_left = t->slice - t->pos + t->outer + 1;
    m->c_io_ns = t->saved_io;
}
