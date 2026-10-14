/* Wall-clock pacing for the windowed and browser main loops. */
#include "frame_clock.h"

#include "audio.h"

#include <SDL.h>

enum {
    MAX_FRAME_US = 50000,       /* never try to catch up more than this per frame */
    REPORT_EVERY_US = 5000000,
};

uint64_t frame_clock_now_us(void)
{
    return SDL_GetPerformanceCounter() * 1000000 / SDL_GetPerformanceFrequency();
}

void frame_clock_start(FrameClock *c, const Machine *m, bool report)
{
    *c = (FrameClock){ .report = report };
    c->last_us = c->window_start_us = frame_clock_now_us();
    c->window_instructions = m->cpu.instructions;
}

static void report_window(FrameClock *c, const Machine *m, uint64_t now)
{
    FramePerf *p = &c->window;
    p->wall_us = now - c->window_start_us;
    p->instructions = m->cpu.instructions - c->window_instructions;
    p->dropped_us = p->wall_us > p->emulated_us ? p->wall_us - p->emulated_us : 0;
    double busy = p->busy_us ? (double)p->busy_us : 1.0;
    AudioStats audio = audio_take_stats((Machine *)m);
    log_msg("perf: speed %.3fx real, host busy %.1f%%, executed %.2f MIPS, capacity %.1f MIPS "
            "(guest clock %.0f MIPS), dropped %.0f ms, longest frame %.0f ms, "
            "audio underruns %u, resyncs %u, discarded %.0f ms",
            (double)p->emulated_us / (double)p->wall_us, 100.0 * p->busy_us / (double)p->wall_us,
            (double)p->instructions / (double)p->wall_us, (double)p->instructions / busy,
            (double)m->ips / 1e6, p->dropped_us / 1000.0, p->longest_us / 1000.0, audio.underruns,
            audio.resyncs, audio_rate((Machine *)m) ? audio.dropped_frames * 1000.0 / audio_rate((Machine *)m) : 0.0);
    c->window = (FramePerf){ 0 };
    c->window_start_us = now;
    c->window_instructions = m->cpu.instructions;
}

void frame_clock_run(FrameClock *c, Machine *m)
{
    uint64_t now = frame_clock_now_us(), elapsed = now - c->last_us;
    c->last_us = now;
    if (elapsed > MAX_FRAME_US)
        elapsed = MAX_FRAME_US;
    c->owed_us += (int64_t)elapsed;
    if (c->owed_us > MAX_FRAME_US)
        c->owed_us = MAX_FRAME_US;
    uint64_t before = m->virtual_us;
    if (c->owed_us > 0)
        machine_run_for(m, (uint64_t)c->owed_us);
    uint64_t advanced = m->virtual_us - before;
    c->owed_us -= (int64_t)advanced;
    /* A wait inside the frame (lifted code in a nested call) already kept pace with the wall
     * clock; owing that time back would freeze the machine, and its audio, for as long. */
    if (c->owed_us < -MAX_FRAME_US)
        c->owed_us = -MAX_FRAME_US;
    c->window.emulated_us += advanced;
    uint64_t busy = frame_clock_now_us() - now;
    c->window.busy_us += busy;
    if (busy > c->window.longest_us)
        c->window.longest_us = busy;
    if (c->report && now - c->window_start_us >= REPORT_EVERY_US)
        report_window(c, m, now);
}
