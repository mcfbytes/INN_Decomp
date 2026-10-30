/* Exact emulated time for lifted C.
 *
 * The CPU charges each instruction one slot plus the bus time of its port accesses (cpu.stall) and ends
 * a scheduler slice once its slots are used up; what an instruction takes past the end is dropped.
 * Lifted C runs no instructions, and machine_charge_c keeps its time in a separate account that ends a
 * slice every TICK_SLICE_US, so a lifted call takes a little more time than the original over a long
 * busy loop, and its slice ends, ticks and interrupts fall at other points.
 *
 * A lifted function hooked from the CPU can instead charge the original's path instruction by
 * instruction: lift_time_begin at the entry (it counts the hooked instruction), lift_time_insns for
 * the instructions after it (the bus time of a port access made just before joins the first of them,
 * as the CPU charges an IN or OUT), lift_time_end before returning. Slices then end where the CPU's
 * would, and interrupts that come due are taken at the instruction boundary the CPU would take them at,
 * when the caller's IF is set and the call says interrupts are on there (lift_time_insns' irqs). The
 * handler runs as a nested guest call; its sentinel return costs one slot the original does not run,
 * which is taken back.
 *
 * Exact mode needs the scheduler state a hook leaves (machine_step's slice accounting); a call made
 * directly from other lifted C, a shadow or synthetic run, native mode or a slice already out of step
 * fall back to machine_charge_c (lift_time_begin returns false). */
#ifndef DOSRT_LIFT_TIME_H
#define DOSRT_LIFT_TIME_H

#include <stdbool.h>
#include <stdint.h>

#include "machine.h"

typedef struct LiftTime {
    bool exact;
    bool absorb;       /* the sentinel ended the slice: the next instruction belongs to the old one */
    uint32_t pos;      /* slots used in the current slice */
    uint32_t slice;    /* slots per slice */
    uint64_t outer;    /* slots machine_step's cpu_run reported before the hook */
    uint64_t saved_io; /* machine_charge_c's account, set aside meanwhile */
} LiftTime;

bool lift_time_begin(Machine *m, LiftTime *t);
/* n instructions of the original's path; irqs: interrupts are on at their boundaries (the caller's
 * IF permitting). */
void lift_time_insns(Machine *m, LiftTime *t, uint32_t n, bool irqs);
void lift_time_end(Machine *m, LiftTime *t);

#endif
