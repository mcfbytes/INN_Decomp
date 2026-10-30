/* Link settling (docs/runtime.md, "Link settling"): replies are held until the window after the
 * guest's last send, a late reply is waited for, earlier bytes are not held, a guest that keeps
 * sending still settles, a drain keeps the hold, and INN_LINK_SETTLE_MS switches it on. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devices.h"

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            failures++;                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                      \
            fputc('\n', stderr);                               \
        }                                                      \
    } while (0)

enum { PIPE_BYTES = 64, QUIET_MS = 5, WINDOW_US = 3000, PENDING_MAX_US = 250000 };

/* A stand-in server: bytes in `in` are readable now; `late` becomes readable on the late_after-th
 * receive poll from now (a reply still on its way when the window ends). */
typedef struct Pipe {
    uint8_t out[PIPE_BYTES], in[PIPE_BYTES];
    int out_len, in_len, polls, late_after;
    char late;
    bool closed, eof;
} Pipe;

static int pipe_send(void *ctx, const uint8_t *buf, int len)
{
    Pipe *p = ctx;
    if (p->out_len + len > PIPE_BYTES)
        len = PIPE_BYTES - p->out_len;
    memcpy(p->out + p->out_len, buf, (size_t)len);
    p->out_len += len;
    return len;
}

static int pipe_recv(void *ctx, uint8_t *buf, int cap)
{
    Pipe *p = ctx;
    if (p->late && ++p->polls >= p->late_after) {
        p->in[p->in_len++] = (uint8_t)p->late;
        p->late = 0;
    }
    if (!p->in_len && p->eof)
        return -1;
    int n = p->in_len < cap ? p->in_len : cap;
    memcpy(buf, p->in, (size_t)n);
    memmove(p->in, p->in + n, (size_t)(p->in_len - n));
    p->in_len -= n;
    return n;
}

static bool pipe_connected(void *ctx) { return true; }
static void pipe_close(void *ctx) { ((Pipe *)ctx)->closed = true; }

static void give(Pipe *p, const char *s)
{
    size_t n = strlen(s);
    memcpy(p->in + p->in_len, s, n);
    p->in_len += (int)n;
}

static int take(SerialLink *l, char *out, int cap)
{
    int n = l->recv(l->ctx, (uint8_t *)out, cap - 1);
    out[n > 0 ? n : 0] = 0;
    return n;
}

static SerialLink settled(Machine *m, Pipe *p)
{
    SerialLink inner = { p, pipe_send, pipe_recv, pipe_connected, pipe_close };
    return serial_link_settle(m, inner, QUIET_MS, WINDOW_US);
}

static void test_hold_and_release(void)
{
    Machine *m = machine_create();
    Pipe p = { 0 };
    SerialLink l = settled(m, &p);
    char got[PIPE_BYTES];
    CHECK(l.connected(l.ctx), "carrier");

    m->virtual_us = 100000;
    give(&p, "HI");
    CHECK(take(&l, got, sizeof got) == 2 && !strcmp(got, "HI"), "unprompted bytes pass at once");

    give(&p, "A"); /* already on its way before the guest sends */
    CHECK(l.send(l.ctx, (const uint8_t *)"Q", 1) == 1 && p.out_len == 1, "send reaches the server");
    give(&p, "R1");
    m->virtual_us += WINDOW_US - 1;
    CHECK(take(&l, got, sizeof got) == 1 && !strcmp(got, "A"), "bytes from before the send are not held (%s)", got);
    CHECK(take(&l, got, sizeof got) == 0, "the reply is held inside the window");
    m->virtual_us += 1;
    CHECK(take(&l, got, 2) == 1 && !strcmp(got, "R"), "the reply is released at the window, by capacity");
    CHECK(take(&l, got, sizeof got) == 1 && !strcmp(got, "1"), "the rest follows");
    CHECK(take(&l, got, sizeof got) == 0, "nothing more");

    /* A reply that arrives during the wait is part of the settled reply. */
    l.send(l.ctx, (const uint8_t *)"Q", 1);
    p.late = 'L';
    p.polls = 0;
    p.late_after = 4;
    m->virtual_us += WINDOW_US;
    CHECK(take(&l, got, sizeof got) == 1 && !strcmp(got, "L"), "a late reply is waited for (%s)", got);

    /* Each send extends the window. */
    l.send(l.ctx, (const uint8_t *)"a", 1);
    give(&p, "X");
    m->virtual_us += WINDOW_US - 1000;
    CHECK(take(&l, got, sizeof got) == 0, "held");
    l.send(l.ctx, (const uint8_t *)"b", 1);
    m->virtual_us += WINDOW_US - 1000;
    CHECK(take(&l, got, sizeof got) == 0, "still held after a second send");
    m->virtual_us += 1000;
    CHECK(take(&l, got, sizeof got) == 1 && !strcmp(got, "X"), "released after the last send's window");

    /* A guest that never stops sending still settles. */
    uint64_t first = m->virtual_us;
    l.send(l.ctx, (const uint8_t *)"s", 1);
    give(&p, "Y");
    bool early = false;
    for (m->virtual_us = first + 1000; m->virtual_us < first + PENDING_MAX_US; m->virtual_us += 1000) {
        p.out_len = 0;
        l.send(l.ctx, (const uint8_t *)"s", 1);
        early |= take(&l, got, sizeof got) != 0;
    }
    CHECK(!early, "held while the guest keeps sending");
    CHECK(take(&l, got, sizeof got) == 1 && !strcmp(got, "Y"), "released %d us after the first send", PENDING_MAX_US);

    /* The server closing: what is buffered comes first, then -1. */
    l.send(l.ctx, (const uint8_t *)"q", 1);
    give(&p, "Z");
    p.eof = true;
    m->virtual_us += PENDING_MAX_US;
    CHECK(take(&l, got, sizeof got) == 1 && !strcmp(got, "Z"), "buffered bytes before the close");
    CHECK(take(&l, got, sizeof got) == -1, "then the close");

    l.close(l.ctx);
    CHECK(p.closed, "close closes the inner link");
    machine_destroy(m);
}

static void test_env(void)
{
    Machine *m = machine_create();
    Pipe p = { 0 };
    SerialLink inner = { &p, pipe_send, pipe_recv, pipe_connected, pipe_close };
    unsetenv("INN_LINK_SETTLE_MS");
    CHECK(serial_link_settle_env(m, inner).ctx == &p, "off without INN_LINK_SETTLE_MS");
    setenv("INN_LINK_SETTLE_MS", "0", 1);
    CHECK(serial_link_settle_env(m, inner).ctx == &p, "off with 0");
    setenv("INN_LINK_SETTLE_MS", "5", 1);
    setenv("INN_LINK_SETTLE_WINDOW_MS", "7", 1);
    SerialLink l = serial_link_settle_env(m, inner);
    CHECK(l.ctx != &p, "on with 5");
    m->virtual_us = 1000;
    l.send(l.ctx, (const uint8_t *)"Q", 1);
    give(&p, "R");
    char got[8];
    m->virtual_us += 6999;
    CHECK(take(&l, got, sizeof got) == 0, "INN_LINK_SETTLE_WINDOW_MS sets the window");
    m->virtual_us += 1;
    CHECK(take(&l, got, sizeof got) == 1, "released at 7 ms");
    l.close(l.ctx);
    unsetenv("INN_LINK_SETTLE_MS");
    unsetenv("INN_LINK_SETTLE_WINDOW_MS");
    machine_destroy(m);
}

/* serial_link_drain (an input script's wait) reads the server into the settling queue; a held reply
 * stays held, unprompted bytes are the guest's. */
static void test_drain(void)
{
    Machine *m = machine_create();
    Pipe p = { 0 };
    SerialLink l = settled(m, &p);
    char got[PIPE_BYTES];
    m->virtual_us = 100000;
    give(&p, "U");
    serial_link_drain(&l);
    CHECK(p.in_len == 0, "the drain read the server");
    CHECK(take(&l, got, sizeof got) == 1 && !strcmp(got, "U"), "unprompted drained bytes pass (%s)", got);
    l.send(l.ctx, (const uint8_t *)"Q", 1);
    give(&p, "R");
    serial_link_drain(&l);
    CHECK(p.in_len == 0 && take(&l, got, sizeof got) == 0, "a drained reply is still held");
    m->virtual_us += WINDOW_US;
    CHECK(take(&l, got, sizeof got) == 1 && !strcmp(got, "R"), "and released at the window");
    l.close(l.ctx);
    machine_destroy(m);
}

int main(void)
{
    test_hold_and_release();
    test_drain();
    test_env();
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("link settle: ok\n");
    return 0;
}
