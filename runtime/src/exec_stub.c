/* TSNEXEC stand-in: the INT 14h export table as callbacks, messages passed whole; the client's memory
 * callbacks run as nested guest calls (sibling docs/protocol/int14h-api.md, docs/runtime.md). */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "bios_internal.h"
#include "lift.h"
#include "exec_stub.h"

enum {
    EXPORTS = 17, SHARED_MAX = 256, QUEUE_MAX = 64, MSG_MAX = 1024, PENDING_FREE_MAX = 100,
    VEC_SERIAL = 0x14,
    STATUS_OK = 0, DRIVER_VERSION = 3, CONNECTED = 0x80, LINE_RATE = 2400,
    TICK_ADDR = 0x4F0, /* the BIOS inter-application area holds the client's tick counter */
};

typedef struct ExecMsg {
    uint16_t len;
    uint8_t bytes[MSG_MAX];
} ExecMsg;

typedef struct ExecStub {
    ExecSendHook on_send;
    void *user;
    uint16_t table_off, previous_off, shared_off;
    uint16_t shared_len;
    bool has_previous;
    uint32_t alloc_cb, free_cb, deref_cb; /* seg:off of the client's memory callbacks */
    ExecMsg queue[QUEUE_MAX];
    int q_head, q_count;
    uint32_t pending_free[PENDING_FREE_MAX];
    int pending_count;
    char next_program[64];
} ExecStub;

static ExecStub *stub(Machine *m) { return m->exec_stub; }

/* From the client callbacks to the pending frees: everything a Send, Receive or SetCallbacks changes. */
#define STATE_FROM offsetof(ExecStub, alloc_cb)
#define STATE_TO offsetof(ExecStub, next_program)

size_t exec_stub_state_size(void) { return STATE_TO - STATE_FROM; }

void exec_stub_state_save(Machine *m, void *dst)
{
    if (m->exec_stub)
        memcpy(dst, (uint8_t *)m->exec_stub + STATE_FROM, STATE_TO - STATE_FROM);
    else
        memset(dst, 0, STATE_TO - STATE_FROM); /* snapshots are memcmp'd whole */
}

void exec_stub_state_load(Machine *m, const void *src)
{
    if (m->exec_stub)
        memcpy((uint8_t *)m->exec_stub + STATE_FROM, src, STATE_TO - STATE_FROM);
}

static uint16_t arg16(Machine *m, int index) /* far cdecl: args after the 4-byte return address */
{
    return mem_rd16(m, m->cpu.seg_base[S_SS] + (uint16_t)(m->cpu.r[R_SP].w + 4 + 2 * index));
}

static uint32_t arg_far(Machine *m, int index) /* a far pointer as seg << 16 | off */
{
    return (uint32_t)arg16(m, index + 1) << 16 | arg16(m, index);
}

static uint32_t far_linear(uint32_t p) { return real_addr((uint16_t)(p >> 16), (uint16_t)p); }

static void ret16(Machine *m, uint16_t v) { m->cpu.r[R_AX].w = v; }

static void ret_far(Machine *m, uint32_t p)
{
    m->cpu.r[R_AX].w = (uint16_t)p;
    m->cpu.r[R_DX].w = (uint16_t)(p >> 16);
}

/* Calls a client memory callback (far cdecl) with arguments pushed right to left; returns DX:AX.
 * Callee-saved registers are kept as TSNEXEC's wrapper keeps them. */
static uint32_t call_client(Machine *m, uint32_t fn, const uint16_t *args, int n)
{
    Cpu saved = m->cpu;
    for (int i = n - 1; i >= 0; i--)
        cpu_push16(m, args[i]);
    guest_call_far(m, (uint16_t)(fn >> 16), (uint16_t)fn);
    uint32_t result = (uint32_t)m->cpu.r[R_DX].w << 16 | m->cpu.r[R_AX].w;
    Fpu fpu = m->cpu.fpu;
    uint64_t insns = m->cpu.instructions;
    uint32_t stall = m->cpu.stall;
    m->cpu = saved;
    m->cpu.fpu = fpu;
    m->cpu.instructions = insns;
    m->cpu.stall = stall;
    return result;
}

static uint32_t deref(Machine *m, uint32_t handle)
{
    ExecStub *t = stub(m);
    if (!t->deref_cb)
        return 0;
    uint16_t args[2] = {(uint16_t)handle, (uint16_t)(handle >> 16)};
    return call_client(m, t->deref_cb, args, 2);
}

static void free_pending(Machine *m)
{
    ExecStub *t = stub(m);
    int n = t->pending_count;
    t->pending_count = 0;
    for (int i = 0; i < n && t->free_cb; i++) {
        uint16_t args[2] = {(uint16_t)t->pending_free[i], (uint16_t)(t->pending_free[i] >> 16)};
        call_client(m, t->free_cb, args, 2);
    }
}

/* ---- exports ---- */

static CallbackResult x_get_status(Machine *m) { ret16(m, CONNECTED << 8 | DRIVER_VERSION); return CB_DONE; }

static CallbackResult x_get_shared(Machine *m)
{
    ExecStub *t = stub(m);
    uint32_t out = far_linear(arg_far(m, 0));
    mem_wr16(m, out, t->shared_off);
    mem_wr16(m, out + 2, BIOS_SEG);
    ret16(m, t->shared_len);
    return CB_DONE;
}

static CallbackResult x_set_shared(Machine *m)
{
    ExecStub *t = stub(m);
    uint32_t src = arg_far(m, 0);
    uint16_t len = arg16(m, 2);
    if (!src || !len) {
        t->shared_len = 0;
        return CB_DONE;
    }
    len = len > SHARED_MAX ? SHARED_MAX : len;
    for (uint16_t i = 0; i < len; i++)
        m->mem[real_addr(BIOS_SEG, t->shared_off) + i] = mem_rd8(m, far_linear(src) + i);
    t->shared_len = len;
    return CB_DONE;
}

static CallbackResult x_connect(Machine *m) { ret16(m, STATUS_OK); return CB_DONE; }

static CallbackResult x_send(Machine *m)
{
    ExecStub *t = stub(m);
    uint32_t handle = arg_far(m, 0);
    uint16_t len = arg16(m, 2);
    if (!len || len > MSG_MAX) {
        ret16(m, 0);
        return CB_DONE;
    }
    uint32_t p = deref(m, handle);
    uint8_t buf[MSG_MAX];
    for (uint16_t i = 0; i < len; i++)
        buf[i] = mem_rd8(m, far_linear(p) + i);
    if (t->pending_count < PENDING_FREE_MAX - 1)
        t->pending_free[t->pending_count++] = handle;
    if (t->on_send)
        t->on_send(m, buf, len, t->user);
    ret16(m, 1);
    return CB_DONE;
}

static CallbackResult x_receive(Machine *m)
{
    ExecStub *t = stub(m);
    uint32_t out = far_linear(arg_far(m, 0));
    if (!t->q_count || !t->alloc_cb) {
        mem_wr16(m, out, 0);
        mem_wr16(m, out + 2, 0);
        ret16(m, 0);
        return CB_DONE;
    }
    ExecMsg *msg = &t->queue[t->q_head];
    uint16_t size = msg->len;
    uint32_t handle = call_client(m, t->alloc_cb, &size, 1);
    if (!handle) {
        ret16(m, 0);
        return CB_DONE;
    }
    uint32_t p = far_linear(deref(m, handle));
    for (uint16_t i = 0; i < msg->len; i++)
        mem_wr8(m, p + i, msg->bytes[i]);
    t->q_head = (t->q_head + 1) % QUEUE_MAX;
    t->q_count--;
    mem_wr16(m, out, (uint16_t)handle);
    mem_wr16(m, out + 2, (uint16_t)(handle >> 16));
    ret16(m, size);
    return CB_DONE;
}

static CallbackResult x_set_ack_timeout(Machine *m) { ret_far(m, TICK_ADDR); return CB_DONE; }
static CallbackResult x_disconnect(Machine *m) { return CB_DONE; }

static CallbackResult x_poll(Machine *m)
{
    free_pending(m);
    ret16(m, STATUS_OK);
    return CB_DONE;
}

static CallbackResult x_set_next_program(Machine *m)
{
    ExecStub *t = stub(m);
    uint32_t name = arg_far(m, 0);
    if (name)
        mem_read_cstr(m, (uint16_t)(name >> 16), (uint16_t)name, t->next_program, sizeof t->next_program);
    else
        t->next_program[0] = 0;
    log_msg("tsn: next program '%s'", t->next_program);
    ret16(m, 1);
    return CB_DONE;
}

static CallbackResult x_service(Machine *m) { ret16(m, STATUS_OK); return CB_DONE; }

static CallbackResult x_get_previous(Machine *m)
{
    ExecStub *t = stub(m);
    ret_far(m, t->has_previous ? (uint32_t)BIOS_SEG << 16 | t->previous_off : 0);
    return CB_DONE;
}

static CallbackResult x_is_transmit_idle(Machine *m) { ret16(m, 1); return CB_DONE; }
static CallbackResult x_switch_host(Machine *m) { ret16(m, STATUS_OK); return CB_DONE; }
static CallbackResult x_flush(Machine *m) { return CB_DONE; }

static CallbackResult x_set_callbacks(Machine *m)
{
    ExecStub *t = stub(m);
    t->alloc_cb = arg_far(m, 0);
    t->deref_cb = arg_far(m, 2);
    t->free_cb = arg_far(m, 4);
    return CB_DONE;
}

static CallbackResult x_get_line_rate(Machine *m) { ret16(m, LINE_RATE); return CB_DONE; }

static CallbackResult int14_discover(Machine *m)
{
    ret_far(m, (uint32_t)BIOS_SEG << 16 | stub(m)->table_off);
    return CB_DONE;
}

/* ---- set-up ---- */

void exec_stub_install(Machine *m, ExecSendHook on_send, void *user)
{
    static const struct { const char *name; CallbackFn fn; } EXPORT[EXPORTS] = {
        {"exec_get_status", x_get_status}, {"exec_get_shared", x_get_shared},
        {"exec_set_shared", x_set_shared}, {"exec_connect", x_connect},
        {"exec_send", x_send}, {"exec_receive", x_receive},
        {"exec_set_ack_timeout", x_set_ack_timeout}, {"exec_disconnect", x_disconnect},
        {"exec_poll", x_poll}, {"exec_set_next_program", x_set_next_program},
        {"exec_service", x_service}, {"exec_get_previous", x_get_previous},
        {"exec_is_transmit_idle", x_is_transmit_idle}, {"exec_switch_host", x_switch_host},
        {"exec_flush", x_flush}, {"exec_set_callbacks", x_set_callbacks},
        {"exec_get_line_rate", x_get_line_rate},
    };
    ExecStub *t = calloc(1, sizeof *t);
    if (!t)
        fatal("out of memory");
    m->exec_stub = t;
    t->on_send = on_send;
    t->user = user;
    uint8_t table[EXPORTS * 4];
    for (int i = 0; i < EXPORTS; i++) {
        int id = callback_register(m, EXPORT[i].name, EXPORT[i].fn, -1);
        const uint8_t stub[4] = {0xFE, 0x38, (uint8_t)id, 0xCB}; /* callback, RETF */
        uint16_t off = bios_rom_emit(m, stub, sizeof stub);
        table[4 * i] = (uint8_t)off, table[4 * i + 1] = (uint8_t)(off >> 8);
        table[4 * i + 2] = (uint8_t)BIOS_SEG, table[4 * i + 3] = (uint8_t)(BIOS_SEG >> 8);
    }
    t->table_off = bios_rom_emit(m, table, sizeof table);
    static const uint8_t zero[SHARED_MAX + 64];
    t->shared_off = bios_rom_emit(m, zero, SHARED_MAX);
    t->previous_off = bios_rom_emit(m, zero, 64);
    callback_register(m, "int14_exec", int14_discover, VEC_SERIAL);
}

void exec_stub_set_send_hook(Machine *m, ExecSendHook on_send, void *user)
{
    stub(m)->on_send = on_send;
    stub(m)->user = user;
}

void exec_stub_shutdown(Machine *m)
{
    free(m->exec_stub);
    m->exec_stub = NULL;
}

void exec_stub_set_shared(Machine *m, uint16_t offset, const uint8_t *data, uint16_t len)
{
    ExecStub *t = stub(m);
    if (offset >= SHARED_MAX)
        return;
    if (len > SHARED_MAX - offset)
        len = (uint16_t)(SHARED_MAX - offset);
    memcpy(m->mem + real_addr(BIOS_SEG, t->shared_off) + offset, data, len);
    if (offset + len > t->shared_len)
        t->shared_len = (uint16_t)(offset + len);
}

void exec_stub_set_previous(Machine *m, const char *name)
{
    ExecStub *t = stub(m);
    size_t n = strlen(name);
    n = n > 63 ? 63 : n;
    memcpy(m->mem + real_addr(BIOS_SEG, t->previous_off), name, n);
    m->mem[real_addr(BIOS_SEG, t->previous_off) + n] = 0;
    t->has_previous = true;
}

void exec_stub_deliver(Machine *m, const uint8_t *msg, uint16_t len)
{
    ExecStub *t = stub(m);
    if (t->q_count == QUEUE_MAX || len > MSG_MAX) {
        log_msg("tsn: host message dropped (queue full or %u bytes)", len);
        return;
    }
    ExecMsg *q = &t->queue[(t->q_head + t->q_count) % QUEUE_MAX];
    q->len = len;
    memcpy(q->bytes, msg, len);
    t->q_count++;
}
