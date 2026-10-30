/* Lifted-function hooks: dispatch at the entry, nested near/far guest calls, signature guard. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "dos.h"
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

typedef struct Run { uint16_t ax; int mismatches; uint64_t insns, us; } Run;

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
    Run r = { AX, lift_verify_mismatches(m), m->cpu.instructions, m->virtual_us };
    CHECK(m->cpu.halted, "program reached hlt");
    CHECK(SP == 0x100, "stack balanced (SP=%04X)", SP);
    machine_destroy(m);
    return r;
}

static Run run_program(LiftedFn fn, uint16_t off, bool near, bool good_signature, bool verify)
{
    return run_program_ex(fn, off, near, good_signature, verify, false);
}


/* ---- INN_VERIFY_KEEP_TIME over many time slices ---- */

/* mov cx,3000 / L: call G / loop L / hlt, G as above: 9000 instructions, 90 slices of 2000. */
static Run run_loop(bool hooked)
{
    static const uint8_t loop[] = { 0xB9, 0xB8, 0x0B, 0xE8, G_OFF - 6, 0x00, 0xE2, 0xFB, 0xF4 };
    Machine *m = machine_create();
    memcpy(m->mem + real_addr(CODE_SEG, 0), PROGRAM, sizeof PROGRAM);
    memcpy(m->mem + real_addr(CODE_SEG, 0), loop, sizeof loop);
    entries[0] = (LiftedEntry){ .module = "TEST.EXE", .off = G_OFF, .fn = lifted_g, .name = "fn",
                                .near_return = true, .scratch_regs = LIFT_REG_FLAGS };
    memcpy(entries[0].signature, PROGRAM + G_OFF, LIFT_SIGNATURE_LEN);
    if (!hooked)
        entries[0].signature[0] ^= 0xFF;
    lift_add_table(m, entries, 1);
    lift_verify_enable(m, 100000);
    lift_module_loaded(m, "TEST.EXE", CODE_SEG, sizeof PROGRAM);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    SP = 0x100;
    m->cpu.eip = 0;
    m->cpu.flags &= ~F_IF;
    for (int i = 0; i < 100000 && !m->cpu.halted; i++)
        machine_step(m);
    Run r = { AX, lift_verify_mismatches(m), m->cpu.instructions, machine_now_ns(m) / 1000 };
    CHECK(m->cpu.halted, "loop reached hlt");
    machine_destroy(m);
    return r;
}

/* ---- INT 33h driver state in the snapshot (shadow verification) ---- */

/* A shadow run's lifted half is restored from a snapshot taken before the original ran; the driver's
 * position, mickeys and buttons are host-side, so a lifted function that reads them (AX=0Bh) sees the
 * original's values only when they are part of the DOS snapshot. */
static void test_mouse_snapshot(void)
{
    Machine *m = machine_create();
    bios_init(m); /* wires the VGA and the INT 33h driver (mouse_init) */
    size_t size = dos_snapshot_size();
    uint8_t *before = malloc(size), *after = malloc(size);
    mouse_host_motion(m, 320, 100, 640, 200);
    dos_snapshot_save(m, before);
    mouse_host_motion(m, 600, 190, 640, 200);
    mouse_host_button(m, 0, true);
    dos_snapshot_save(m, after);
    CHECK(memcmp(before, after, size) != 0, "mouse motion and buttons change the DOS snapshot");
    dos_snapshot_load(m, before);
    dos_snapshot_save(m, after);
    CHECK(memcmp(before, after, size) == 0, "loading the snapshot restores the INT 33h driver's state");
    /* Before mouse_init the driver's part is zeros and a load does nothing. */
    Machine *n = machine_create();
    memset(after, 0xFF, size);
    dos_snapshot_save(n, after);
    bool zero = true;
    for (size_t i = size - mouse_state_size(); i < size; i++)
        zero = zero && after[i] == 0;
    CHECK(zero, "a machine without a mouse saves a zeroed driver state");
    dos_snapshot_load(n, before);
    free(before);
    free(after);
    machine_destroy(n);
    machine_destroy(m);
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

/* The recorded load image: a DLL, driver or .ADV of another build never gets its entries armed. */
static uint64_t test_image_hash(const uint8_t *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ULL;
    return h;
}

static uint16_t run_program_here(Machine *m)
{
    memcpy(m->mem + real_addr(CODE_SEG, 0), PROGRAM, sizeof PROGRAM);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    SP = 0x100;
    m->cpu.eip = 0;
    m->cpu.halted = false;
    m->cpu.flags &= ~F_IF;
    AX = 0;
    for (int i = 0; i < 100 && !m->cpu.halted; i++)
        machine_step(m);
    CHECK(m->cpu.halted && SP == 0x100, "program reached hlt with a balanced stack");
    return AX;
}

static void test_module_image(void)
{
    enum { ORIGINAL_RESULT = 5 * 2 + 1, LIFTED_F_RESULT = 5 * 3 + 1 + 100 };
    static LiftedModule meta;
    meta.name = "TEST.EXE";
    meta.image_bytes = sizeof PROGRAM;
    meta.image_hash = test_image_hash(PROGRAM, sizeof PROGRAM);
    uint8_t other[sizeof PROGRAM];
    memcpy(other, PROGRAM, sizeof other);
    other[0] ^= 0xFF;

    Machine *m = machine_create();
    entries[0] = (LiftedEntry){ .module = "TEST.EXE", .off = F_OFF, .fn = lifted_f, .name = "fn",
                                .scratch_regs = LIFT_REG_FLAGS };
    memcpy(entries[0].signature, PROGRAM + F_OFF, LIFT_SIGNATURE_LEN);
    lift_add_table(m, entries, 1);
    lift_add_module(m, &meta);

    CHECK(!lift_module_loaded_image(m, "TEST.EXE", CODE_SEG, other, sizeof other),
          "an image of another build is not armed");
    CHECK(run_program_here(m) == ORIGINAL_RESULT, "another build runs its original (AX=%u)", AX);
    CHECK(lift_module_loaded_image(m, "TEST.EXE", CODE_SEG, PROGRAM, sizeof PROGRAM),
          "the recorded image is armed");
    CHECK(run_program_here(m) == LIFTED_F_RESULT, "the recorded image runs lifted (AX=%u)", AX);
    CHECK(!lift_module_loaded_image(m, "TEST.EXE", CODE_SEG, PROGRAM, sizeof PROGRAM - 1),
          "a truncated image is not armed");
    CHECK(run_program_here(m) == ORIGINAL_RESULT, "a truncated image runs the original (AX=%u)", AX);
    machine_destroy(m);
}

static void test_module_names(void)
{
    char name[16];
    lift_module_name("C:\\INN\\CAWDOR\\TWGENN.EXE", name, sizeof name);
    CHECK(!strcmp(name, "TWGENN-CAW.EXE"), "Cawdor gallery is its own module (%s)", name);
    lift_module_name("c:\\inn\\cawdor\\twgenn.exe", name, sizeof name);
    CHECK(!strcmp(name, "TWGENN-CAW.EXE"), "case-insensitive Cawdor gallery (%s)", name);
    lift_module_name("C:\\INN\\TWINION\\TWGENN.EXE", name, sizeof name);
    CHECK(!strcmp(name, "TWGENN.EXE"), "Twinion gallery keeps its name (%s)", name);
    lift_module_name("C:\\INN\\CAWDOR\\RUINS.EXE", name, sizeof name);
    CHECK(!strcmp(name, "RUINS.EXE"), "RUINS is its own module (%s)", name);
    lift_module_name("C:\\INN\\YSERBIUS\\DARKSTRT.EXE", name, sizeof name);
    CHECK(!strcmp(name, "DARKSTRT.EXE"), "a plain program name (%s)", name);
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
    test_module_names();
    test_module_image();
    /* INN_VERIFY_KEEP_TIME: a verified call costs what the original costs, no more. */
    Run plain = run_program(lifted_g, G_OFF, true, false, false);
    setenv("INN_VERIFY_KEEP_TIME", "1", 1);
    r = run_program(lifted_g, G_OFF, true, true, true);
    CHECK(r.ax == ORIGINAL_RESULT && r.mismatches == 0, "faithful G verifies with the clock kept (AX=%u, %d)", r.ax,
          r.mismatches);
    CHECK(r.insns == plain.insns && r.us == plain.us, "verified G takes the original's time (%llu insns %llu us, "
          "unhooked %llu %llu)", (unsigned long long)r.insns, (unsigned long long)r.us,
          (unsigned long long)plain.insns, (unsigned long long)plain.us);
    r = run_program(lifted_f, F_OFF, false, true, true);
    CHECK(r.mismatches == 1 && r.ax == ORIGINAL_RESULT, "unfaithful F is caught with the clock kept (%d, AX=%u)",
          r.mismatches, r.ax);
    CHECK(r.insns == plain.insns, "verified F takes the original's time (%llu insns, unhooked %llu)",
          (unsigned long long)r.insns, (unsigned long long)plain.insns);
    Run unhooked = run_loop(false);
    r = run_loop(true);
    CHECK(r.mismatches == 0 && r.ax == unhooked.ax, "3000 verified calls of G (AX=%u, %d mismatches)", r.ax,
          r.mismatches);
    CHECK(r.insns == unhooked.insns && r.us == unhooked.us, "3000 verified calls take the original's time "
          "(%llu insns %llu us, unhooked %llu %llu)", (unsigned long long)r.insns, (unsigned long long)r.us,
          (unsigned long long)unhooked.insns, (unsigned long long)unhooked.us);
    unsetenv("INN_VERIFY_KEEP_TIME");
    test_mouse_snapshot();
    test_overlays();
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    return failures != 0;
}
