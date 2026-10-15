/* PIT channel 0 as a host timer: IRQ 0 every divisor / 1193182 s of the native clock. */
#include "host_timer.h"
#include "native_internal.h"

enum { PIT_HZ = 1193182 };

static uint32_t divisor_value(uint16_t divisor)
{
    return divisor ? divisor : 0x10000u;
}

/* One period later, exactly: the fraction of a nanosecond is carried in 1/PIT_HZ units. */
static void advance_due(NativeState *n)
{
    uint64_t num = (uint64_t)divisor_value(n->divisor) * 1000000000u + n->timer_due_frac;
    n->timer_due_ns += num / PIT_HZ;
    n->timer_due_frac = (uint32_t)(num % PIT_HZ);
}

static uint64_t now_ns(const Machine *m)
{
    return m->virtual_us * 1000 + native_state(m)->sub_us_ns;
}

void native_timer_reset(Machine *m)
{
    NativeState *n = native_state(m);
    n->timer_due_ns = now_ns(m);
    n->timer_due_frac = 0;
    n->timer_pending = 0;
    advance_due(n);
}

void host_timer_set_divisor(Machine *m, uint16_t divisor)
{
    native_state(m)->divisor = divisor;
    native_timer_reset(m); /* reprogramming the counter restarts its period */
}

uint16_t host_timer_divisor(const Machine *m)
{
    return native_state(m)->divisor;
}

uint64_t host_timer_next_due_us(const Machine *m)
{
    const NativeState *n = native_state(m);
    if (n->timer_pending)
        return m->virtual_us;
    return (n->timer_due_ns + 999) / 1000;
}

void host_timer_speaker(Machine *m, uint16_t divisor, bool on)
{
    (void)m, (void)divisor, (void)on; /* no INN code drives the speaker */
}

void host_timer_poll(Machine *m)
{
    NativeState *n = native_state(m);
    uint64_t now = now_ns(m);
    if (now >= n->timer_due_ns) {
        uint64_t period = (uint64_t)divisor_value(n->divisor) * 1000000000u / PIT_HZ;
        uint64_t overdue = (now - n->timer_due_ns) / period + 1;
        if (overdue > HOST_TIMER_MAX_CATCH_UP) { /* a long jump: only the last few ticks count */
            uint64_t skip = overdue - HOST_TIMER_MAX_CATCH_UP;
            n->timer_due_ns += skip * period;
            n->timer_dropped += skip;
        }
        while (now >= n->timer_due_ns) {
            if (n->timer_pending < HOST_TIMER_MAX_CATCH_UP)
                n->timer_pending++;
            else
                n->timer_dropped++;
            advance_due(n);
        }
    }
    if (n->timer_pending && !(n->pic[0].irr & 1u)) {
        n->timer_pending--;
        native_irq_raise(m, 0);
    }
}
