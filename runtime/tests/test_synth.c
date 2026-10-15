/* Synthetic calls (lift_synth.c): calls from a call list are verified when a trigger is reached,
 * and the program then continues from exactly the trigger state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lift_internal.h"

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

enum { CODE_SEG = 0x1000, DATA_SEG = 0x3000, STACK_SEG = 0x2000, T_OFF = 0x40, F_OFF = 0x50, G_OFF = 0x70,
       H_OFF = 0x80 };

/* main: mov cx,3 / call far T / loop / mov ax,1234h / hlt.  T: retf (the trigger).
 * F(a, b, far *p): *p = a + b, returns it in AX.  G(a, b) near, callee pops: returns a - b + DX.
 * H: never returns. */
static const uint8_t PROGRAM[] = {
    [0x00] = 0xB9, 0x03, 0x00, 0x9A, T_OFF, 0x00, CODE_SEG & 0xFF, CODE_SEG >> 8, 0xE2, 0xF9, 0xB8, 0x34, 0x12, 0xF4,
    [T_OFF] = 0xCB,
    [F_OFF] = 0x55, 0x8B, 0xEC, 0x8B, 0x46, 0x06, 0x03, 0x46, 0x08, 0xC4, 0x5E, 0x0A, 0x26, 0x89, 0x07, 0x5D, 0xCB,
    [G_OFF] = 0x55, 0x8B, 0xEC, 0x8B, 0x46, 0x04, 0x2B, 0x46, 0x06, 0x03, 0xC2, 0x5D, 0xC2, 0x04, 0x00,
    [H_OFF] = 0xEB, 0xFE, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
};

static uint16_t seen_a, seen_b, seen_seg;
static int f_runs;

static void f_store(Machine *m, int16_t bias, int16_t ax_bias)
{
    uint16_t a = stack_arg16(m, 4), b = stack_arg16(m, 6), off = stack_arg16(m, 8), seg = stack_arg16(m, 10);
    seen_a = a;
    seen_b = b;
    seen_seg = seg;
    f_runs++;
    mem_wr16(m, real_addr(seg, off), (uint16_t)(a + b + bias));
    AX = (uint16_t)(a + b + ax_bias);
    BX = off;
    cpu_set_seg(&m->cpu, S_ES, seg);
    lift_ret_far(m, 0);
}

static void f_good(Machine *m) { f_store(m, 0, 0); }
static void f_bad_memory(Machine *m) { f_store(m, 1, 0); }
static void f_bad_result(Machine *m) { f_store(m, 0, 1); }

static void g_good(Machine *m)
{
    AX = (uint16_t)(stack_arg16(m, 2) - stack_arg16(m, 4) + DX);
    lift_ret_near(m, 4);
}

static void h_returns(Machine *m) { lift_ret_far(m, 0); }

static LiftedEntry entries[3];

typedef struct Run { int mismatches, synth_calls[3], synth_mismatches[3], result_diffs; } Run;

static Run run(LiftedFn f, const char *list, uint16_t f_scratch)
{
    char path[64];
    snprintf(path, sizeof path, "synth_test_%d.txt", (int)getpid());
    FILE *out = fopen(path, "w");
    fputs(list, out);
    fclose(out);
    Machine *m = machine_create();
    memcpy(m->mem + real_addr(CODE_SEG, 0), PROGRAM, sizeof PROGRAM);
    mem_wr16(m, real_addr(DATA_SEG, 0x200), 5);
    entries[0] = (LiftedEntry){ .module = "TEST.EXE", .off = F_OFF, .fn = f, .name = "f", .scratch_regs = f_scratch };
    entries[1] = (LiftedEntry){ .module = "TEST.EXE", .off = G_OFF, .fn = g_good, .name = "g", .near_return = true,
                                .scratch_regs = LIFT_REG_FLAGS };
    entries[2] = (LiftedEntry){ .module = "TEST.EXE", .off = H_OFF, .fn = h_returns, .name = "h",
                                .scratch_regs = LIFT_REG_FLAGS };
    for (int i = 0; i < 3; i++)
        memcpy(entries[i].signature, PROGRAM + entries[i].off, LIFT_SIGNATURE_LEN);
    lift_add_table(m, entries, 3);
    setenv("INN_SYNTH_CHECK", "1", 1); /* a restore that is not exact ends the test */
    lift_synth_load(m, path);
    remove(path);
    lift_module_loaded(m, "TEST.EXE", CODE_SEG, sizeof PROGRAM);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    cpu_set_seg(&m->cpu, S_DS, DATA_SEG);
    SP = 0x100;
    DX = 0x10;
    m->cpu.eip = 0;
    m->cpu.flags &= ~F_IF;
    for (int i = 0; i < 1000 && !m->cpu.halted; i++)
        machine_step(m);
    CHECK(m->cpu.halted && AX == 0x1234 && CX == 0 && SP == 0x100 && DX == 0x10,
          "the program continues unaffected (AX=%04X CX=%04X SP=%04X DX=%04X)", AX, CX, SP, DX);
    CHECK(mem_rd16(m, real_addr(DATA_SEG, 0x200)) == 5, "memory written by calls is restored");
    CHECK(!m->a20, "A20 is restored");
    LiftState *s = lift_state(m);
    Run r = { .mismatches = lift_verify_mismatches(m), .result_diffs = s->tables[0].stats[0].synth_result_diffs };
    for (int i = 0; i < 3; i++) {
        r.synth_calls[i] = s->tables[0].stats[i].synth_calls;
        r.synth_mismatches[i] = s->tables[0].stats[i].synth_mismatches;
        CHECK(s->tables[0].stats[i].shadow_calls == 0, "no live call was verified");
    }
    machine_destroy(m);
    return r;
}

static const char LIST[] =
    "# comment\n"
    "dgroup TEST.EXE 2000\n"
    "after TEST.EXE 0000:0040 2   # the second entry of T\n"
    "call TEST.EXE f 3 4 buf(2)\n"
    "call TEST.EXE f -1 1 far(DS,0x200)\n"
    "call TEST.EXE f [DS:0x200] 0x7FFF p=buf(8,\"ab#\",fill=0xCC)\n"
    "call TEST.EXE 0000:0050 'a' 2 buf(4)+2 DS=DGROUP\n"
    "call TEST.EXE g 10 3 DX=0x100\n";

int main(void)
{
    enum { F_SCRATCH = LIFT_REG_BX | LIFT_REG_ES | LIFT_REG_FLAGS };
    Run r = run(f_good, LIST, F_SCRATCH);
    CHECK(r.synth_calls[0] == 4 && r.synth_calls[1] == 1, "every call ran (f %d, g %d)", r.synth_calls[0],
          r.synth_calls[1]);
    CHECK(r.mismatches == 0, "faithful lifts verify (%d mismatches)", r.mismatches);
    CHECK(seen_a == 'a' && seen_b == 2 && seen_seg == 0xFFFF, "arguments and HMA buffers (a=%04X b=%04X seg=%04X)",
          seen_a, seen_b, seen_seg);

    f_runs = 0;
    r = run(f_bad_memory, LIST, F_SCRATCH);
    CHECK(r.synth_mismatches[0] == 4 && r.mismatches == 4, "a wrong store is a mismatch in every call (%d)",
          r.synth_mismatches[0]);
    CHECK(f_runs == 4, "the lift ran once per call (%d)", f_runs);

    r = run(f_bad_result, LIST, F_SCRATCH | LIFT_REG_AX);
    CHECK(r.mismatches == 0 && r.result_diffs == 4, "a wrong scratch AX is noted, not a mismatch (%d, %d)",
          r.mismatches, r.result_diffs);
    r = run(f_bad_result, LIST, F_SCRATCH);
    CHECK(r.mismatches == 4, "with AX not scratch (*_RET_AX) a wrong result is a mismatch (%d)", r.mismatches);

    r = run(f_good, "after TEST.EXE 0000:0040 3\ncall TEST.EXE g 1 2\ncall TEST.EXE h\n", F_SCRATCH);
    CHECK(r.synth_calls[1] == 1 && r.synth_mismatches[2] == 1, "a call that never returns is reported (%d)",
          r.synth_mismatches[2]);

    r = run(f_good, "after TEST.EXE 0000:0040 4\ncall TEST.EXE f 1 2 buf(2)\n", F_SCRATCH);
    CHECK(r.synth_calls[0] == 0, "a trigger that is not reached fires no call");

    r = run(f_good, "after TEST.EXE f 1\ncall TEST.EXE g 1 2\n", F_SCRATCH);
    CHECK(r.synth_calls[1] == 0, "a lifted entry never entered fires no call");
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    return failures != 0;
}
