/* 8253/8254 programmable interval timer. Counters are derived from m->virtual_us rather than
 * stepped, so reads are exact to the scheduler slice. Channel 0 drives IRQ0; channel 2 is
 * gated by port 61h and feeds the speaker. */
#include <stdlib.h>

#include "devices.h"

enum {
    PIT_PORT_CH0 = 0x40, PIT_PORT_CMD = 0x43, PORT_SYSCTL_B = 0x61,
    PIT_CHANNELS = 3, PIT_FULL_COUNT = 0x10000,
    CMD_CHANNEL_SHIFT = 6, CMD_ACCESS_SHIFT = 4, CMD_MODE_SHIFT = 1,
    CMD_READBACK = 3, ACCESS_LATCH = 0, ACCESS_LO = 1, ACCESS_HI = 2, ACCESS_LOHI = 3,
    READBACK_NO_COUNT = 0x20,
    SYSB_GATE2 = 0x01, SYSB_SPEAKER = 0x02, SYSB_REFRESH = 0x10, SYSB_OUT2 = 0x20,
    REFRESH_PERIOD_NS = 15085,
    MAX_IRQS_PER_TICK = 4,
};

typedef struct PitChannel {
    uint8_t mode, access;
    uint32_t reload;       /* 1..65536 */
    uint64_t start_clock;  /* PIT clock at load or gate */
    uint64_t periods_seen; /* channel 0: IRQ edges already raised */
    uint16_t latch;
    bool latched, read_hi, write_hi, armed, fired;
    uint8_t write_lo;
} PitChannel;

typedef struct Pit {
    PitChannel ch[PIT_CHANNELS];
    uint8_t sysctl_b;
} Pit;

static Pit *pit_state(Machine *m) { return m->pit; }

static uint64_t pit_clock(Machine *m)
{
    return m->virtual_us * PIT_CLOCK_HZ / 1000000;
}

static uint64_t elapsed(Machine *m, const PitChannel *c) { return pit_clock(m) - c->start_clock; }

static bool periodic(const PitChannel *c) { return c->mode == 2 || c->mode == 3 || c->mode >= 6; }

static uint16_t current_count(Machine *m, const PitChannel *c)
{
    if (!c->armed)
        return (uint16_t)c->reload;
    uint64_t e = elapsed(m, c);
    if (periodic(c) && (c->mode & 3) == 3) {
        uint64_t half = c->reload / 2 ? c->reload / 2 : 1;
        return (uint16_t)((c->reload - 2 * (e % half)) & ~1u);
    }
    if (periodic(c))
        return (uint16_t)(c->reload - e % c->reload);
    return (uint16_t)(c->reload - e); /* mode 0/1/4: wraps past zero like the hardware */
}

static bool channel_output(Machine *m, const PitChannel *c)
{
    if (!c->armed)
        return c->mode != 0;
    uint64_t e = elapsed(m, c);
    if (periodic(c) && (c->mode & 3) == 3)
        return e % c->reload < (c->reload + 1) / 2;
    if (periodic(c))
        return e % c->reload != c->reload - 1;
    return e >= c->reload;
}

static void channel_load(Machine *m, PitChannel *c, uint32_t value)
{
    c->reload = value ? value : PIT_FULL_COUNT;
    c->start_clock = pit_clock(m);
    c->periods_seen = 0;
    c->armed = true;
    c->fired = false;
}

static uint8_t channel_read(Machine *m, PitChannel *c)
{
    uint16_t v = c->latched ? c->latch : current_count(m, c);
    bool hi = c->access == ACCESS_HI || (c->access == ACCESS_LOHI && c->read_hi);
    if (c->access == ACCESS_LOHI)
        c->read_hi = !c->read_hi;
    if (c->access != ACCESS_LOHI || !c->read_hi)
        c->latched = false;
    return (uint8_t)(hi ? v >> 8 : v);
}

static void channel_write(Machine *m, PitChannel *c, uint8_t v)
{
    switch (c->access) {
    case ACCESS_LO: channel_load(m, c, v); break;
    case ACCESS_HI: channel_load(m, c, (uint32_t)v << 8); break;
    default:
        if (!c->write_hi) {
            c->write_lo = v;
            c->write_hi = true;
        } else {
            c->write_hi = false;
            channel_load(m, c, c->write_lo | (uint32_t)v << 8);
        }
    }
}

static void latch_channel(Machine *m, PitChannel *c)
{
    if (c->latched)
        return;
    c->latch = current_count(m, c);
    c->latched = true;
    c->read_hi = false;
}

static void pit_command(Machine *m, uint8_t v)
{
    Pit *p = pit_state(m);
    int sel = v >> CMD_CHANNEL_SHIFT;
    if (sel == CMD_READBACK) {
        for (int i = 0; i < PIT_CHANNELS; i++)
            if (!(v & READBACK_NO_COUNT) && (v & (2u << i)))
                latch_channel(m, &p->ch[i]);
        return;
    }
    PitChannel *c = &p->ch[sel];
    int access = v >> CMD_ACCESS_SHIFT & 3;
    if (access == ACCESS_LATCH) {
        latch_channel(m, c);
        return;
    }
    c->access = (uint8_t)access;
    c->mode = v >> CMD_MODE_SHIFT & 7;
    c->latched = c->read_hi = c->write_hi = false;
    c->armed = false; /* counting starts when the count is written */
}

static uint8_t sysctl_read(Machine *m)
{
    Pit *p = pit_state(m);
    uint8_t v = p->sysctl_b & (SYSB_GATE2 | SYSB_SPEAKER);
    if ((m->virtual_us * 1000 / REFRESH_PERIOD_NS) & 1)
        v |= SYSB_REFRESH;
    if (channel_output(m, &p->ch[2]))
        v |= SYSB_OUT2;
    return v;
}

static uint32_t pit_in(Machine *m, uint16_t port, int size)
{
    if (port == PORT_SYSCTL_B)
        return sysctl_read(m);
    if (port == PIT_PORT_CMD)
        return 0xFF;
    return channel_read(m, &pit_state(m)->ch[port - PIT_PORT_CH0]);
}

static void pit_out(Machine *m, uint16_t port, uint32_t value, int size)
{
    Pit *p = pit_state(m);
    if (port == PORT_SYSCTL_B) {
        bool gate_rise = (value & SYSB_GATE2) && !(p->sysctl_b & SYSB_GATE2);
        p->sysctl_b = (uint8_t)value;
        if (gate_rise && p->ch[2].armed)
            p->ch[2].start_clock = pit_clock(m);
    } else if (port == PIT_PORT_CMD) {
        pit_command(m, (uint8_t)value);
    } else {
        channel_write(m, &p->ch[port - PIT_PORT_CH0], (uint8_t)value);
    }
}

/* One IRQ0 edge per elapsed period; several periods inside one slice still yield an edge each
 * slice (at most MAX_IRQS_PER_TICK are counted), which is what INT 08h counters expect. */
static void pit_tick(Machine *m)
{
    PitChannel *c = &pit_state(m)->ch[0];
    if (!c->armed)
        return;
    if (!periodic(c)) {
        if (!c->fired && elapsed(m, c) >= c->reload) {
            c->fired = true;
            pic_lower_irq(m, PIC_IRQ_TIMER);
            pic_raise_irq(m, PIC_IRQ_TIMER);
        }
        return;
    }
    uint64_t periods = elapsed(m, c) / c->reload;
    if (periods == c->periods_seen)
        return;
    if (periods - c->periods_seen > MAX_IRQS_PER_TICK)
        c->periods_seen = periods - MAX_IRQS_PER_TICK;
    c->periods_seen++;
    pic_lower_irq(m, PIC_IRQ_TIMER);
    pic_raise_irq(m, PIC_IRQ_TIMER);
}

double pit_timer_hz(Machine *m)
{
    return (double)PIT_CLOCK_HZ / pit_state(m)->ch[0].reload;
}

bool pit_speaker(Machine *m, double *hz)
{
    Pit *p = pit_state(m);
    bool on = (p->sysctl_b & (SYSB_GATE2 | SYSB_SPEAKER)) == (SYSB_GATE2 | SYSB_SPEAKER);
    if (hz)
        *hz = (double)PIT_CLOCK_HZ / p->ch[2].reload;
    return on && p->ch[2].armed;
}

void pit_init(Machine *m)
{
    Pit *p = calloc(1, sizeof *p);
    if (!p)
        fatal("out of memory");
    m->pit = p;
    for (int i = 0; i < PIT_CHANNELS; i++) {
        p->ch[i].mode = 3;
        p->ch[i].access = ACCESS_LOHI;
        p->ch[i].reload = PIT_FULL_COUNT;
    }
    channel_load(m, &p->ch[0], 0);
    port_register(m, PIT_PORT_CH0, PIT_PORT_CMD, pit_in, pit_out);
    port_register(m, PORT_SYSCTL_B, PORT_SYSCTL_B, pit_in, pit_out);
    machine_add_ticker(m, pit_tick);
}
