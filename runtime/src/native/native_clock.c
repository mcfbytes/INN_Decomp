/* The native clock (docs/native.md section 5): m->virtual_us advances by work charges and waits, and
 * in real-time mode is paced against the platform's wall clock. */
#include "host_audio.h"
#include "native_internal.h"

static bool deterministic(const NativeState *n)
{
    return n->cfg.deterministic || !n->platform;
}

uint64_t native_wall_us(NativeState *n)
{
    return n->platform ? n->platform->now_us(n->platform->ctx) : 0;
}

void native_reanchor(Machine *m)
{
    NativeState *n = native_state(m);
    n->anchor_wall_us = native_wall_us(n);
    n->anchor_virtual_us = m->virtual_us;
}

void native_charge(Machine *m, uint32_t ns)
{
    NativeState *n = native_state(m);
    uint64_t total = (uint64_t)n->sub_us_ns + ns;
    m->virtual_us += total / 1000;
    n->sub_us_ns = (uint32_t)(total % 1000);
}

static void set_clock(Machine *m, uint64_t virtual_us)
{
    if (virtual_us > m->virtual_us) {
        m->virtual_us = virtual_us;
        native_state(m)->sub_us_ns = 0;
    }
}

static bool quit_requested(const NativeState *n)
{
    return n->platform && n->platform->quit_requested && n->platform->quit_requested(n->platform->ctx);
}

/* Lets emulated time run on towards until, at most one wait slice: a jump when deterministic, else a
 * sleep that keeps the clock with the wall clock while the platform polls input and the link. */
static void wait_slice(Machine *m, uint64_t until)
{
    NativeState *n = native_state(m);
    if (deterministic(n)) {
        uint64_t slice_end = m->virtual_us + NATIVE_WAIT_SLICE_US;
        set_clock(m, until < slice_end ? until : slice_end);
        if (n->headless_hook)
            n->headless_hook(m);
        if (n->platform && m->virtual_us - n->last_yield_us >= n->cfg.yield_every_us)
            native_yield(m);
        return;
    }
    uint64_t wall = native_wall_us(n);
    uint64_t paced = n->anchor_virtual_us + (wall - n->anchor_wall_us);
    if (paced < until) {
        uint64_t sleep = until - paced;
        n->platform->sleep_us(n->platform->ctx, sleep < NATIVE_WAIT_SLICE_US ? sleep : NATIVE_WAIT_SLICE_US);
        if (n->platform->poll_events)
            n->platform->poll_events(n->platform->ctx, m);
        wall = native_wall_us(n);
        paced = n->anchor_virtual_us + (wall - n->anchor_wall_us);
    }
    if (paced > until + NATIVE_BEHIND_US)
        n->anchor_wall_us = wall - (until - n->anchor_virtual_us); /* slow host: drop the lost time */
    set_clock(m, paced < until ? paced : until);
    if (wall - n->last_present_us >= NATIVE_PRESENT_US)
        native_yield(m);
}

/* Emulated time of the next tick, not counting ticks already overdue (they wait for delivery), or of
 * the end of a Sound Blaster block when that comes first. */
static uint64_t next_tick_us(Machine *m)
{
    const NativeState *n = native_state(m);
    uint64_t tick = (n->timer_due_ns + 999) / 1000, block = host_audio_next_due_us(m);
    return block < tick ? block : tick;
}

void native_wait_until(Machine *m, uint64_t virtual_us)
{
    NativeState *n = native_state(m);
    while (m->virtual_us < virtual_us && !quit_requested(n)) {
        if (native_poll_deliver(m))
            continue;
        uint64_t tick = next_tick_us(m);
        wait_slice(m, tick < virtual_us ? tick : virtual_us);
    }
}

void native_idle(Machine *m)
{
    NativeState *n = native_state(m);
    uint32_t flags = m->cpu.flags;
    m->cpu.flags |= F_IF;
    n->woken = false;
    while (!native_poll_deliver(m) && !n->woken && !quit_requested(n))
        wait_slice(m, next_tick_us(m));
    m->cpu.flags = flags;
}
