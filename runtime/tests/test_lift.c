/* Lifted-function hooks: dispatch at the entry, nested near/far guest calls, signature guard. */
#include <stdio.h>
#include <stdlib.h>
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

static Run run_program_ex(LiftedFn fn, uint16_t off, bool near, bool good_signature, bool verify, bool no_shadow)
{
    Machine *m = machine_create();
    memcpy(m->mem + real_addr(CODE_SEG, 0), PROGRAM, sizeof PROGRAM);
    entries[0] = (LiftedEntry){ .module = "TEST.EXE", .off = off, .fn = fn, .name = "fn",
                                .near_return = near, .scratch_regs = LIFT_REG_FLAGS,
                                .no_shadow = no_shadow };
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

static Run run_program(LiftedFn fn, uint16_t off, bool near, bool good_signature, bool verify)
{
    return run_program_ex(fn, off, near, good_signature, verify, false);
}


/* ---- overlays: code that an overlay manager reads into a reused buffer ---- */

enum { OV_A = 0x3000, OV_B = 0x4000, OV_PROGRAM_SEG = 0x1200, OV_LIFTED = 0x9999 };

/* mov ax, 0034h + program segment (a relocated word, as in the file) / retf */
static const uint8_t OV_FILE_CODE[] = { 0xB8, 0x34, 0x00, 0xCB };

static void lifted_overlay(Machine *m)
{
    AX = OV_LIFTED;
    lift_ret_far(m, 0);
}

static void place_overlay(Machine *m, uint16_t seg, uint16_t program_seg)
{
    memcpy(m->mem + real_addr(seg, 0), OV_FILE_CODE, sizeof OV_FILE_CODE);
    mem_wr16(m, real_addr(seg, 1), (uint16_t)(0x34 + program_seg)); /* the manager applies the fixup */
}

/* Far-calls seg:0 from a stub and returns AX. */
static uint16_t call_overlay(Machine *m, uint16_t seg)
{
    const uint8_t stub[] = { 0x9A, 0x00, 0x00, (uint8_t)seg, (uint8_t)(seg >> 8), 0xF4 };
    memcpy(m->mem + real_addr(CODE_SEG, 0), stub, sizeof stub);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    SP = 0x100;
    m->cpu.eip = 0;
    m->cpu.halted = false;
    AX = 0;
    for (int i = 0; i < 100 && !m->cpu.halted; i++)
        machine_step(m);
    CHECK(m->cpu.halted && SP == 0x100, "overlay call returned");
    return AX;
}

static void test_overlays(void)
{
    static LiftedEntry ov[1];
    ov[0] = (LiftedEntry){ .module = "PROG.EXE#0010", .fn = lifted_overlay, .name = "ov", .scratch_regs = LIFT_REG_FLAGS,
                           .reloc_mask = 1 << 1, .unknown_mask = 0xFFF & ~0xFu };
    memcpy(ov[0].signature, OV_FILE_CODE, sizeof OV_FILE_CODE);
    Machine *m = machine_create();
    m->cpu.flags &= ~F_IF;
    lift_add_table(m, ov, 1);
    uint16_t original = 0x34 + OV_PROGRAM_SEG;

    place_overlay(m, OV_A, OV_PROGRAM_SEG);
    lift_overlay_loaded(m, "PROG.EXE#0010", OV_A, sizeof OV_FILE_CODE, OV_PROGRAM_SEG);
    CHECK(call_overlay(m, OV_A) == OV_LIFTED, "overlay entry hooked where it was read");

    place_overlay(m, OV_B, OV_PROGRAM_SEG);
    lift_overlay_loaded(m, "PROG.EXE#0010", OV_B, sizeof OV_FILE_CODE, OV_PROGRAM_SEG);
    CHECK(call_overlay(m, OV_A) == original, "reloading elsewhere disarms the old copy");
    CHECK(call_overlay(m, OV_B) == OV_LIFTED, "and arms the new one");

    mem_wr16(m, real_addr(OV_B, 1), 0x0055);
    CHECK(call_overlay(m, OV_B) == 0x0055, "other bytes at the entry run the original");
    place_overlay(m, OV_B, OV_PROGRAM_SEG);
    CHECK(call_overlay(m, OV_B) == OV_LIFTED, "entry bytes are checked on every call");

    lift_overlay_loaded(m, "PROG.EXE#0020", OV_B, 0x100, OV_PROGRAM_SEG);
    CHECK(call_overlay(m, OV_B) == original, "another overlay read over it evicts it");

    lift_overlay_loaded(m, "PROG.EXE#0010", OV_B, sizeof OV_FILE_CODE, OV_PROGRAM_SEG + 1);
    CHECK(call_overlay(m, OV_B) == original, "fixups are checked against the program segment");
    machine_destroy(m);
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
    r = run_program_ex(lifted_f, F_OFF, false, true, true, true);
    CHECK(r.ax == LIFTED_F_RESULT && r.mismatches == 0, "no-shadow F runs lifted, unverified (AX=%u, %d mismatches)",
          r.ax, r.mismatches);
    setenv("INN_LIFT_ONLY", "other,fnx", 1);
    r = run_program(lifted_f, F_OFF, false, true, false);
    CHECK(r.ax == ORIGINAL_RESULT, "INN_LIFT_ONLY without the name leaves it unhooked (AX=%u)", r.ax);
    setenv("INN_LIFT_ONLY", "other,fn", 1);
    r = run_program(lifted_f, F_OFF, false, true, false);
    CHECK(r.ax == LIFTED_F_RESULT, "INN_LIFT_ONLY with the name hooks it (AX=%u)", r.ax);
    unsetenv("INN_LIFT_ONLY");
    test_overlays();
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    return failures != 0;
}
