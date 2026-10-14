/* SerialLink over a browser WebSocket: binary frames carry raw serial bytes both ways.
 * The server end is innkeeperd --ws-bind; carrier follows the socket state. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/websocket.h>

enum { WS_INBOX_INITIAL = 4096 };

typedef enum WsState { WS_CONNECTING, WS_OPEN, WS_CLOSED } WsState;

/* Received bytes wait here until the UART paces them in; the socket offers no back-pressure,
 * so the inbox grows instead of dropping. */
typedef struct WsLink {
    EMSCRIPTEN_WEBSOCKET_T socket;
    WsState state;
    uint8_t *inbox;
    size_t head, len, cap;
} WsLink;

static bool inbox_reserve(WsLink *w, size_t extra)
{
    if (w->head) {
        memmove(w->inbox, w->inbox + w->head, w->len);
        w->head = 0;
    }
    if (w->len + extra <= w->cap)
        return true;
    size_t cap = w->cap ? w->cap : WS_INBOX_INITIAL;
    while (cap < w->len + extra)
        cap *= 2;
    uint8_t *grown = realloc(w->inbox, cap);
    if (!grown)
        return false;
    w->inbox = grown;
    w->cap = cap;
    return true;
}

static bool on_open(int type, const EmscriptenWebSocketOpenEvent *e, void *ctx)
{
    ((WsLink *)ctx)->state = WS_OPEN;
    log_msg("uart: websocket open");
    return true;
}

static bool on_message(int type, const EmscriptenWebSocketMessageEvent *e, void *ctx)
{
    WsLink *w = ctx;
    if (e->isText || !e->numBytes)
        return true;
    if (!inbox_reserve(w, e->numBytes)) {
        log_msg("uart: websocket inbox out of memory, dropping %u bytes", e->numBytes);
        return true;
    }
    memcpy(w->inbox + w->len, e->data, e->numBytes);
    w->len += e->numBytes;
    return true;
}

static bool on_error(int type, const EmscriptenWebSocketErrorEvent *e, void *ctx)
{
    ((WsLink *)ctx)->state = WS_CLOSED;
    log_msg("uart: websocket error");
    return true;
}

static bool on_close(int type, const EmscriptenWebSocketCloseEvent *e, void *ctx)
{
    ((WsLink *)ctx)->state = WS_CLOSED;
    log_msg("uart: websocket closed (code %u)", e->code);
    return true;
}

static int ws_send(void *ctx, const uint8_t *buf, int len)
{
    WsLink *w = ctx;
    if (w->state != WS_OPEN)
        return w->state == WS_CONNECTING ? 0 : -1;
    if (emscripten_websocket_send_binary(w->socket, (void *)buf, (uint32_t)len) != EMSCRIPTEN_RESULT_SUCCESS)
        return -1;
    return len;
}

static int ws_recv(void *ctx, uint8_t *buf, int cap)
{
    WsLink *w = ctx;
    size_t n = w->len < (size_t)cap ? w->len : (size_t)cap;
    memcpy(buf, w->inbox + w->head, n);
    w->head += n;
    w->len -= n;
    if (n == 0 && w->state == WS_CLOSED)
        return -1;
    return (int)n;
}

static bool ws_connected(void *ctx) { return ((WsLink *)ctx)->state == WS_OPEN; }

static void ws_close(void *ctx)
{
    WsLink *w = ctx;
    emscripten_websocket_close(w->socket, 1000, "client closed");
    emscripten_websocket_delete(w->socket);
    free(w->inbox);
    free(w);
}

static EMSCRIPTEN_WEBSOCKET_T ws_open(const char *url, WsLink *w)
{
    EmscriptenWebSocketCreateAttributes attrs;
    emscripten_websocket_init_create_attributes(&attrs);
    attrs.url = url;
    attrs.protocols = NULL;
    EMSCRIPTEN_WEBSOCKET_T socket = emscripten_websocket_new(&attrs);
    if (socket <= 0)
        return 0;
    emscripten_websocket_set_onopen_callback(socket, w, on_open);
    emscripten_websocket_set_onmessage_callback(socket, w, on_message);
    emscripten_websocket_set_onerror_callback(socket, w, on_error);
    emscripten_websocket_set_onclose_callback(socket, w, on_close);
    return socket;
}

int uart_attach_ws(Machine *m, int com, const char *url)
{
    if (!emscripten_websocket_is_supported())
        return -1;
    WsLink *w = calloc(1, sizeof *w);
    if (!w)
        return -1;
    w->socket = ws_open(url, w);
    if (!w->socket) {
        free(w);
        return -1;
    }
    SerialLink link = { w, ws_send, ws_recv, ws_connected, ws_close };
    if (uart_attach(m, com, link) != 0) {
        ws_close(w);
        return -1;
    }
    return 0;
}
#else
int uart_attach_ws(Machine *m, int com, const char *url)
{
    return -1; /* native builds reach the server over plain TCP (uart_attach_tcp) */
}
#endif
