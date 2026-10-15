/* Native core (docs/native.md sections 5 and 6): the clock, the interrupt model, delivery at safe
 * points, ROM lifts and strict mode. Built with Emscripten (INN_NATIVE, JSPI) it also proves that the
 * C stack suspends inside a setjmp scope and longjmps out afterwards (section 4). */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "bios_internal.h"
#include "devices.h"
#include "host_platform.h"
#include "host_timer.h"
#include "lift.h"
#include "native.h"
#include "native_internal.h"

static int failures;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

enum {
    CODE_SEG = 0x1000, STACK_SEG = 0x2000, STACK_TOP = 0x800,
    BODY_OFF = 0x10,  /* original: mov ax,1111h / retf; lifted */
    ISR_OFF = 0x20,   /* original: inc word [500h] / iret; lifted as a handler that checks IF */
    PLAIN_OFF = 0x30, /* mov ax,3333h / retf: not lifted */
    SLEEPY_OFF = 0x40, /* nop / retf; lifted as a function that suspends (JSPI proof) */
    PROGRAM_BYTES = 0x60,
    CALLER_CS = 0x3000, CALLER_IP = 0x42,
    LIFTED_AX = 0xAAAA,
    DIVISOR_100HZ = 11932, /* 99.998 Hz */
};

static const uint8_t PROGRAM[PROGRAM_BYTES] = {
    [BODY_OFF] = 0xB8, 0x11, 0x11, 0xCB,
    [ISR_OFF] = 0xFF, 0x06, 0x00, 0x05, 0xCF,
    [PLAIN_OFF] = 0xB8, 0x33, 0x33, 0xCB,
    [SLEEPY_OFF] = 0x90, 0xCB,
};

static void lifted_body(Machine *m)
{
    AX = LIFTED_AX;
    lift_ret_far(m, 0);
}

static int isr_calls;
static bool isr_saw_if;
static void lifted_isr(Machine *m)
{
    isr_calls++;
    isr_saw_if = (m->cpu.flags & F_IF) != 0;
    lift_ret_iret(m);
}

static void lifted_sleepy(Machine *m);

static LiftedEntry entries[3];

static void add_entry(int i, uint16_t off, LiftedFn fn, const char *name)
{
    entries[i] = (LiftedEntry){ .module = "TEST.EXE", .off = off, .fn = fn, .name = name,
                                .scratch_regs = LIFT_REG_FLAGS, .no_shadow = true };
    memcpy(entries[i].signature, PROGRAM + off, LIFT_SIGNATURE_LEN);
}

static Machine *make_machine(bool with_bios)
{
    Machine *m = machine_create();
    if (with_bios)
        bios_init(m);
    NativeConfig cfg = { .deterministic = true, .strict = true, .call_cost_ns = 1000, .yield_every_us = 4000 };
    native_init(m, &cfg);
    native_set_platform(m, NULL);
    memcpy(m->mem + real_addr(CODE_SEG, 0), PROGRAM, sizeof PROGRAM);
    add_entry(0, BODY_OFF, lifted_body, "body");
    add_entry(1, ISR_OFF, lifted_isr, "isr");
    add_entry(2, SLEEPY_OFF, lifted_sleepy, "sleepy");
    lift_add_table(m, entries, 3);
    lift_module_loaded(m, "TEST.EXE", CODE_SEG, sizeof PROGRAM);
    cpu_set_seg(&m->cpu, S_CS, CALLER_CS);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    SP = STACK_TOP;
    m->cpu.eip = CALLER_IP;
    m->cpu.flags |= F_IF;
    native_set_active(m, true);
    return m;
}

static void set_vector(Machine *m, int vector, uint16_t seg, uint16_t off)
{
    mem_wr16(m, (uint32_t)vector * 4, off);
    mem_wr16(m, (uint32_t)vector * 4 + 2, seg);
}

/* Service stubs as interrupt handlers: each records its vector; the EOI is the test's business. */
static int irq_order[32], irq_count, vector_calls[256];
static int stub_vector[CALLBACK_MAX];
static bool eoi_in_handler;

static CallbackResult record_irq(Machine *m)
{
    int id = mem_rd8(m, m->cpu.seg_base[S_CS] + (uint16_t)(m->cpu.eip - 1));
    int vector = stub_vector[id];
    vector_calls[vector]++;
    if (irq_count < 32)
        irq_order[irq_count] = vector;
    irq_count++;
    if (eoi_in_handler) {
        if (vector >= 0x70)
            native_pic_eoi(m, 1, -1);
        native_pic_eoi(m, 0, -1);
    }
    return CB_DONE;
}

static void install_recorders(Machine *m)
{
    static const char *const names[16] = { "irq0", "irq1", "irq2", "irq3", "irq4", "irq5", "irq6", "irq7",
                                            "irq8", "irq9", "irq10", "irq11", "irq12", "irq13", "irq14", "irq15" };
    for (int irq = 0; irq < 16; irq++) {
        int vector = irq < 8 ? 0x08 + irq : 0x70 + irq - 8;
        stub_vector[callback_register(m, names[irq], record_irq, vector)] = vector;
    }
    irq_count = 0;
    memset(vector_calls, 0, sizeof vector_calls);
    eoi_in_handler = false;
}

static void check_caller_state(Machine *m, const char *what)
{
    CHECK(SP == STACK_TOP && m->cpu.seg[S_CS] == CALLER_CS && (m->cpu.eip & 0xFFFF) == CALLER_IP,
          "%s: caller state restored (SP=%04X CS:IP=%04X:%04X)", what, SP, m->cpu.seg[S_CS],
          (unsigned)(m->cpu.eip & 0xFFFF));
}

static void test_clock(void)
{
    Machine *m = make_machine(false);
    native_pic_set_mask(m, 0, 0xFF); /* nothing to deliver: waits are pure clock jumps */
    uint64_t t0 = m->virtual_us;
    native_charge(m, 400);
    native_charge(m, 400);
    CHECK(m->virtual_us == t0, "charges below 1 us accumulate (%llu)", (unsigned long long)(m->virtual_us - t0));
    native_charge(m, 400);
    CHECK(m->virtual_us == t0 + 1, "and carry into the clock");
    native_safepoint(m);
    CHECK(m->virtual_us == t0 + 2 && machine_now_ns(m) == m->virtual_us * 1000,
          "a safe point charges call_cost_ns; machine_now_ns is the native clock");
    native_wait_until(m, t0 + 123456);
    CHECK(m->virtual_us == t0 + 123456, "a wait jumps the deterministic clock (%llu)",
          (unsigned long long)(m->virtual_us - t0));
    guest_call_far(m, CODE_SEG, BODY_OFF);
    CHECK(AX == LIFTED_AX && m->virtual_us == t0 + 123457, "a dispatched call is charged once");
    check_caller_state(m, "clock");
    machine_destroy(m);
}

static void test_priority(void)
{
    Machine *m = make_machine(false);
    install_recorders(m);
    native_irq_raise(m, 3);
    native_irq_raise(m, 10);
    native_irq_raise(m, 1);
    CHECK(native_deliver_irq(m) && irq_count == 1 && irq_order[0] == 0x09, "IRQ 1 first (vector %02X)", irq_order[0]);
    CHECK(!native_deliver_irq(m), "in-service IRQ 1 blocks everything below it");
    native_pic_eoi(m, 0, -1);
    CHECK(native_deliver_irq(m) && irq_order[1] == 0x72, "then IRQ 10 through the cascade (vector %02X)", irq_order[1]);
    CHECK(!native_deliver_irq(m), "the cascade line in service blocks IRQ 3");
    native_pic_eoi(m, 1, -1);
    native_pic_eoi(m, 0, -1);
    CHECK(native_deliver_irq(m) && irq_order[2] == 0x0B, "then IRQ 3 (vector %02X)", irq_order[2]);
    native_pic_eoi(m, 0, -1);
    CHECK(!native_deliver_irq(m) && irq_count == 3, "each request is delivered once");

    native_pic_set_mask(m, 0, 0x08);
    CHECK(native_pic_mask(m, 0) == 0x08 && native_pic_mask(m, 1) == 0, "masks read back");
    native_irq_raise(m, 3);
    CHECK(!native_deliver_irq(m), "a masked line is not delivered");
    native_pic_set_mask(m, 0, 0);
    CHECK(native_deliver_irq(m) && irq_order[3] == 0x0B, "unmasked, it is");
    native_pic_eoi(m, 0, -1);

    native_irq_raise(m, 4);
    CHECK(native_deliver_irq(m) && irq_order[4] == 0x0C, "IRQ 4 in service");
    native_irq_raise(m, 0);
    CHECK(native_deliver_irq(m) && irq_order[5] == 0x08, "IRQ 0 nests above it");
    native_pic_eoi(m, 0, 4);
    native_irq_raise(m, 5);
    CHECK(!native_deliver_irq(m), "after a specific EOI of 4, IRQ 0 still blocks IRQ 5");
    native_pic_eoi(m, 0, -1);
    CHECK(native_deliver_irq(m) && irq_order[6] == 0x0D, "the non-specific EOI clears IRQ 0, and 5 runs");
    native_pic_eoi(m, 0, -1);
    native_irq_raise(m, 6);
    native_irq_lower(m, 6);
    CHECK(!native_deliver_irq(m), "a lowered request is withdrawn");
    check_caller_state(m, "priority");
    machine_destroy(m);
}

static void test_if(void)
{
    Machine *m = make_machine(false);
    install_recorders(m);
    eoi_in_handler = true;
    m->cpu.flags &= ~(uint32_t)F_IF;
    native_irq_raise(m, 1);
    native_safepoint(m);
    CHECK(irq_count == 0, "no delivery with IF clear");
    guest_call_far(m, CODE_SEG, BODY_OFF);
    CHECK(irq_count == 0, "not at a dispatched call either");
    m->cpu.flags |= F_IF;
    guest_call_far(m, CODE_SEG, BODY_OFF);
    CHECK(irq_count == 1 && irq_order[0] == 0x09 && AX == LIFTED_AX, "a dispatched call with IF set delivers it first");
    CHECK(native_state(m)->pic[0].isr == 0, "the handler's EOI cleared it");
    CHECK(m->cpu.flags & F_IF, "the handler's IRET restored IF");
    check_caller_state(m, "if");
    machine_destroy(m);
}

static int count_ticks(Machine *m, uint16_t divisor, uint64_t us)
{
    install_recorders(m);
    eoi_in_handler = true;
    host_timer_set_divisor(m, divisor);
    native_wait_until(m, m->virtual_us + us);
    return vector_calls[0x08];
}

static void test_timer(void)
{
    Machine *m = make_machine(false);
    int ticks = count_ticks(m, DIVISOR_100HZ, 1000000);
    CHECK(ticks == 99, "divisor 11932: 99 ticks in 1 s, got %d", ticks);
    CHECK(host_timer_divisor(m) == DIVISOR_100HZ, "divisor reads back");
    ticks = count_ticks(m, 0, 1000000);
    CHECK(ticks == 18, "divisor 0 (65536): 18 ticks in 1 s, got %d", ticks);
    check_caller_state(m, "timer");

    /* Almost a second without safe points: only the last 5 of 99 ticks are delivered, one per safe point. */
    install_recorders(m);
    eoi_in_handler = true;
    host_timer_set_divisor(m, DIVISOR_100HZ);
    native_charge(m, 995000000u);
    native_safepoint(m);
    CHECK(irq_count == 1, "one tick per safe point (%d)", irq_count);
    for (int i = 0; i < 20; i++)
        native_safepoint(m);
    CHECK(irq_count == HOST_TIMER_MAX_CATCH_UP, "catch-up is capped at %d ticks, got %d", HOST_TIMER_MAX_CATCH_UP,
          irq_count);
    uint64_t next = host_timer_next_due_us(m);
    CHECK(next > m->virtual_us && next <= m->virtual_us + 10001, "then the next tick is due within a period");
    machine_destroy(m);
}

static void test_native_int(void)
{
    Machine *m = make_machine(false);
    set_vector(m, 0x61, CODE_SEG, ISR_OFF);
    isr_calls = 0;
    m->cpu.flags |= F_IF | F_CF;
    uint64_t before = m->cpu.instructions;
    native_int(m, 0x61);
    CHECK(isr_calls == 1 && !isr_saw_if, "native_int runs the lifted handler with IF clear");
    CHECK((m->cpu.flags & (F_IF | F_CF)) == (F_IF | F_CF) && m->cpu.instructions == before,
          "its IRET restores the flags, no instruction interpreted");
    check_caller_state(m, "native_int");
    machine_destroy(m);
}

static int user_ticks;
static CallbackResult count_user_tick(Machine *m)
{
    user_ticks++;
    return CB_DONE;
}

static bool irq5_ran;
static CallbackResult irq5_callback(Machine *m)
{
    irq5_ran = true;
    cb_set_carry(m, true); /* a callback changes the flags its IRET restores */
    return CB_DONE;
}

static uint32_t bios_ticks(Machine *m)
{
    return mem_rd16(m, BDA_BASE + BDA_TICKS) | (uint32_t)mem_rd16(m, BDA_BASE + BDA_TICKS + 2) << 16;
}

static void test_rom_lifts(void)
{
    Machine *m = make_machine(true);
    m->cpu.flags = (m->cpu.flags | F_IF) & ~(uint32_t)F_CF;
    bios_irq_handler(m, 0x0D, "test irq5", irq5_callback, false);
    native_irq_raise(m, 5);
    native_safepoint(m);
    CHECK(irq5_ran, "the ROM IRQ routine's callback runs natively");
    CHECK(native_state(m)->pic[0].isr == 0, "its EOI is sent (ISR %02X)", native_state(m)->pic[0].isr);
    CHECK((m->cpu.flags & (F_IF | F_CF)) == (F_IF | F_CF), "its IRET restores the frame's flags");

    callback_register(m, "test int1c", count_user_tick, 0x1C);
    uint32_t ticks = bios_ticks(m);
    native_irq_raise(m, 0);
    native_safepoint(m);
    CHECK(bios_ticks(m) == ticks + 1 && user_ticks == 1 && native_state(m)->pic[0].isr == 0,
          "the timer routine counts the tick, calls INT 1Ch and sends its EOI");

    native_irq_raise(m, 3);
    native_irq_raise(m, 11);
    native_safepoint(m);
    native_safepoint(m);
    CHECK(native_state(m)->pic[0].isr == 0 && native_state(m)->pic[1].isr == 0 && native_state(m)->pic[0].irr == 0 &&
              native_state(m)->pic[1].irr == 0,
          "default IRQ vectors, master and slave, run natively and send EOIs");
    native_int(m, 0x1F + 0x40); /* an unused vector: the ROM's dummy IRET */
    check_caller_state(m, "rom lifts");
    machine_destroy(m);
}

static int retry_calls;
static CallbackResult blocking_service(Machine *m)
{
    return retry_calls++ == 0 ? bios_block(m) : CB_DONE;
}

static void test_waits(void)
{
    Machine *m = make_machine(false);
    install_recorders(m);
    eoi_in_handler = true;
    host_timer_set_divisor(m, DIVISOR_100HZ);
    uint64_t t0 = m->virtual_us;
    lift_idle(m);
    CHECK(irq_count == 1 && irq_order[0] == 0x08, "lift_idle waits for one interrupt");
    CHECK(m->virtual_us >= t0 + 10001 && m->virtual_us <= t0 + 10002, "the clock moved to the tick (%llu us)",
          (unsigned long long)(m->virtual_us - t0));
    m->cpu.flags &= ~(uint32_t)F_IF;
    lift_idle(m);
    CHECK(irq_count == 2 && !(m->cpu.flags & F_IF), "idle enables interrupts only while it waits");

    callback_register(m, "blocking", blocking_service, 0x62);
    m->cpu.flags |= F_IF;
    t0 = m->virtual_us;
    guest_int(m, 0x62);
    CHECK(retry_calls == 2 && irq_count == 3 && m->virtual_us > t0 + 5000,
          "CB_RETRY waits for an interrupt and calls the service again (calls %d, irqs %d)", retry_calls, irq_count);
    check_caller_state(m, "waits");
    machine_destroy(m);
}

#ifndef __EMSCRIPTEN__
/* Runs fn in a child; returns its exit status and leaves its stderr in out. */
static int run_child(void (*fn)(void), char *out, size_t cap)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 2);
        close(fds[0]);
        fn();
        _exit(0);
    }
    close(fds[1]);
    size_t len = 0;
    ssize_t got;
    while (len + 1 < cap && (got = read(fds[0], out + len, cap - 1 - len)) > 0)
        len += (size_t)got;
    out[len] = 0;
    close(fds[0]);
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void call_unlifted(void)
{
    Machine *m = make_machine(false);
    guest_call_far(m, CODE_SEG, PLAIN_OFF);
}

static void port_access(void)
{
    Machine *m = make_machine(false);
    port_out(m, 0x43, 0x36, 1);
}

static void call_unlifted_lenient(void)
{
    setenv("INN_NATIVE_STRICT", "0", 1);
    Machine *m = machine_create();
    native_init(m, NULL);
    memcpy(m->mem + real_addr(CODE_SEG, 0), PROGRAM, sizeof PROGRAM);
    lift_module_loaded(m, "TEST.EXE", CODE_SEG, sizeof PROGRAM);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    SP = STACK_TOP;
    native_set_active(m, true);
    guest_call_far(m, CODE_SEG, PLAIN_OFF);
    fprintf(stderr, "AX=%04X\n", AX);
    machine_destroy(m);
}

static void test_strict(void)
{
    char out[1024];
    int status = run_child(call_unlifted, out, sizeof out);
    CHECK(status == 1 && strstr(out, "fatal: native: guest code at 1000:0030 (TEST.EXE +0000:0030) would be interpreted"),
          "strict mode: an unresolved target is fatal with its module (status %d, %s)", status, out);
    status = run_child(port_access, out, sizeof out);
    CHECK(status == 1 && strstr(out, "fatal: native: port out 0043 at"), "port I/O is fatal (status %d, %s)", status,
          out);
    status = run_child(call_unlifted_lenient, out, sizeof out);
    CHECK(status == 0 && strstr(out, "AX=3333") && strstr(out, "instructions interpreted while native") &&
              strstr(out, "TEST.EXE 2"),
          "INN_NATIVE_STRICT=0 interprets it and reports the count per module (status %d, %s)", status, out);
}
#endif

/* ---- JSPI proof (Emscripten build) ---- */

static jmp_buf sleepy_exit;
static int sleeps;

#ifdef __EMSCRIPTEN__
/* Suspends the whole C stack (dispatcher frames, an indirect call through the lifted table) and then
 * unwinds it with longjmp, as a process exit does. */
static void lifted_sleepy(Machine *m)
{
    for (int i = 0; i < 100; i++) {
        emscripten_sleep(1);
        sleeps++;
    }
    longjmp(sleepy_exit, 1);
}

static uint64_t browser_now_us(void *ctx)
{
    return (uint64_t)(emscripten_get_now() * 1000.0);
}

static void browser_sleep_us(void *ctx, uint64_t us)
{
    emscripten_sleep((unsigned)(us / 1000));
}

static void test_jspi(void)
{
    Machine *m = make_machine(false);
    sleeps = 0;
    if (!setjmp(sleepy_exit))
        guest_call_far(m, CODE_SEG, SLEEPY_OFF);
    CHECK(sleeps == 100, "100 suspensions inside the setjmp scope, then longjmp out (%d)", sleeps);

    /* A real-time wait through the platform suspends too, and the machine still runs. */
    static const HostPlatform platform = { .now_us = browser_now_us, .sleep_us = browser_sleep_us };
    NativeConfig cfg = { .strict = true, .call_cost_ns = 1000, .yield_every_us = 4000 };
    native_init(m, &cfg);
    native_set_platform(m, &platform);
    install_recorders(m);
    eoi_in_handler = true;
    host_timer_set_divisor(m, DIVISOR_100HZ);
    double start = emscripten_get_now();
    uint64_t t0 = m->virtual_us;
    native_wait_until(m, t0 + 50000);
    double wall_ms = emscripten_get_now() - start;
    CHECK(irq_count == 4 && wall_ms >= 45, "a 50 ms wait (4 ticks of 10.00002 ms) paced by the wall clock (%d ticks, %.1f ms)", irq_count, wall_ms);
    native_set_platform(m, NULL);
    guest_call_far(m, CODE_SEG, BODY_OFF);
    CHECK(AX == LIFTED_AX, "dispatch works after the longjmp");
    machine_destroy(m);
}
#else
static void lifted_sleepy(Machine *m)
{
    sleeps++;
    longjmp(sleepy_exit, 1);
}

static void test_longjmp(void)
{
    Machine *m = make_machine(false);
    sleeps = 0;
    if (!setjmp(sleepy_exit))
        guest_call_far(m, CODE_SEG, SLEEPY_OFF);
    CHECK(sleeps == 1, "longjmp out of a dispatched lifted call");
    guest_call_far(m, CODE_SEG, BODY_OFF);
    CHECK(AX == LIFTED_AX, "dispatch works after the longjmp");
    machine_destroy(m);
}
#endif

int main(void)
{
    unsetenv("INN_NATIVE_STRICT");
    test_clock();
    test_priority();
    test_if();
    test_timer();
    test_native_int();
    test_rom_lifts();
    test_waits();
#ifdef __EMSCRIPTEN__
    test_jspi();
#else
    test_strict();
    test_longjmp();
#endif
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        printf("test_native: all checks passed\n");
    return failures != 0;
}
