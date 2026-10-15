/* Native core (docs/native.md sections 4-6): mode, safe points, yields, ROM lifts and the rules for
 * guest code that nothing native resolves. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "host_input.h"
#include "host_link.h"
#include "host_timer.h"
#include "host_video.h"
#include "lift_internal.h"
#include "native_internal.h"

enum { DEFAULT_CALL_COST_NS = 1000, DEFAULT_YIELD_EVERY_US = 4000, PORT61_RESET = 0x30 };

static bool env_is(const char *name, const char *value)
{
    const char *v = getenv(name);
    return v && !strcmp(v, value);
}

static NativeConfig config_from_env(void)
{
    NativeConfig c = { .strict = !env_is("INN_NATIVE_STRICT", "0"),
                       .deterministic = env_is("INN_NATIVE_DETERMINISTIC", "1"),
                       .call_cost_ns = DEFAULT_CALL_COST_NS, .yield_every_us = DEFAULT_YIELD_EVERY_US };
    const char *cost = getenv("INN_NATIVE_CALL_NS");
    if (cost && *cost)
        c.call_cost_ns = (uint32_t)strtoul(cost, NULL, 10);
    return c;
}

void native_init(Machine *m, const NativeConfig *cfg)
{
    NativeState *n = m->native_state;
    if (!n) {
        n = calloc(1, sizeof *n);
        if (!n)
            fatal("out of memory");
        m->native_state = n;
        n->port61 = PORT61_RESET;
        native_timer_reset(m);
    }
    n->cfg = cfg ? *cfg : config_from_env();
}

bool native_active(const Machine *m)
{
    return m->native;
}

void native_set_active(Machine *m, bool on)
{
    NativeState *n = native_state(m);
    if (on == m->native)
        return;
    m->native = on;
    if (on) {
        native_reanchor(m);
        if (!n->cfg.strict && !dosrt_interp_profile) {
            if (!n->interp_profile && !(n->interp_profile = calloc(MEM_ONE_MEG / 16, sizeof(uint32_t))))
                fatal("out of memory");
            dosrt_interp_profile = n->interp_profile;
        }
    } else if (n->interp_profile && dosrt_interp_profile == n->interp_profile) {
        dosrt_interp_profile = NULL;
    }
}

void native_set_platform(Machine *m, const HostPlatform *p)
{
    NativeState *n = native_state(m);
    n->platform = p;
    native_reanchor(m);
    n->last_yield_us = n->last_present_us = p ? native_wall_us(n) : m->virtual_us;
}

void native_set_headless_hook(Machine *m, void (*fn)(Machine *m))
{
    native_state(m)->headless_hook = fn;
}

void native_wake(Machine *m)
{
    native_state(m)->woken = true;
}

uint8_t native_port61(const Machine *m)
{
    return native_state(m)->port61;
}

void native_set_port61(Machine *m, uint8_t v)
{
    native_state(m)->port61 = v;
}

/* ---- yields and pacing ---- */

static bool deterministic(const NativeState *n)
{
    return n->cfg.deterministic || !n->platform;
}

static void present(Machine *m, NativeState *n)
{
    static uint32_t frame[VGA_MAX_W * VGA_MAX_H];
    int w = 0, h = 0;
    if (m->native_video)
        host_video_render(m, frame, &w, &h);
    else
        vga_render(m, frame, &w, &h);
    n->platform->present(n->platform->ctx, frame, w, h);
}

void native_yield(Machine *m)
{
    NativeState *n = native_state(m);
    const HostPlatform *p = n->platform;
    if (!p) {
        n->last_yield_us = m->virtual_us;
        if (n->headless_hook)
            n->headless_hook(m);
        return;
    }
    if (p->poll_events)
        p->poll_events(p->ctx, m);
    if (p->audio_pump)
        p->audio_pump(p->ctx, m);
    uint64_t wall = native_wall_us(n);
    if (p->present && wall - n->last_present_us >= NATIVE_PRESENT_US) {
        n->last_present_us = wall;
        present(m, n);
    }
    p->sleep_us(p->ctx, 0);
    n->last_yield_us = deterministic(n) ? m->virtual_us : native_wall_us(n);
}

/* Every NATIVE_CHECK_EVERY safe points: sleep while the clock runs ahead of the wall clock, drop time
 * it fell behind by, and give the platform its turn every yield_every_us. */
static void pace(Machine *m, NativeState *n)
{
    if (deterministic(n)) {
        if (m->virtual_us - n->last_yield_us >= n->cfg.yield_every_us)
            native_yield(m);
        return;
    }
    uint64_t wall = native_wall_us(n);
    int64_t ahead = (int64_t)(m->virtual_us - n->anchor_virtual_us) - (int64_t)(wall - n->anchor_wall_us);
    if (ahead > NATIVE_AHEAD_US) {
        n->platform->sleep_us(n->platform->ctx, (uint64_t)ahead);
        n->last_yield_us = native_wall_us(n);
        if (n->platform->poll_events)
            n->platform->poll_events(n->platform->ctx, m);
    } else if (ahead < -(int64_t)NATIVE_BEHIND_US) {
        native_reanchor(m);
    }
    if (wall - n->last_yield_us >= n->cfg.yield_every_us)
        native_yield(m);
}

bool native_poll_deliver(Machine *m)
{
    host_input_poll(m);
    host_link_pump(m);
    host_timer_poll(m);
    return (m->cpu.flags & F_IF) && native_deliver_irq(m);
}

void native_poll(Machine *m)
{
    NativeState *n = native_state(m);
    native_poll_deliver(m);
    if (++n->safepoints % NATIVE_CHECK_EVERY == 0)
        pace(m, n);
}

void native_safepoint(Machine *m)
{
    native_charge(m, native_state(m)->cfg.call_cost_ns);
    native_poll(m);
}

void native_interp_time(Machine *m, uint32_t us)
{
    native_charge(m, us * 1000u);
}

/* ---- ROM lifts and guest code nothing native resolves ---- */

void native_rom_lift(Machine *m, uint16_t seg, uint16_t off, LiftedFn fn, const char *name)
{
    NativeState *n = native_state(m);
    uint32_t linear = real_addr(seg, off);
    int i = 0;
    while (i < n->rom_count && n->rom[i].linear != linear)
        i++;
    if (i == NATIVE_ROM_LIFTS_MAX)
        fatal("native: ROM lift table full registering %s", name);
    if (i == n->rom_count)
        n->rom_count++;
    n->rom[i] = (NativeRomLift){ .linear = linear, .fn = fn, .name = name };
}

_Noreturn static void interpreted_fatal(Machine *m)
{
    uint16_t cs = m->cpu.seg[S_CS], ip = (uint16_t)m->cpu.eip, base = 0;
    const char *module = lift_module_owning(m, cs, &base);
    fatal("native: guest code at %04X:%04X (%s +%04X:%04X) would be interpreted", cs, ip,
          module ? module : "no module", (uint16_t)(cs - base), ip);
}

bool native_unresolved(Machine *m, uint32_t linear)
{
    NativeState *n = native_state(m);
    for (int i = 0; i < n->rom_count; i++)
        if (n->rom[i].linear == linear) {
            n->rom[i].fn(m);
            return true;
        }
    if (n->cfg.strict)
        interpreted_fatal(m);
    return false;
}

void native_cpu_entry(Machine *m)
{
    uint32_t linear = m->cpu.seg_base[S_CS] + (m->cpu.eip & 0xFFFF);
    if (native_state(m)->cfg.strict && !lift_hooked(m, linear))
        interpreted_fatal(m);
}

void native_port_fatal(Machine *m, bool out, uint16_t port)
{
    fatal("native: port %s %04X at %04X:%04X", out ? "out" : "in", port, m->cpu.seg[S_CS],
          (unsigned)(m->cpu.eip & 0xFFFF));
}

/* Non-strict runs end with what the interpreter still ran while native, per module. */
static void report_interpreted(Machine *m, const uint32_t *profile)
{
    enum { MODULES_MAX = 32 };
    typedef struct ModuleCount { const char *name; uint64_t count; } ModuleCount;
    ModuleCount per[MODULES_MAX];
    int count = 0;
    uint64_t total = 0;
    for (uint32_t para = 0; para < MEM_ONE_MEG / 16; para++) {
        if (!profile[para])
            continue;
        total += profile[para];
        uint16_t base;
        const char *name = para >= 0xF000 ? "BIOS/ROM" : lift_module_owning(m, (uint16_t)para, &base);
        if (!name)
            name = "no module";
        int i = 0;
        while (i < count && strcmp(per[i].name, name) != 0)
            i++;
        if (i == count && count < MODULES_MAX)
            per[count++] = (ModuleCount){ .name = name };
        if (i < count)
            per[i].count += profile[para];
    }
    if (!total)
        return;
    log_msg("native: %llu instructions interpreted while native (INN_NATIVE_STRICT=0)", (unsigned long long)total);
    for (int i = 0; i < count; i++)
        log_msg("native:   %s %llu", per[i].name, (unsigned long long)per[i].count);
}

void native_shutdown(Machine *m)
{
    NativeState *n = native_state(m);
    if (!n)
        return;
    if (n->interp_profile) {
        report_interpreted(m, n->interp_profile);
        if (dosrt_interp_profile == n->interp_profile)
            dosrt_interp_profile = NULL;
        free(n->interp_profile);
    }
    free(n);
    m->native_state = NULL;
}
