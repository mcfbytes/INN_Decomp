/* SerialLink backends: a raw TCP client (native builds) and an in-memory pipe for tests. */
#define _POSIX_C_SOURCE 200809L /* getaddrinfo under strict C17 */
#include <stdlib.h>
#include <string.h>

#include "devices.h"

/* ---- in-memory pipe ---- */

enum { PIPE_SIZE = 4096 };

typedef struct Ring { uint8_t data[PIPE_SIZE]; int head, count; } Ring;
typedef struct Pipe { Ring to_guest, from_guest; } Pipe;

static int ring_put(Ring *r, const uint8_t *buf, int len)
{
    int n = 0;
    for (; n < len && r->count < PIPE_SIZE; n++)
        r->data[(r->head + r->count++) % PIPE_SIZE] = buf[n];
    return n;
}

static int ring_get(Ring *r, uint8_t *buf, int cap)
{
    int n = 0;
    for (; n < cap && r->count; n++, r->count--) {
        buf[n] = r->data[r->head];
        r->head = (r->head + 1) % PIPE_SIZE;
    }
    return n;
}

static int pipe_send(void *ctx, const uint8_t *buf, int len) { return ring_put(&((Pipe *)ctx)->from_guest, buf, len); }
static int pipe_recv(void *ctx, uint8_t *buf, int cap) { return ring_get(&((Pipe *)ctx)->to_guest, buf, cap); }
static bool pipe_connected(void *ctx) { return true; }
static void pipe_close(void *ctx) { free(ctx); }

int uart_attach_pipe(Machine *m, int com)
{
    Pipe *p = calloc(1, sizeof *p);
    if (!p)
        return -1;
    SerialLink link = { p, pipe_send, pipe_recv, pipe_connected, pipe_close };
    if (uart_attach(m, com, link) != 0) {
        free(p);
        return -1;
    }
    return 0;
}

static Pipe *pipe_of(Machine *m, int com)
{
    SerialLink *l = uart_link(m, com);
    return l && l->send == pipe_send ? l->ctx : NULL;
}

int uart_test_give(Machine *m, int com, const uint8_t *buf, int len)
{
    Pipe *p = pipe_of(m, com);
    return p ? ring_put(&p->to_guest, buf, len) : -1;
}

int uart_test_take(Machine *m, int com, uint8_t *buf, int cap)
{
    Pipe *p = pipe_of(m, com);
    return p ? ring_get(&p->from_guest, buf, cap) : -1;
}

/* ---- TCP client ---- */

#ifndef __EMSCRIPTEN__
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef struct TcpLink {
    int fd;
    bool connecting, up;
} TcpLink;

/* A non-blocking connect finishes when the socket turns writable; SO_ERROR says how. */
static bool tcp_connected(void *ctx)
{
    TcpLink *t = ctx;
    if (!t->connecting)
        return t->up;
    struct pollfd pfd = { t->fd, POLLOUT, 0 };
    if (poll(&pfd, 1, 0) <= 0)
        return false;
    int err = 0;
    socklen_t len = sizeof err;
    getsockopt(t->fd, SOL_SOCKET, SO_ERROR, &err, &len);
    t->connecting = false;
    t->up = err == 0;
    if (!t->up)
        log_msg("uart: tcp connect failed: %s", strerror(err));
    return t->up;
}

static int tcp_send(void *ctx, const uint8_t *buf, int len)
{
    TcpLink *t = ctx;
    if (!tcp_connected(t))
        return t->connecting ? 0 : -1;
    ssize_t n = send(t->fd, buf, (size_t)len, MSG_NOSIGNAL);
    if (n >= 0)
        return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return 0;
    t->up = false;
    return -1;
}

static int tcp_recv(void *ctx, uint8_t *buf, int cap)
{
    TcpLink *t = ctx;
    if (!tcp_connected(t))
        return 0;
    ssize_t n = recv(t->fd, buf, (size_t)cap, 0);
    if (n > 0)
        return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;
    t->up = false; /* peer closed: carrier drops */
    return -1;
}

static void tcp_close(void *ctx)
{
    TcpLink *t = ctx;
    close(t->fd);
    free(t);
}

static int tcp_open(const char *host, int port)
{
    char service[16];
    snprintf(service, sizeof service, "%d", port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(host, service, &hints, &res) != 0 || !res)
        return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        if (connect(fd, res->ai_addr, res->ai_addrlen) != 0 && errno != EINPROGRESS) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    return fd;
}

int uart_attach_tcp(Machine *m, int com, const char *host, int port)
{
    int fd = tcp_open(host, port);
    if (fd < 0)
        return -1;
    TcpLink *t = calloc(1, sizeof *t);
    if (!t) {
        close(fd);
        return -1;
    }
    t->fd = fd;
    t->connecting = true;
    SerialLink link = { t, tcp_send, tcp_recv, tcp_connected, tcp_close };
    if (uart_attach(m, com, link) != 0) {
        tcp_close(t);
        return -1;
    }
    return 0;
}
#else
int uart_attach_tcp(Machine *m, int com, const char *host, int port)
{
    return -1; /* the browser build reaches the server through a WebSocket link instead */
}
#endif
