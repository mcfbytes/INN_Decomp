/* Lifted-function hooks: dispatch at the entry, nested near/far guest calls, signature guard. */
#include <stdio.h>
#include <string.h>

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

enum { CODE_SEG = 0x1000, STACK_SEG = 0x2000, F_OFF = 0x10, G_OFF = 0x20, H_OFF = 0x30 };

/* main: mov ax,5 / call far F / hlt.  F: add ax,ax / call G / retf.  G: inc ax / ret.
 * H: add ax,100 / retf (only reached from the lifted F). */
static const uint8_t PROGRAM[] = {
    [0x00] = 0xB8, 0x05, 0x00, 0x9A, F_OFF, 0x00, CODE_SEG & 0xFF, CODE_SEG >> 8, 0xF4,
    [F_OFF] = 0x01, 0xC0, 0xE8, G_OFF - (F_OFF + 5), 0x00, 0xCB, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    [G_OFF] = 0x40, 0xC3,
    [H_OFF] = 0x05, 100, 0x00, 0xCB,
};

/* Same contract as F, different arithmetic so the test can tell which one ran. */
static void lifted_f(Machine *m)
{
    AX = (uint16_t)(AX * 3);
    guest_call_near(m, G_OFF);
    guest_call_far(m, CODE_SEG, H_OFF);
    lift_ret_far(m, 0);
}

/* A faithful lift of G. */
static void lifted_g(Machine *m)
{
    AX = (uint16_t)(AX + 1);
    m->cpu.flags &= ~F_ARITH;
    if (AX == 0)
        m->cpu.flags |= F_ZF;
    lift_ret_near(m, 0);
}

typedef struct Run { uint16_t ax; int mismatches; } Run;

static LiftedEntry entries[1];

static Run run_program(LiftedFn fn, uint16_t off, bool near, bool good_signature, bool verify)
{
    Machine *m = machine_create();
    memcpy(m->mem + real_addr(CODE_SEG, 0), PROGRAM, sizeof PROGRAM);
    entries[0] = (LiftedEntry){ .module = "TEST.EXE", .off = off, .fn = fn, .name = "fn",
                                .near_return = near, .scratch_regs = LIFT_REG_FLAGS };
    memcpy(entries[0].signature, PROGRAM + off, LIFT_SIGNATURE_LEN);
    if (!good_signature)
        entries[0].signature[0] ^= 0xFF;
    lift_add_table(m, entries, 1);
    if (verify)
        lift_verify_enable(m, 10);
    lift_module_loaded(m, "TEST.EXE", CODE_SEG, sizeof PROGRAM);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    SP = 0x100;
    m->cpu.eip = 0;
    m->cpu.flags &= ~F_IF;
    for (int i = 0; i < 100 && !m->cpu.halted; i++)
        machine_step(m);
    Run r = { AX, lift_verify_mismatches(m) };
    CHECK(m->cpu.halted, "program reached hlt");
    CHECK(SP == 0x100, "stack balanced (SP=%04X)", SP);
    machine_destroy(m);
    return r;
}

int main(void)
{
    enum { ORIGINAL_RESULT = 5 * 2 + 1, LIFTED_F_RESULT = 5 * 3 + 1 + 100 };
    Run r = run_program(lifted_f, F_OFF, false, true, false);
    CHECK(r.ax == LIFTED_F_RESULT, "lifted F ran with nested calls (AX=%u)", r.ax);
    r = run_program(lifted_f, F_OFF, false, false, false);
    CHECK(r.ax == ORIGINAL_RESULT, "signature mismatch runs the original (AX=%u)", r.ax);
    r = run_program(lifted_g, G_OFF, true, true, true);
    CHECK(r.ax == ORIGINAL_RESULT && r.mismatches == 0, "faithful G verifies (AX=%u, %d mismatches)",
          r.ax, r.mismatches);
    r = run_program(lifted_f, F_OFF, false, true, true);
    CHECK(r.mismatches == 1, "unfaithful F is caught (%d mismatches)", r.mismatches);
    CHECK(r.ax == ORIGINAL_RESULT, "verification continues from the original (AX=%u)", r.ax);
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    return failures != 0;
}
