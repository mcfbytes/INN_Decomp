/* Host link (docs/native.md section 9): open, send, receive, carrier, break, the pump and its
 * throttle, and the TCP constructor against a loopback listener. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __EMSCRIPTEN__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "devices.h"
#include "host_link.h"

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

enum { PIPE_BYTES = 64, LOOPBACK_PORT = 2719, THROTTLE_US = 1000 };

typedef struct Pipe {
    uint8_t out[PIPE_BYTES], in[PIPE_BYTES];
    int out_len, in_len;
    bool up, closed;
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
    int n = p->in_len < cap ? p->in_len : cap;
    memcpy(buf, p->in, (size_t)n);
    memmove(p->in, p->in + n, (size_t)(p->in_len - n));
    p->in_len -= n;
    return n;
}

static bool pipe_connected(void *ctx) { return ((Pipe *)ctx)->up; }
static void pipe_close(void *ctx) { ((Pipe *)ctx)->closed = true; }

static int pumped;
static void count_pump(Machine *m) { pumped++; }

static void test_pipe(void)
{
    Machine *m = machine_create();
    uint8_t buf[8];
    CHECK(!host_link_carrier(m), "no carrier without a link");
    CHECK(host_link_send(m, (const uint8_t *)"x", 1) < 0, "send without a link fails");
    CHECK(host_link_recv(m, buf, sizeof buf) == 0, "nothing to receive without a link");

    Pipe p = { .up = false };
    SerialLink link = { &p, pipe_send, pipe_recv, pipe_connected, pipe_close };
    CHECK(host_link_attach(m, link) == 0, "attach");
    CHECK(!host_link_carrier(m), "no carrier while connecting");
    p.up = true;
    CHECK(host_link_carrier(m), "carrier once connected");

    CHECK(host_link_send(m, (const uint8_t *)"ATZ", 3) == 3, "send takes 3 bytes");
    CHECK(p.out_len == 3 && !memcmp(p.out, "ATZ", 3), "the link got the bytes");
    memcpy(p.in, "OK\r\n", 4);
    p.in_len = 4;
    CHECK(host_link_recv(m, buf, 2) == 2 && !memcmp(buf, "OK", 2), "recv honours the capacity");
    CHECK(host_link_recv(m, buf, sizeof buf) == 2 && !memcmp(buf, "\r\n", 2), "recv returns the rest");

    p.out_len = 0;
    host_link_break(m);
    CHECK(p.out_len == 1 && p.out[0] == '\r', "break sends the CR stand-in");
    host_link_set_dtr(m, false);
    host_link_set_dtr(m, true);

    int tickers = m->ticker_count;
    host_link_set_pump(m, count_pump);
    CHECK(m->ticker_count == tickers + 1, "an emulated machine gets a ticker");
    host_link_set_pump(m, count_pump);
    CHECK(m->ticker_count == tickers + 1, "a second registration adds none");
    pumped = 0;
    m->virtual_us = 5000;
    host_link_pump(m);
    host_link_pump(m);
    CHECK(pumped == 1, "the pump is throttled (ran %d times)", pumped);
    m->virtual_us += THROTTLE_US;
    m->tickers[m->ticker_count - 1](m);
    CHECK(pumped == 2, "the ticker runs the pump after a millisecond");
    m->virtual_us += THROTTLE_US;
    m->irq_hold = true;
    host_link_pump(m);
    CHECK(pumped == 2, "the pump waits while interrupts are held");
    m->irq_hold = false;
    host_link_set_pump(m, NULL);
    host_link_pump(m);
    CHECK(pumped == 2, "an unregistered pump does not run");

    host_link_close(m);
    CHECK(p.closed, "close closes the link");
    CHECK(!host_link_carrier(m), "no carrier after close");
    machine_destroy(m);
}

static int pump_sends;
static void sending_pump(Machine *m)
{
    pump_sends++;
    host_link_send(m, (const uint8_t *)"p", 1); /* the pump's own calls are not safe points */
}

static void test_pump_calls(void)
{
    Machine *m = machine_create();
    Pipe p = { .up = true };
    SerialLink link = { &p, pipe_send, pipe_recv, pipe_connected, pipe_close };
    host_link_attach(m, link);
    host_link_set_pump(m, sending_pump);
    pump_sends = 0;
    m->virtual_us = 1;
    host_link_pump(m);
    CHECK(pump_sends == 1 && p.out_len == 1, "a pump can use the link");
    host_link_kick(m);
    CHECK(pump_sends == 2, "a kick ignores the throttle");
    host_link_close(m);
    machine_destroy(m);
}

static void test_open_failures(void)
{
    Machine *m = machine_create();
    CHECK(host_link_open(m, "nowhere") == -1, "a spec without a port fails");
    CHECK(host_link_open(m, ":2710") == -1, "a spec without a host fails");
    CHECK(host_link_attach(m, (SerialLink){ 0 }) == -1, "an empty link is refused");
    SerialLink ws = serial_link_ws("ws://127.0.0.1:1/");
#ifndef __EMSCRIPTEN__
    CHECK(!ws.send, "native builds have no WebSocket link");
#endif
    if (ws.close)
        ws.close(ws.ctx);
    machine_destroy(m);
}

#ifndef __EMSCRIPTEN__
static int listen_loopback(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(LOOPBACK_PORT) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void test_tcp(void)
{
    int server = listen_loopback();
    if (server < 0) {
        fprintf(stderr, "skip tcp: port %d busy\n", LOOPBACK_PORT);
        return;
    }
    Machine *m = machine_create();
    CHECK(host_link_open(m, "127.0.0.1:2719") == 0, "open");
    int peer = accept(server, NULL, NULL);
    CHECK(peer >= 0, "the listener sees the connection");
    for (int i = 0; i < 100 && !host_link_carrier(m); i++)
        usleep(1000);
    CHECK(host_link_carrier(m), "carrier once the connect finished");
    CHECK(host_link_send(m, (const uint8_t *)"hello", 5) == 5, "send");
    char got[8] = { 0 };
    CHECK(read(peer, got, sizeof got) == 5 && !memcmp(got, "hello", 5), "the peer reads what was sent");
    CHECK(write(peer, "yo", 2) == 2, "peer writes");
    uint8_t buf[8];
    int n = 0;
    for (int i = 0; i < 100 && n == 0; i++) {
        n = host_link_recv(m, buf, sizeof buf);
        if (n == 0)
            usleep(1000);
    }
    CHECK(n == 2 && !memcmp(buf, "yo", 2), "recv (%d)", n);
    close(peer);
    n = 0;
    for (int i = 0; i < 100 && n == 0; i++) {
        n = host_link_recv(m, buf, sizeof buf);
        if (n == 0)
            usleep(1000);
    }
    CHECK(n < 0, "a closed peer reads as a broken link (%d)", n);
    CHECK(!host_link_carrier(m), "carrier drops with the peer");
    host_link_close(m);
    machine_destroy(m);
    close(server);
}
#endif

int main(void)
{
    test_pipe();
    test_pump_calls();
    test_open_failures();
#ifndef __EMSCRIPTEN__
    test_tcp();
#endif
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    puts("test_host_link: ok");
    return 0;
}
