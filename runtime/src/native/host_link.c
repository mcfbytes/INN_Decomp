/* Host link (docs/native.md section 9): the existing SerialLink behind the host-API seam. The lifted
 * driver registers a pump that moves its rings to and from it. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "host_link.h"
#include "lift_internal.h"
#include "native.h"

/* The pump is throttled by emulated time: a socket read per safe point would dwarf the work between. */
enum { HOST_LINK_PUMP_US = 1000, BYTE_BITS = 10, PACE_IDLE_US = 2000 };

typedef struct HostLink {
    Machine *owner;
    SerialLink link;
    void (*pump)(Machine *m);
    uint64_t next_pump_us;
    bool in_pump;
    bool dtr_logged;
    uint32_t byte_us;               /* line time of one byte; 0 while the link is unpaced */
    uint64_t tx_next_us, rx_next_us; /* when the line is free for the next byte each way */
} HostLink;

/* One link per process: the client has one modem. The state follows the machine that opened it. */
static HostLink host;

static SerialLink *link_of(Machine *m)
{
    return host.owner == m && host.link.send ? &host.link : NULL;
}

/* A shadow run keeps the original's end state, so link bytes the lifted half moved would be lost or
 * sent twice: the function being shadowed belongs in INN_VERIFY_SKIP. Reported once each. */
static void note_shadow(Machine *m)
{
    enum { SEEN_MAX = 64 };
    static const LiftedEntry *seen[SEEN_MAX];
    static int seen_count;
    LiftState *s = lift_state(m);
    if (!s || !s->in_shadow || !s->shadow_of || host.in_pump)
        return;
    const LiftedEntry *e = s->shadow_of->entry;
    for (int i = 0; i < seen_count; i++)
        if (seen[i] == e)
            return;
    if (seen_count < SEEN_MAX)
        seen[seen_count++] = e;
    log_msg("host_link: used during a shadow run of %s %s (add it to INN_VERIFY_SKIP)", e->module, e->name);
}

/* Every API call is a safe point in native mode, except the ones the pump itself makes from one. */
static void enter(Machine *m)
{
    note_shadow(m);
    if (m->native && !host.in_pump)
        native_safepoint(m);
}

int host_link_attach(Machine *m, SerialLink link)
{
    if (!link.send)
        return -1;
    host_link_close(m);
    host.owner = m;
    host.link = link;
    host.pump = NULL;
    host.next_pump_us = 0;
    host.in_pump = false;
    host.dtr_logged = false;
    host.byte_us = 0;
    return 0;
}

void host_link_close(Machine *m)
{
    if (host.owner == m && host.link.close)
        host.link.close(host.link.ctx);
    if (host.owner == m)
        memset(&host, 0, sizeof host);
}

int host_link_open(Machine *m, const char *spec)
{
    SerialLink link;
    if (!strncmp(spec, "ws://", 5) || !strncmp(spec, "wss://", 6)) {
        link = serial_link_ws(spec);
    } else {
        const char *colon = strrchr(spec, ':');
        char name[256];
        if (!colon || colon == spec || (size_t)(colon - spec) >= sizeof name)
            return -1;
        memcpy(name, spec, (size_t)(colon - spec));
        name[colon - spec] = 0;
        link = serial_link_tcp(name, atoi(colon + 1));
    }
    if (host_link_attach(m, link) != 0)
        return -1;
    return 0;
}

/* Bytes the line can carry now: one per byte time, with credit only for the last PACE_IDLE_US, so an
 * idle line does not release a burst. */
static int allowance(Machine *m, uint64_t *next, int want)
{
    if (!host.byte_us || want <= 0)
        return want;
    uint64_t now = m->virtual_us;
    if (now < *next)
        return 0;
    if (now - *next > PACE_IDLE_US)
        *next = now;
    uint64_t n = 1 + (now - *next) / host.byte_us;
    return n < (uint64_t)want ? (int)n : want;
}

void host_link_set_rate(Machine *m, uint32_t baud)
{
    const char *pacing = getenv("INN_LINK_PACING");
    if (host.owner != m)
        return;
    host.byte_us = baud && !(pacing && !strcmp(pacing, "0")) ? BYTE_BITS * 1000000u / baud : 0;
}

bool host_link_carrier(Machine *m)
{
    enter(m);
    SerialLink *l = link_of(m);
    return l && l->connected && l->connected(l->ctx);
}

int host_link_send(Machine *m, const uint8_t *buf, int len)
{
    enter(m);
    SerialLink *l = link_of(m);
    if (!l)
        return -1;
    int n = allowance(m, &host.tx_next_us, len);
    int taken = n ? l->send(l->ctx, buf, n) : 0;
    if (taken > 0 && host.byte_us)
        host.tx_next_us += (uint64_t)taken * host.byte_us;
    return taken;
}

int host_link_recv(Machine *m, uint8_t *buf, int cap)
{
    enter(m);
    SerialLink *l = link_of(m);
    int want = l && l->recv ? allowance(m, &host.rx_next_us, cap) : 0;
    int n = want ? l->recv(l->ctx, buf, want) : 0;
    if (n > 0 && host.byte_us)
        host.rx_next_us += (uint64_t)n * host.byte_us;
    if (n > 0 && m->native)
        native_wake(m);
    return n;
}

void host_link_drain(Machine *m)
{
    SerialLink *l = link_of(m);
    if (l)
        serial_link_drain(l);
}

void host_link_set_dtr(Machine *m, bool on)
{
    enter(m);
    if (!host.dtr_logged) {
        host.dtr_logged = true;
        log_msg("host_link: DTR changes are ignored (%s)", on ? "raised" : "dropped");
    }
}

void host_link_break(Machine *m)
{
    /* innkeeperd takes a CR between frames for the PAD escape a real BREAK would cause. */
    static const uint8_t BREAK_STANDIN = '\r';
    SerialLink *l = link_of(m);
    enter(m);
    if (l)
        l->send(l->ctx, &BREAK_STANDIN, 1); /* a BREAK is not a character: no line time */
}

/* Emulated and hybrid-hw runs have no safe points; the machine's tickers stand in for them. */
static void link_tick(Machine *m)
{
    host_link_pump(m);
}

void host_link_set_pump(Machine *m, void (*pump)(Machine *m))
{
    if (host.owner != m)
        return;
    host.pump = pump;
    if (!pump || m->native)
        return;
    for (int i = 0; i < m->ticker_count; i++)
        if (m->tickers[i] == link_tick)
            return;
    machine_add_ticker(m, link_tick);
}

static void run_pump(Machine *m)
{
    host.in_pump = true;
    host.pump(m);
    host.in_pump = false;
}

void host_link_kick(Machine *m)
{
    if (host.owner != m || !host.pump || host.in_pump)
        return;
    note_shadow(m);
    run_pump(m);
}

void host_link_pump(Machine *m)
{
    if (host.owner != m || !host.pump || host.in_pump || m->virtual_us < host.next_pump_us)
        return;
    /* Not while interrupts are held (a shadow run). IF is not consulted: a ticker slice often ends inside
     * a BIOS service with IF clear, and the UART would have kept its interrupt pending until IF came back. */
    if (m->irq_hold)
        return;
    host.next_pump_us = m->virtual_us + HOST_LINK_PUMP_US;
    run_pump(m);
}
