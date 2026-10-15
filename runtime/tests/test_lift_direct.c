/* Direct calls (docs/lifting.md, "Direct calls"): guest_call_far, guest_call_near and guest_int run
 * lifted entries, jump thunks, bare returns and service stubs in C when no interrupt is due. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "lift.h"

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
    CODE_SEG = 0x1000, STACK_SEG = 0x2000, STACK_TOP = 0x100,
    BODY_OFF = 0x10,     /* original: mov ax,1111h / retf */
    NEAR_OFF = 0x20,     /* original: mov ax,2222h / ret */
    JMP_OFF = 0x30,      /* jmp near BODY / jmp short JMP2 (below) / jmp far BODY */
    SHORT_OFF = 0x40, FAR_OFF = 0x50,
    RETF4_OFF = 0x60, RET_OFF = 0x70, IRET_OFF = 0x78,
    ISR_OFF = 0x80,      /* inc word [COUNTER] / iret */
    PLAIN_OFF = 0xA0,    /* mov ax,3333h / retf: not lifted */
    THUNK_TO_PLAIN_OFF = 0xB0,
    COUNTER = 0x500,
    LIFTED_AX = 0xAAAA, STUB_AX = 0x5757, ORIGINAL_AX = 0x1111, NEAR_AX = 0x2222, PLAIN_AX = 0x3333
};

/* Little-endian displacement of a near JMP at from to dest. */
#define REL16(dest, from) (uint8_t)((dest) - ((from) + 3)), (uint8_t)(((dest) - ((from) + 3)) >> 8)

static const uint8_t PROGRAM[] = {
    [BODY_OFF] = 0xB8, 0x11, 0x11, 0xCB,
    [NEAR_OFF] = 0xB8, 0x22, 0x22, 0xC3,
    [JMP_OFF] = 0xE9, REL16(BODY_OFF, JMP_OFF),
    [SHORT_OFF] = 0xEB, (uint8_t)(JMP_OFF - (SHORT_OFF + 2)),
    [FAR_OFF] = 0xEA, SHORT_OFF, 0x00, CODE_SEG & 0xFF, CODE_SEG >> 8,
    [RETF4_OFF] = 0xCA, 0x04, 0x00,
    [RET_OFF] = 0xCB,
    [IRET_OFF] = 0xCF,
    [ISR_OFF] = 0xFF, 0x06, COUNTER & 0xFF, COUNTER >> 8, 0xCF,
    [PLAIN_OFF] = 0xB8, 0x33, 0x33, 0xCB,
    [THUNK_TO_PLAIN_OFF] = 0xE9, REL16(PLAIN_OFF, THUNK_TO_PLAIN_OFF),
};

static void lifted_body(Machine *m)
{
    AX = LIFTED_AX;
    lift_ret_far(m, 0);
}

static void lifted_near(Machine *m)
{
    AX = LIFTED_AX;
    lift_ret_near(m, 0);
}

static LiftedEntry entries[2];

static Machine *make_machine(void)
{
    Machine *m = machine_create();
    memcpy(m->mem + real_addr(CODE_SEG, 0), PROGRAM, sizeof PROGRAM);
    entries[0] = (LiftedEntry){ .module = "TEST.EXE", .off = BODY_OFF, .fn = lifted_body, .name = "body",
                                .scratch_regs = LIFT_REG_FLAGS, .no_shadow = true };
    memcpy(entries[0].signature, PROGRAM + BODY_OFF, LIFT_SIGNATURE_LEN);
    entries[1] = (LiftedEntry){ .module = "TEST.EXE", .off = NEAR_OFF, .fn = lifted_near, .name = "near",
                                .near_return = true, .scratch_regs = LIFT_REG_FLAGS, .no_shadow = true };
    memcpy(entries[1].signature, PROGRAM + NEAR_OFF, LIFT_SIGNATURE_LEN);
    lift_add_table(m, entries, 2);
    lift_module_loaded(m, "TEST.EXE", CODE_SEG, sizeof PROGRAM);
    cpu_set_seg(&m->cpu, S_CS, 0x3000);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    SP = STACK_TOP;
    m->cpu.eip = 0x42;
    m->cpu.flags &= ~F_IF;
    return m;
}

static uint64_t far_call(Machine *m, uint16_t off)
{
    m->cpu.stall = 0; /* direct calls leave their charge here, and the CPU counts it as instructions */
    uint64_t before = m->cpu.instructions;
    AX = 0;
    guest_call_far(m, CODE_SEG, off);
    return m->cpu.instructions - before;
}

static void check_restored(Machine *m, const char *what)
{
    CHECK(SP == STACK_TOP && m->cpu.seg[S_CS] == 0x3000 && (m->cpu.eip & 0xFFFF) == 0x42,
          "%s: caller state restored (SP=%04X CS:IP=%04X:%04X)", what, SP, m->cpu.seg[S_CS],
          (unsigned)(m->cpu.eip & 0xFFFF));
}

static void test_lifted_and_thunks(void)
{
    Machine *m = make_machine();
    uint64_t ran = far_call(m, BODY_OFF);
    CHECK(AX == LIFTED_AX && ran == 0, "lifted entry runs without the CPU (AX=%04X, %llu instructions)", AX,
          (unsigned long long)ran);
    CHECK(m->cpu.stall > 0, "a direct call is charged bus time");
    check_restored(m, "lifted");

    static const struct { uint16_t off; const char *name; } thunks[] = {
        { JMP_OFF, "near jmp" }, { SHORT_OFF, "short jmp then near jmp" }, { FAR_OFF, "far jmp chain" },
    };
    for (size_t i = 0; i < sizeof thunks / sizeof thunks[0]; i++) {
        ran = far_call(m, thunks[i].off);
        CHECK(AX == LIFTED_AX && ran == 0, "%s is followed to the lifted entry (AX=%04X, %llu instructions)",
              thunks[i].name, AX, (unsigned long long)ran);
    }
    check_restored(m, "thunks");

    ran = far_call(m, THUNK_TO_PLAIN_OFF);
    CHECK(AX == PLAIN_AX && ran > 0, "a jump to unlifted code is not followed, the CPU runs the thunk (AX=%04X)", AX);
    ran = far_call(m, PLAIN_OFF);
    CHECK(AX == PLAIN_AX && ran > 0, "unlifted code runs on the CPU (AX=%04X)", AX);
    check_restored(m, "unlifted");

    machine_destroy(m);
}

static void test_returns(void)
{
    Machine *m = make_machine();
    uint64_t ran = far_call(m, RET_OFF);
    CHECK(ran == 0 && SP == STACK_TOP, "a bare RETF returns directly (%llu instructions, SP=%04X)",
          (unsigned long long)ran, SP);
    mem_wr16(m, real_addr(STACK_SEG, STACK_TOP - 2), 0xBEEF);
    mem_wr16(m, real_addr(STACK_SEG, STACK_TOP - 4), 0xCAFE);
    SP = STACK_TOP - 4; /* two argument words, popped by the callee */
    ran = far_call(m, RETF4_OFF);
    CHECK(ran == 0 && SP == STACK_TOP, "RETF 4 pops its arguments (%llu instructions, SP=%04X)",
          (unsigned long long)ran, SP);
    machine_destroy(m);
}

static int stub_calls;
static CallbackResult stub_done(Machine *m)
{
    stub_calls++;
    AX = STUB_AX;
    m->cpu.flags &= ~(uint32_t)F_CF; /* the IRET restores the caller's flags, as for any ISR */
    return CB_DONE;
}

static CallbackResult stub_retry(Machine *m)
{
    AX = STUB_AX;
    return stub_calls++ == 0 ? CB_RETRY : CB_DONE;
}

static void test_stubs(void)
{
    enum { VECTOR = 0x60 };
    Machine *m = make_machine();
    callback_register(m, "stub_done", stub_done, VECTOR);
    stub_calls = 0;
    m->cpu.flags |= F_CF;
    uint64_t before = m->cpu.instructions;
    AX = 0;
    guest_int(m, VECTOR);
    CHECK(stub_calls == 1 && AX == STUB_AX && m->cpu.instructions == before,
          "service stub runs its callback without the CPU (calls=%d, %llu instructions)", stub_calls,
          (unsigned long long)(m->cpu.instructions - before));
    CHECK((m->cpu.flags & F_CF) && SP == STACK_TOP, "stub IRET restores the pushed flags (flags=%04X, SP=%04X)",
          (unsigned)(m->cpu.flags & 0xFFFF), SP);
    machine_destroy(m);

    m = make_machine();
    callback_register(m, "stub_retry", stub_retry, VECTOR);
    stub_calls = 0;
    before = m->cpu.instructions;
    AX = 0;
    guest_int(m, VECTOR);
    CHECK(stub_calls == 2 && AX == STUB_AX && m->cpu.instructions > before,
          "CB_RETRY falls back to the CPU, which retries (calls=%d)", stub_calls);
    check_restored(m, "retry");
    machine_destroy(m);
}

static void test_near(void)
{
    Machine *m = make_machine();
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    uint64_t before = m->cpu.instructions;
    AX = 0;
    guest_call_near(m, NEAR_OFF);
    CHECK(AX == LIFTED_AX && m->cpu.instructions == before && SP == STACK_TOP,
          "near call to a lifted entry is direct (AX=%04X)", AX);
    machine_destroy(m);
}

static void test_interrupt_due(void)
{
    enum { TIMER_VECTOR = 0x08 };
    Machine *m = make_machine();
    pic_init(m);
    mem_wr16(m, TIMER_VECTOR * 4u, ISR_OFF);
    mem_wr16(m, TIMER_VECTOR * 4u + 2, CODE_SEG);
    mem_wr16(m, COUNTER, 0);
    m->cpu.flags |= F_IF;
    pic_raise_irq(m, 0);
    uint64_t ran = far_call(m, BODY_OFF);
    CHECK(mem_rd16(m, COUNTER) == 1 && AX == LIFTED_AX && ran > 0,
          "a due interrupt runs on the CPU before the lifted entry (ISR ran %u times, AX=%04X)",
          mem_rd16(m, COUNTER), AX);
    ran = far_call(m, BODY_OFF);
    CHECK(mem_rd16(m, COUNTER) == 1 && AX == LIFTED_AX && ran == 0, "the next call is direct again");
    machine_destroy(m);
}

static void test_disabled(void)
{
    setenv("INN_DIRECT_CALLS", "0", 1);
    Machine *m = make_machine();
    uint64_t ran = far_call(m, BODY_OFF);
    CHECK(AX == LIFTED_AX && ran > 0, "INN_DIRECT_CALLS=0 goes through the CPU path (%llu instructions)",
          (unsigned long long)ran);
    ran = far_call(m, RET_OFF);
    CHECK(ran > 0 && SP == STACK_TOP, "and runs a bare return on the CPU");
    machine_destroy(m);

    setenv("INN_DIRECT_CALLS", "3", 1);
    m = make_machine();
    ran = far_call(m, BODY_OFF);
    CHECK(AX == LIFTED_AX && ran > 0, "INN_DIRECT_CALLS=3 leaves lifted entries to the CPU");
    machine_destroy(m);

    setenv("INN_DIRECT_CALLS", "2", 1);
    m = make_machine();
    ran = far_call(m, BODY_OFF);
    CHECK(AX == LIFTED_AX && ran == 0, "INN_DIRECT_CALLS=2 calls lifted entries directly");
    machine_destroy(m);
    unsetenv("INN_DIRECT_CALLS");
}

static bool unresolved_seen;
static bool unresolved_hook(Machine *m, uint32_t linear)
{
    unresolved_seen = linear == real_addr(CODE_SEG, PLAIN_OFF);
    AX = 0x7777;
    lift_ret_far(m, 0);
    return true;
}

static void test_unresolved(void)
{
    Machine *m = make_machine();
    lift_set_unresolved(m, unresolved_hook);
    uint64_t ran = far_call(m, PLAIN_OFF);
    CHECK(unresolved_seen && AX == 0x7777 && ran == 0, "an unresolved target is offered to the hook");
    ran = far_call(m, BODY_OFF);
    CHECK(AX == LIFTED_AX, "a lifted target is not");
    machine_destroy(m);
}

int main(void)
{
    unsetenv("INN_DIRECT_CALLS");
    test_lifted_and_thunks();
    test_returns();
    test_stubs();
    test_near();
    test_interrupt_due();
    test_disabled();
    test_unresolved();
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    return failures != 0;
}
