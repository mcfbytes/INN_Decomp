/* Machine glue: memory slow paths, callback stubs, port dispatch and the scheduler. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lift.h"
#include "machine.h"
#include "native.h"

uint8_t mem_rd8_slow(Machine *m, uint32_t linear)
{
    if (mem_is_vga(linear) && !m->native_video)
        return vga_mem_read(m, linear);
    return m->mem[linear];
}

void mem_wr8_slow(Machine *m, uint32_t linear, uint8_t v)
{
    if (mem_is_vga(linear) && m->native_video)
        m->mem[linear] = v;
    else if (mem_is_vga(linear))
        vga_mem_write(m, linear, v);
    /* Option ROM and BIOS space is read-only. */
}

void mem_read_block(Machine *m, uint32_t linear, void *dst, size_t len)
{
    uint8_t *out = dst;
    for (size_t i = 0; i < len; i++)
        out[i] = mem_rd8(m, linear + (uint32_t)i);
}

void mem_write_block(Machine *m, uint32_t linear, const void *src, size_t len)
{
    const uint8_t *in = src;
    for (size_t i = 0; i < len; i++)
        mem_wr8(m, linear + (uint32_t)i, in[i]);
}

void mem_read_cstr(Machine *m, uint16_t seg, uint16_t off, char *dst, size_t cap)
{
    size_t i = 0;
    for (; i + 1 < cap; i++) {
        char ch = (char)mem_rd8(m, real_addr(seg, (uint16_t)(off + i)));
        if (!ch)
            break;
        dst[i] = ch;
    }
    dst[i] = 0;
}

uint32_t callback_stub_addr(int id)
{
    return real_addr(CALLBACK_SEG, (uint16_t)(CALLBACK_OFF_BASE + id * 4));
}

int callback_register(Machine *m, const char *name, CallbackFn fn, int vector)
{
    if (m->callback_count >= CALLBACK_MAX)
        fatal("callback table full registering %s", name);
    int id = m->callback_count++;
    m->callbacks[id] = fn;
    m->callback_names[id] = name;
    uint8_t *stub = m->mem + callback_stub_addr(id);
    stub[0] = 0xFE;
    stub[1] = 0x38;
    stub[2] = (uint8_t)id;
    stub[3] = 0xCF;
    if (vector >= 0) {
        uint16_t off = (uint16_t)(CALLBACK_OFF_BASE + id * 4);
        m->mem[vector * 4] = (uint8_t)off;
        m->mem[vector * 4 + 1] = (uint8_t)(off >> 8);
        m->mem[vector * 4 + 2] = (uint8_t)CALLBACK_SEG;
        m->mem[vector * 4 + 3] = (uint8_t)(CALLBACK_SEG >> 8);
    }
    return id;
}

void port_register(Machine *m, uint16_t first, uint16_t last, PortInFn in, PortOutFn out)
{
    for (uint32_t p = first; p <= last; p++) {
        m->port_in[p] = in;
        m->port_out[p] = out;
    }
}

/* VGA registers, whose state shadow verification snapshots and compares. */
static bool vga_port(uint16_t port)
{
    return port >= 0x3B0 && port <= 0x3DF;
}

static void end_slice(Machine *m);

/* Drivers time out on fixed poll counts, which only works if port I/O costs real bus time. Lifted
 * C runs no instructions, so its accesses advance time directly and a poll loop in C still sees
 * retrace, UART and timer state change; shadow runs keep time still (lift_verify.c). */
void machine_charge_c(Machine *m, uint32_t ns)
{
    if (m->native) {
        native_charge(m, ns);
        return;
    }
    if (m->io_mode != IO_LIVE || !lift_in_c(m)) {
        m->cpu.stall += (uint32_t)(m->ips * ns / 1000000000u);
        return;
    }
    m->c_io_ns += ns;
    if (m->c_io_ns >= TICK_SLICE_US * 1000u) {
        m->c_io_ns -= TICK_SLICE_US * 1000u;
        end_slice(m);
    }
}

static void charge_io(Machine *m)
{
    machine_charge_c(m, m->io_cost_ns);
}

static uint32_t device_in(Machine *m, uint16_t port, int size)
{
    if (size > 1 && (!m->port_in[port] || m->port_in[port] != m->port_in[(uint16_t)(port + 1)])) {
        uint32_t lo = device_in(m, port, 1), hi = device_in(m, (uint16_t)(port + 1), 1);
        uint32_t v = lo | hi << 8;
        if (size == 4)
            v |= device_in(m, (uint16_t)(port + 2), 2) << 16;
        return v;
    }
    if (m->port_in[port])
        return m->port_in[port](m, port, size);
    if (m->trace)
        log_msg("io: unhandled in %04X", port);
    return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFFu;
}

static void device_out(Machine *m, uint16_t port, uint32_t value, int size)
{
    if (size > 1 && (!m->port_out[port] || m->port_out[port] != m->port_out[(uint16_t)(port + 1)])) {
        device_out(m, port, value & 0xFF, 1);
        device_out(m, (uint16_t)(port + 1), value >> 8 & 0xFF, 1);
        if (size == 4)
            device_out(m, (uint16_t)(port + 2), value >> 16, 2);
        return;
    }
    if (m->port_out[port])
        m->port_out[port](m, port, value, size);
    else if (m->trace)
        log_msg("io: unhandled out %04X <- %X", port, value);
}

uint32_t port_in(Machine *m, uint16_t port, int size)
{
    if (__builtin_expect(m->native, 0))
        native_port_fatal(m, false, port);
    charge_io(m);
    if (m->io_mode == IO_REPLAY) {
        if (vga_port(port)) /* VGA state is part of the snapshot, so the lifted run drives it too */
            device_in(m, port, size);
        return lift_io_replay_in(m, port, size);
    }
    uint32_t v = device_in(m, port, size);
    if (m->io_mode == IO_RECORD)
        lift_io_record(m, port, size, v, false);
    return v;
}

void port_out(Machine *m, uint16_t port, uint32_t value, int size)
{
    if (__builtin_expect(m->native, 0))
        native_port_fatal(m, true, port);
    charge_io(m);
    if (m->io_mode == IO_REPLAY) {
        lift_io_replay_out(m, port, value, size);
        if (vga_port(port))
            device_out(m, port, value, size);
        return;
    }
    if (m->io_mode == IO_RECORD)
        lift_io_record(m, port, size, value, true);
    device_out(m, port, value, size);
}

Machine *machine_create(void)
{
    Machine *m = calloc(1, sizeof *m);
    if (!m)
        fatal("out of memory");
    m->mem = calloc(1, MEM_SIZE);
    if (!m->mem)
        fatal("out of memory");
    memset(m->mem + 0xC0000, 0xFF, MEM_ONE_MEG - 0xC0000);
    m->ips = 20000000;
    m->io_cost_ns = 1000;
    m->fpu_present = true;
    cpu_reset(m);
    native_init(m, NULL);
    lift_init(m);
    return m;
}

void machine_destroy(Machine *m)
{
    native_shutdown(m); /* reports per module, so before the lifted modules are forgotten */
    lift_shutdown(m);
    free(m->mem);
    free(m);
}

void machine_add_ticker(Machine *m, TickFn fn)
{
    if (m->ticker_count >= (int)(sizeof m->tickers / sizeof m->tickers[0]))
        fatal("ticker table full");
    m->tickers[m->ticker_count++] = fn;
}

static void end_slice(Machine *m)
{
    if (m->native) { /* the non-strict fallback: time passes, devices stay suspended */
        m->slice_left = m->ips * TICK_SLICE_US / 1000000;
        m->slice_start_insn = m->cpu.instructions;
        native_interp_time(m, TICK_SLICE_US);
        return;
    }
    if (m->ticks_frozen) { /* the replayed run must not move devices or pull bytes from a link */
        m->slice_left = m->ips * TICK_SLICE_US / 1000000;
        m->slice_start_insn = m->cpu.instructions;
        return;
    }
    m->virtual_us += TICK_SLICE_US;
    m->slice_left = m->ips * TICK_SLICE_US / 1000000;
    m->slice_start_insn = m->cpu.instructions;
    for (int i = 0; i < m->ticker_count; i++)
        m->tickers[i](m);
}

/* Time follows instructions executed, so nested guest calls from lifted code cost what they run. */
void machine_step(Machine *m)
{
    if (m->slice_left == 0)
        m->slice_left = m->ips * TICK_SLICE_US / 1000000;
    uint64_t done = cpu_run(m, m->slice_left);
    if (m->cpu.hook_pending) {
        m->cpu.hook_pending = false;
        uint32_t linear = m->cpu.seg_base[S_CS] + (m->cpu.eip & 0xFFFF);
        if (lift_dispatch(m, linear)) {
            done++;
            m->cpu.instructions++;
        } else {
            m->cpu.hook_skip = linear;
        }
    }
    m->slice_left -= done < m->slice_left ? done : m->slice_left;
    if (m->slice_left == 0 || m->cpu.halted || m->cpu.yielded)
        end_slice(m);
}

void machine_run_for(Machine *m, uint64_t us)
{
    uint64_t end = m->virtual_us + us;
    while (m->virtual_us < end && !m->exit_requested)
        machine_step(m);
}

uint64_t machine_now_ns(const Machine *m)
{
    enum { SLICE_NS = TICK_SLICE_US * 1000 };
    if (m->native)
        return m->virtual_us * 1000;
    uint64_t insn = m->cpu.instructions - m->slice_start_insn;
    uint64_t ns = m->ips ? insn * 1000000000u / m->ips : 0;
    return m->virtual_us * 1000 + (ns < SLICE_NS ? ns : SLICE_NS);
}

void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
