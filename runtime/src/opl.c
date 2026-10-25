/* YM3812 (OPL2) as on an AdLib at 388h/389h, mirrored at 228h/229h and 220h-223h (SB Pro left/right).
 * Synthesis is Nuked-OPL3 in OPL2 mode; timers, status and the waveform-select enable are ours. */
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "opl3.h"

enum {
    OPL_PORT_ADDR = 0x388, OPL_PORT_DATA = 0x389,
    SB_OPL_PORT_ADDR = 0x228, SB_OPL_PORT_DATA = 0x229, SBPRO_OPL_FIRST = 0x220, SBPRO_OPL_LAST = 0x223,
    OPL_NATIVE_RATE = 49716, /* 14.31818 MHz / 4 / 72 */
    REG_TEST = 0x01, REG_TIMER1 = 0x02, REG_TIMER2 = 0x03, REG_TIMER_CTRL = 0x04,
    REG_WAVE_FIRST = 0xE0, REG_WAVE_LAST = 0xF5, WAVE_REGS = REG_WAVE_LAST - REG_WAVE_FIRST + 1,
    TEST_WSE = 0x20,
    CTRL_IRQ_RESET = 0x80, CTRL_MASK_T1 = 0x40, CTRL_MASK_T2 = 0x20,
    CTRL_START_T2 = 0x02, CTRL_START_T1 = 0x01,
    STATUS_IRQ = 0x80, STATUS_T1 = 0x40, STATUS_T2 = 0x20,
    STATUS_OPL2_ID = 0x06, /* low bits read as 110 on a YM3812, 000 on a YMF262 */
    T1_SAMPLES = 4, T2_SAMPLES = 16, /* timer resolution: 80.4 us and 321.8 us */
    TIMER_COUNTS = 256,
    FRAC_ONE = 1 << 16,
};

typedef struct OplTimer {
    uint8_t preset;
    bool running, masked, overflow;
    uint64_t start_ns;
    uint64_t tick_ns;
} OplTimer;

typedef struct Opl {
    opl3_chip chip;
    uint8_t addr;
    bool wse;
    uint8_t wave[WAVE_REGS]; /* written waveforms; the chip sees 0 while WSE is off */
    OplTimer t1, t2;
    int16_t prev[AUDIO_CHANNELS], cur[AUDIO_CHANNELS];
    uint32_t frac; /* position between prev and cur in 1/FRAC_ONE native samples */
} Opl;

static Opl *opl_state(Machine *m) { return m->opl; }

static uint64_t timer_period(const OplTimer *t) { return (uint64_t)(TIMER_COUNTS - t->preset) * t->tick_ns; }

/* Overflows are evaluated lazily, when the status is read or the timer is reprogrammed. */
static void timer_update(OplTimer *t, uint64_t now)
{
    if (!t->running || now < t->start_ns)
        return;
    uint64_t period = timer_period(t), periods = (now - t->start_ns) / period;
    if (periods == 0)
        return;
    t->start_ns += periods * period;
    if (!t->masked)
        t->overflow = true;
}

static void timer_control(OplTimer *t, uint64_t now, bool masked, bool start)
{
    timer_update(t, now);
    t->masked = masked;
    if (masked)
        t->overflow = false;
    if (start && !t->running)
        t->start_ns = now;
    t->running = start;
}

static void write_control(Opl *o, uint64_t now, uint8_t v)
{
    if (v & CTRL_IRQ_RESET) {
        timer_update(&o->t1, now);
        timer_update(&o->t2, now);
        o->t1.overflow = o->t2.overflow = false;
        return;
    }
    timer_control(&o->t1, now, v & CTRL_MASK_T1, v & CTRL_START_T1);
    timer_control(&o->t2, now, v & CTRL_MASK_T2, v & CTRL_START_T2);
}

static uint8_t read_status(Machine *m)
{
    Opl *o = opl_state(m);
    uint64_t now = machine_now_ns(m);
    timer_update(&o->t1, now);
    timer_update(&o->t2, now);
    uint8_t s = STATUS_OPL2_ID;
    if (o->t1.overflow)
        s |= STATUS_T1 | STATUS_IRQ;
    if (o->t2.overflow)
        s |= STATUS_T2 | STATUS_IRQ;
    return s;
}

/* An OPL2 forces sine waves while WSE is clear; Nuked-OPL3 has no such switch. */
static void write_wse(Opl *o, bool on)
{
    if (on == o->wse)
        return;
    o->wse = on;
    for (int i = 0; i < WAVE_REGS; i++)
        OPL3_WriteReg(&o->chip, (uint16_t)(REG_WAVE_FIRST + i), on ? o->wave[i] : 0);
}

static void write_data(Machine *m, uint8_t v)
{
    Opl *o = opl_state(m);
    switch (o->addr) {
    case REG_TIMER1: o->t1.preset = v; return;
    case REG_TIMER2: o->t2.preset = v; return;
    case REG_TIMER_CTRL: write_control(o, machine_now_ns(m), v); return;
    default: break;
    }
    audio_sync(m);
    if (o->addr == REG_TEST) {
        write_wse(o, v & TEST_WSE);
    } else if (o->addr >= REG_WAVE_FIRST && o->addr <= REG_WAVE_LAST) {
        o->wave[o->addr - REG_WAVE_FIRST] = v;
        OPL3_WriteReg(&o->chip, o->addr, o->wse ? v : 0);
    } else {
        OPL3_WriteReg(&o->chip, o->addr, v);
    }
}

void opl_write_reg(Machine *m, uint8_t reg, uint8_t value)
{
    opl_state(m)->addr = reg;
    write_data(m, value); /* audio_sync before a sound change; timers use machine_now_ns */
}

uint8_t opl_read_status(Machine *m)
{
    return read_status(m);
}

static uint32_t opl_in(Machine *m, uint16_t port, int size)
{
    (void)size;
    return (port & 1) ? 0xFF : opl_read_status(m);
}

static void opl_out(Machine *m, uint16_t port, uint32_t value, int size)
{
    (void)size;
    if (port & 1)
        opl_write_reg(m, opl_state(m)->addr, (uint8_t)value);
    else
        opl_state(m)->addr = (uint8_t)value;
}

/* Linear interpolation from the chip's native rate, as Nuked's own resampler does. */
static void opl_render(Machine *m, int32_t *mix, int frames)
{
    Opl *o = opl_state(m);
    uint32_t step = (uint32_t)((uint64_t)OPL_NATIVE_RATE * FRAC_ONE / (uint64_t)audio_rate(m));
    for (int i = 0; i < frames; i++) {
        while (o->frac >= FRAC_ONE) {
            memcpy(o->prev, o->cur, sizeof o->prev);
            OPL3_Generate(&o->chip, o->cur);
            o->frac -= FRAC_ONE;
        }
        for (int c = 0; c < AUDIO_CHANNELS; c++)
            mix[i * AUDIO_CHANNELS + c] += o->prev[c] + (int32_t)((o->cur[c] - o->prev[c]) * (int64_t)o->frac / FRAC_ONE);
        o->frac += step;
    }
}

void opl_init(Machine *m)
{
    Opl *o = calloc(1, sizeof *o);
    if (!o)
        fatal("out of memory");
    m->opl = o;
    OPL3_Reset(&o->chip, OPL_NATIVE_RATE);
    o->t1.tick_ns = (uint64_t)T1_SAMPLES * 1000000000u / OPL_NATIVE_RATE;
    o->t2.tick_ns = (uint64_t)T2_SAMPLES * 1000000000u / OPL_NATIVE_RATE;
    port_register(m, OPL_PORT_ADDR, OPL_PORT_DATA, opl_in, opl_out);
    port_register(m, SB_OPL_PORT_ADDR, SB_OPL_PORT_DATA, opl_in, opl_out);
    port_register(m, SBPRO_OPL_FIRST, SBPRO_OPL_LAST, opl_in, opl_out);
    audio_name_source(m, audio_add_source(m, opl_render), "opl");
}

void opl_shutdown(Machine *m)
{
    free(m->opl);
    m->opl = NULL;
}
