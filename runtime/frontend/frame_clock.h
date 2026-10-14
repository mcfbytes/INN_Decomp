/* Paces the machine against the host wall clock, one call per displayed frame, and optionally
 * reports how much host time emulation takes (docs/web.md, "Performance"). */
#ifndef INN_FRAME_CLOCK_H
#define INN_FRAME_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "machine.h"

typedef struct FramePerf {
    uint64_t wall_us, busy_us, emulated_us, dropped_us, instructions;
} FramePerf;

typedef struct FrameClock {
    uint64_t last_us;
    int64_t owed_us; /* emulated time due to catch up with the wall clock; negative after overshoot */
    bool report;
    uint64_t window_start_us, window_instructions;
    FramePerf window;
} FrameClock;

/* Host wall clock in microseconds. */
uint64_t frame_clock_now_us(void);

void frame_clock_start(FrameClock *c, const Machine *m, bool report);
/* Runs m for the wall time since the previous call, capped so a stalled tab or a slow host
 * slows the game down instead of making it race to catch up. */
void frame_clock_run(FrameClock *c, Machine *m);

#endif
