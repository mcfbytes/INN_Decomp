/* State of the native core shared by native.c, native_clock.c, native_irq.c and host_timer.c. */
#ifndef DOSRT_NATIVE_INTERNAL_H
#define DOSRT_NATIVE_INTERNAL_H

#include "host_platform.h"
#include "native.h"

enum {
    NATIVE_ROM_LIFTS_MAX = 32,
    NATIVE_PRESENT_US = 16667,  /* wall time between frames presented from a yield */
    NATIVE_AHEAD_US = 10000,    /* pacing: sleep once the clock is this far ahead of the wall clock */
    NATIVE_BEHIND_US = 50000,   /* and drop the lost time once it is this far behind */
    NATIVE_WAIT_SLICE_US = 4000, /* longest sleep of a wait between event polls */
    NATIVE_CHECK_EVERY = 256,   /* safe points between wall-clock checks */
};

typedef struct NativePic {
    uint8_t irr, isr, imr, lines;
} NativePic;

typedef struct NativeRomLift {
    uint32_t linear;
    LiftedFn fn;
    const char *name;
} NativeRomLift;

typedef struct NativeState {
    NativeConfig cfg;
    const HostPlatform *platform;
    void (*headless_hook)(Machine *m);

    /* clock */
    uint32_t sub_us_ns;           /* charges below 1 us not yet added to virtual_us */
    uint64_t anchor_wall_us, anchor_virtual_us; /* pacing: wall and emulated time that correspond */
    uint64_t last_yield_us;       /* wall time (virtual when deterministic) of the last yield */
    uint64_t last_present_us;     /* wall time of the last frame a yield presented */
    uint32_t safepoints;
    bool woken;                   /* native_wake since the current wait began */

    /* interrupts */
    NativePic pic[2];

    /* PIT channel 0 */
    uint16_t divisor;
    uint64_t timer_due_ns;        /* emulated time of the next tick */
    uint32_t timer_due_frac;      /* remainder of timer_due_ns in 1/PIT_HZ ns */
    int timer_pending;            /* overdue ticks not yet raised */
    uint64_t timer_dropped;

    uint8_t port61;

    NativeRomLift rom[NATIVE_ROM_LIFTS_MAX];
    int rom_count;

    /* non-strict fallback: instructions interpreted while native, per 16-byte paragraph */
    uint32_t *interp_profile;
} NativeState;

static inline NativeState *native_state(const Machine *m) { return m->native_state; }

uint64_t native_wall_us(NativeState *n);
/* Clock reference for pacing: emulated time now corresponds to wall time now. */
void native_reanchor(Machine *m);
/* The polls of a safe point (input, link, timer), then one interrupt if IF allows; true when one was
 * delivered. */
bool native_poll_deliver(Machine *m);
/* Safe point work without the charge: timer, input and link polls, one IRQ, the periodic yield. */
void native_poll(Machine *m);
void native_timer_reset(Machine *m);

#endif
