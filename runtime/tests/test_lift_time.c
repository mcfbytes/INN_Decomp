/* Exact time for lifted C (lift_time.h): a lifted busy loop that charges the original's instructions
 * ends where the original does, to the nanosecond, with the same timer interrupts taken on the way. */
#include <stdio.h>
#include <string.h>

#include "devices.h"
#include "lift.h"
#include "lift_time.h"

static int failures;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            failures++;                                          \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fputc('\n', stderr);                                 \
        }                                                        \
    } while (0)

enum {
    CODE_SEG = 0x0700, STACK_SEG = 0x0600, STACK_TOP = 0x0FFE, TICKS = 0x500,
    MAIN_IF = 0x0E, LOOP_COUNT = 0x47, F_OFF = 0x40, ISR_OFF = 0x80, IDLE_PORT = 0x2F0,
};

/* main: cli; PIT channel 0 at about 1 kHz; sti (or nop); cx = 3; call far F; loop; cli; hlt.
 * F: push ax/dx/di; dx = 2F0h; di = n; in al,dx / dec di / cmp di,0 / jne; pop di/dx/ax; retf.
 * ISR (vector 8): inc word [500h]; push ax; EOI; pop ax; iret. */
static const uint8_t PROGRAM[] = {
    [0x00] = 0xFA, 0xB0, 0x36, 0xE6, 0x43, 0xB8, 0xA9, 0x04, 0xE6, 0x40, 0x88, 0xE0, 0xE6, 0x40,
    [MAIN_IF] = 0xFB, 0xB9, 0x03, 0x00,
    0x9A, F_OFF, 0x00, CODE_SEG & 0xFF, CODE_SEG >> 8, /* 0x12 */
    0xE2, 0xF9,                                          /* 0x17: loop 0x12 */
    0xFA, 0xF4,
    [F_OFF] = 0x50, 0x52, 0x57, 0xBA, IDLE_PORT & 0xFF, IDLE_PORT >> 8, 0xBF, 0x88, 0x13,
    0xEC, 0x4F, 0x83, 0xFF, 0x00, 0x75, 0xF9, 0x5F, 0x5A, 0x58, 0xCB,
    [ISR_OFF] = 0xFF, 0x06, TICKS & 0xFF, TICKS >> 8, 0x50, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF,
};

static int exact_calls;

static void lifted_f(Machine *m)
{
    LiftTime t;
    uint16_t ax = AX, dx = DX, di = DI;
    if (lift_time_begin(m, &t)) /* push ax */
        exact_calls++;
    lift_time_insns(m, &t, 4, true); /* push dx, push di, mov dx, mov di */
    uint16_t n = DI = mem_rd16(m, real_addr(CODE_SEG, LOOP_COUNT));
    for (; n; n--) {
        AL = (uint8_t)port_in(m, IDLE_PORT, 1);
        lift_time_insns(m, &t, 4, true); /* in, dec, cmp, jne */
    }
    lift_time_insns(m, &t, 3, true);  /* pop di, pop dx, pop ax */
    AX = ax, DX = dx, DI = di;
    lift_time_insns(m, &t, 1, false); /* retf: an interrupt due after it is the caller's */
    lift_time_end(m, &t);
    m->cpu.flags = (m->cpu.flags & ~(uint32_t)F_ARITH) | F_ZF | F_PF;
    lift_ret_far(m, 0);
}

typedef struct Result { uint64_t now_ns, virtual_us; uint16_t ticks, sp, ax; } Result;

static Result run(bool lifted, uint16_t loops, bool irqs_on, uint32_t ips)
{
    static LiftedEntry entries[1];
    Machine *m = machine_create();
    bios_init(m);
    m->idle.off = true;
    if (ips)
        m->ips = ips;
    memcpy(m->mem + real_addr(CODE_SEG, 0), PROGRAM, sizeof PROGRAM);
    mem_wr16(m, real_addr(CODE_SEG, LOOP_COUNT), loops);
    if (!irqs_on)
        m->mem[real_addr(CODE_SEG, MAIN_IF)] = 0x90;
    mem_wr16(m, 8 * 4, ISR_OFF);
    mem_wr16(m, 8 * 4 + 2, CODE_SEG);
    if (lifted) {
        entries[0] = (LiftedEntry){ .module = "TIME.EXE", .off = F_OFF, .fn = lifted_f, .name = "f",
                                    .scratch_regs = LIFT_REG_FLAGS };
        memcpy(entries[0].signature, m->mem + real_addr(CODE_SEG, F_OFF), LIFT_SIGNATURE_LEN);
        lift_add_table(m, entries, 1);
        lift_module_loaded(m, "TIME.EXE", CODE_SEG, sizeof PROGRAM);
    }
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    cpu_set_seg(&m->cpu, S_DS, 0);
    m->cpu.eip = 0;
    SP = STACK_TOP;
    m->cpu.flags = 0x0202;
    AX = 0x1234;
    for (int i = 0; i < 1000000 && !m->cpu.halted; i++)
        machine_step(m);
    CHECK(m->cpu.halted, "the program ran to its hlt");
    Result r = { machine_now_ns(m), m->virtual_us, mem_rd16(m, TICKS), SP, AX };
    bios_shutdown(m);
    machine_destroy(m);
    return r;
}

static void compare(uint16_t loops, bool irqs_on, uint32_t ips)
{
    exact_calls = 0;
    Result a = run(false, loops, irqs_on, ips), b = run(true, loops, irqs_on, ips);
    CHECK(exact_calls == 3, "%u loops: %d of 3 lifted calls in exact time", loops, exact_calls);
    CHECK(a.now_ns == b.now_ns, "%u loops, IF %d, %u ips: original ends at %llu ns, lifted at %llu", loops,
          irqs_on, ips, (unsigned long long)a.now_ns, (unsigned long long)b.now_ns);
    CHECK(a.ticks == b.ticks, "%u loops, IF %d: %u timer interrupts against %u", loops, irqs_on, a.ticks, b.ticks);
    CHECK(b.sp == STACK_TOP && b.sp == a.sp && b.ax == a.ax, "%u loops: stack and AX as the original's", loops);
    if (irqs_on && loops >= 1234)
        CHECK(a.ticks > 3, "%u loops: the timer interrupted the loop (%u ticks)", loops, a.ticks);
    else if (!irqs_on)
        CHECK(a.ticks == 0, "%u loops: no interrupt with IF clear (%u)", loops, a.ticks);
}

int main(void)
{
    static const uint16_t LOOPS[] = { 5000, 4999, 1234, 777, 1 };
    for (size_t i = 0; i < sizeof LOOPS / sizeof LOOPS[0]; i++) {
        compare(LOOPS[i], true, 0);
        compare(LOOPS[i], false, 0);
    }
    compare(3001, true, 33000000); /* slots that are not a whole number of ns */
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        printf("lift_time: all tests passed\n");
    return failures ? 1 : 0;
}
