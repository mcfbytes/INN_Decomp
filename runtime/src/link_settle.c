/* Link settling (docs/runtime.md, "Link settling"): a SerialLink wrapper that makes headless runs
 * repeatable. A headless run's emulated clock is independent of the host, except for the moments the
 * server's bytes arrive. Here, once the guest has sent bytes, what the server sends next is held until
 * window_us of emulated time after the guest's last sent byte; the first receive poll at or after that
 * moment waits (wall time) until the server has been quiet for quiet_ms and then releases everything.
 * The reply therefore reaches the guest at the same emulated time in every run. */
#define _POSIX_C_SOURCE 200809L /* clock_gettime, nanosleep */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "devices.h"

enum {
    SETTLE_CAP_MS = 5000,       /* a server that never goes quiet is released after this */
    SETTLE_POLL_US = 500,       /* wall time between socket polls while waiting */
    CONNECT_WAIT_MS = 5000,     /* the first carrier check waits this long for the connect */
    UNPROMPTED_LOG_MAX = 20,
    PENDING_MAX_US = 250000,    /* a guest that keeps sending still settles this long after its first byte */
};

typedef struct Settle {
    Machine *m;
    SerialLink inner;
    uint32_t quiet_us, window_us;
    uint8_t *buf;
    size_t head, count, cap, released; /* buf[head .. head+count): released bytes come first */
    bool pending, closed, connect_waited;
    uint64_t first_send_us, deadline_us;
    /* statistics, logged at close (or exit) */
    uint64_t settles, waited_us, unprompted_bytes, unprompted_bursts, capped;
    uint64_t send_wall_us, max_reply_us; /* wall time from a send to the first byte after it */
    bool awaiting_reply;
} Settle;

static Settle *last_opened; /* for the exit report: a native run never closes its link */

static uint64_t wall_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void nap(void)
{
    struct timespec ts = { 0, SETTLE_POLL_US * 1000L };
    nanosleep(&ts, NULL);
}

static bool reserve(Settle *s, size_t more)
{
    if (s->head + s->count + more <= s->cap)
        return true;
    if (s->head) {
        memmove(s->buf, s->buf + s->head, s->count);
        s->head = 0;
        if (s->count + more <= s->cap)
            return true;
    }
    size_t cap = s->cap ? s->cap : 4096;
    while (cap < s->count + more)
        cap *= 2;
    uint8_t *buf = realloc(s->buf, cap);
    if (!buf)
        return false;
    s->buf = buf;
    s->cap = cap;
    return true;
}

/* Moves whatever the inner link has into the buffer. Returns the bytes taken. */
static size_t pull(Settle *s)
{
    size_t total = 0;
    while (!s->closed && reserve(s, 1024)) {
        int n = s->inner.recv(s->inner.ctx, s->buf + s->head + s->count, 1024);
        if (n < 0)
            s->closed = true;
        if (n <= 0)
            break;
        s->count += (size_t)n;
        total += (size_t)n;
    }
    if (total && s->awaiting_reply) {
        uint64_t latency = wall_us() - s->send_wall_us;
        if (latency > s->max_reply_us)
            s->max_reply_us = latency;
        s->awaiting_reply = false;
    }
    return total;
}

/* Waits until the server has been quiet for quiet_us (or twice its slowest reply), then releases
 * everything held. */
static void settle(Settle *s)
{
    /* A loaded host answers late: wait twice the slowest reply seen so far, up to 4 x quiet_us. */
    uint64_t quiet = 2 * s->max_reply_us;
    if (quiet < s->quiet_us)
        quiet = s->quiet_us;
    if (quiet > 4 * (uint64_t)s->quiet_us)
        quiet = 4 * (uint64_t)s->quiet_us;
    uint64_t start = wall_us(), last = start;
    while (!s->closed) {
        if (pull(s))
            last = wall_us();
        uint64_t now = wall_us();
        if (now - last >= quiet)
            break;
        if (now - start >= (uint64_t)SETTLE_CAP_MS * 1000u) {
            if (!s->capped++)
                log_msg("link-settle: the server did not go quiet within %d ms", SETTLE_CAP_MS);
            break;
        }
        nap();
    }
    s->settles++;
    s->waited_us += wall_us() - start;
    s->pending = false;
    s->awaiting_reply = false; /* a later byte is not this send's reply */
    s->released = s->count;
}

static int settle_send(void *ctx, const uint8_t *buf, int len)
{
    Settle *s = ctx;
    int n = s->inner.send(s->inner.ctx, buf, len);
    if (n > 0) {
        if (!s->pending)
            pull(s); /* what came before the send belongs before it */
        if (!s->pending) {
            s->released = s->count;
            s->first_send_us = s->m->virtual_us;
        }
        s->pending = true;
        s->send_wall_us = wall_us();
        s->awaiting_reply = true;
        uint64_t due = s->m->virtual_us + s->window_us, limit = s->first_send_us + PENDING_MAX_US;
        s->deadline_us = due < limit ? due : limit;
    }
    return n;
}

static int settle_recv(void *ctx, uint8_t *buf, int cap)
{
    Settle *s = ctx;
    if (s->pending && s->m->virtual_us >= s->deadline_us) {
        settle(s);
    } else {
        size_t got = pull(s);
        if (!s->pending && got) {
            /* Not prompted by a send: its emulated arrival time still follows the wall clock. */
            s->unprompted_bytes += got;
            if (s->unprompted_bursts++ < UNPROMPTED_LOG_MAX)
                log_msg("link-settle: %zu bytes arrived unprompted at %llu ms", got,
                        (unsigned long long)(s->m->virtual_us / 1000));
            s->released = s->count;
        }
    }
    size_t n = s->released < (size_t)cap ? s->released : (size_t)cap;
    if (!n)
        return s->closed && !s->count ? -1 : 0;
    memcpy(buf, s->buf + s->head, n);
    s->head += n;
    s->count -= n;
    s->released -= n;
    if (!s->count)
        s->head = 0;
    return (int)n;
}

static bool settle_connected(void *ctx)
{
    Settle *s = ctx;
    if (!s->connect_waited) {
        /* A background connect finishes at a wall time: wait for it once, so carrier rises at the
         * first check in every run. */
        s->connect_waited = true;
        uint64_t start = wall_us();
        while (!s->inner.connected(s->inner.ctx) && wall_us() - start < (uint64_t)CONNECT_WAIT_MS * 1000u)
            nap();
    }
    return s->inner.connected(s->inner.ctx);
}

static void report(const Settle *s)
{
    if (s->settles || s->unprompted_bytes)
        log_msg("link-settle: %llu settles, %llu ms waited, slowest reply %llu ms, "
                "%llu bytes unprompted in %llu bursts",
                (unsigned long long)s->settles, (unsigned long long)(s->waited_us / 1000),
                (unsigned long long)(s->max_reply_us / 1000), (unsigned long long)s->unprompted_bytes,
                (unsigned long long)s->unprompted_bursts);
}

static void report_at_exit(void)
{
    if (last_opened)
        report(last_opened);
}

static void settle_close(void *ctx)
{
    Settle *s = ctx;
    report(s);
    if (last_opened == s)
        last_opened = NULL;
    if (s->inner.close)
        s->inner.close(s->inner.ctx);
    free(s->buf);
    free(s);
}

SerialLink serial_link_settle(Machine *m, SerialLink inner, uint32_t quiet_ms, uint32_t window_us)
{
    if (!inner.send || !inner.recv || !inner.connected)
        return inner;
    Settle *s = calloc(1, sizeof *s);
    if (!s)
        return inner;
    s->m = m;
    s->inner = inner;
    s->quiet_us = quiet_ms * 1000u;
    s->window_us = window_us;
    static bool hooked;
    if (!hooked)
        hooked = atexit(report_at_exit) == 0;
    last_opened = s;
    SerialLink link = { s, settle_send, settle_recv, settle_connected, settle_close };
    return link;
}

SerialLink serial_link_settle_env(Machine *m, SerialLink inner)
{
    enum { DEFAULT_WINDOW_US = 3000 };
    const char *quiet = getenv("INN_LINK_SETTLE_MS");
    if (!quiet || !*quiet || atoi(quiet) <= 0 || !inner.send)
        return inner;
    const char *window = getenv("INN_LINK_SETTLE_WINDOW_MS");
    uint32_t window_us = window && *window ? (uint32_t)atoi(window) * 1000u : DEFAULT_WINDOW_US;
    log_msg("link-settle: on, quiet %d ms, window %u ms", atoi(quiet), window_us / 1000);
    return serial_link_settle(m, inner, (uint32_t)atoi(quiet), window_us);
}
