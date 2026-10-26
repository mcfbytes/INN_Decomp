/* x87 and lifted code: the fpu.h API against instruction bytes, shadow verification of Cpu.fpu (and its
 * scratch bits), and emulator sequences patched by the original but not the lift (memory, signatures). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "devices.h"
#include "lift_internal.h"

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

enum { CODE_SEG = 0x1000, DATA_SEG = 0x0800, STACK_SEG = 0x2000 };

static Machine *new_machine(void)
{
    Machine *m = machine_create();
    pic_init(m); /* unmasked exceptions raise IRQ 13 */
    m->fpu_present = true;
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_DS, DATA_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    m->cpu.eip = 0;
    SP = 0x100;
    m->cpu.flags = 0x0002;
    return m;
}

static void put(Machine *m, uint16_t off, const void *v, size_t n) { memcpy(m->mem + real_addr(DATA_SEG, off), v, n); }
static void put16(Machine *m, uint16_t off, uint16_t v) { mem_wr16(m, real_addr(DATA_SEG, off), v); }

/* ---- part 1: the API does what the instruction bytes do ---- */

typedef struct Op { uint8_t opcode, x; bool mem; uint16_t disp; } Op; /* x: /reg for memory, else modrm */

#define M(op, reg, d) { op, reg, true, d }
#define R(op, modrm) { op, modrm, false, 0 }

enum { D_PI = 0x00, D_TWO = 0x08, D_THIRD32 = 0x10, D_I16 = 0x18, D_I32 = 0x1A, D_I64 = 0x20, D_E80 = 0x28,
       D_CW = 0x40, D_NEG = 0x48, D_BIG = 0x50, OUT = 0x100, DATA_BYTES = 0x400 };

static void setup_data(Machine *m, uint16_t cw)
{
    double pi = 3.141592653589793, two = 2.0, neg = -0.75, big = 1e300;
    float third = 1.0f / 3.0f;
    int16_t i16 = -7;
    int32_t i32 = 123456789;
    int64_t i64 = -98765432101234LL;
    uint8_t e80[10];
    fpu_to_ext80(1.0L / 3.0L, e80);
    put(m, D_PI, &pi, 8);
    put(m, D_TWO, &two, 8);
    put(m, D_THIRD32, &third, 4);
    put(m, D_I16, &i16, 2);
    put(m, D_I32, &i32, 4);
    put(m, D_I64, &i64, 8);
    put(m, D_E80, e80, 10);
    put16(m, D_CW, cw);
    put(m, D_NEG, &neg, 8);
    put(m, D_BIG, &big, 8);
}

/* Arithmetic under the CW at D_CW, stores in every format, compares, transcendentals, stack faults. */
static const Op SEQUENCE[] = {
    M(0xD9, 5, D_CW),                                   /* FLDCW */
    M(0xDD, 0, D_PI), M(0xDF, 0, D_I16), R(0xDE, 0xF9), /* pi / -7 */
    R(0xD9, 0xC0), R(0xD8, 0xC8), R(0xD9, 0xFA),        /* FLD ST0, FMUL ST0,ST0, FSQRT */
    M(0xD8, 0, D_THIRD32), M(0xDC, 4, D_TWO), M(0xDA, 7, D_I32), M(0xDE, 1, D_I16), M(0xDC, 6, D_PI),
    M(0xDD, 2, OUT + 0x00), M(0xD9, 2, OUT + 0x08), M(0xDF, 2, OUT + 0x0C), M(0xDB, 2, OUT + 0x0E),
    M(0xDB, 5, D_E80), R(0xDE, 0xC1), M(0xDF, 5, D_I64), R(0xDE, 0xE9), /* FADDP, FILD m64, FSUBP */
    R(0xD9, 0xC0), R(0xD9, 0xFC), M(0xDF, 7, OUT + 0x12), M(0xDB, 7, OUT + 0x1A), /* FRNDINT, FISTP m64, FSTP m80 */
    M(0xDD, 0, D_TWO), M(0xDC, 2, D_PI), M(0xDD, 7, OUT + 0x24), R(0xDF, 0xE0), /* FCOM, FSTSW m, FSTSW AX */
    M(0xDC, 3, D_TWO), R(0xD9, 0xE4), R(0xD9, 0xE5), M(0xDD, 7, OUT + 0x26),      /* FCOMP, FTST, FXAM */
    R(0xD9, 0xE0), R(0xD9, 0xE1), R(0xD9, 0xC9), R(0xDD, 0xD9),                   /* FCHS FABS FXCH FSTP ST1 */
    R(0xD9, 0xE8), R(0xD9, 0xEB), R(0xD9, 0xF3), R(0xD9, 0xC0), R(0xD9, 0xFE), R(0xD9, 0xC9), R(0xD9, 0xFF),
    R(0xD9, 0xF2), R(0xDE, 0xF9), R(0xD9, 0xEA), R(0xD9, 0xF1), R(0xD9, 0xF0), R(0xD9, 0xED), R(0xD9, 0xFD),
    R(0xD9, 0xEE), R(0xDE, 0xD9), M(0xDD, 7, OUT + 0x28),                          /* FLDZ, FCOMPP */
    M(0xDD, 0, D_NEG), R(0xD9, 0xFA), M(0xDD, 3, OUT + 0x2A),                      /* sqrt(-0.75): IE */
    M(0xDD, 0, D_BIG), M(0xD8, 3, D_PI), M(0xDD, 0, D_BIG), M(0xD9, 3, OUT + 0x32), /* m32 overflow */
    M(0xDD, 0, D_BIG), M(0xDF, 3, OUT + 0x36),                                      /* integer indefinite */
    R(0xD9, 0xE8), R(0xD9, 0xE8), R(0xD9, 0xE8), R(0xD9, 0xE8), R(0xD9, 0xE8), R(0xD9, 0xE8), R(0xD9, 0xE8),
    R(0xD9, 0xE8), R(0xD9, 0xE8), M(0xD9, 7, OUT + 0x38), M(0xDD, 7, OUT + 0x3A),  /* 9 pushes: overflow */
};

static int assemble(uint8_t *code, const Op *ops, int n)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        code[k++] = 0x9B;
        code[k++] = ops[i].opcode;
        if (ops[i].mem) {
            code[k++] = (uint8_t)(ops[i].x << 3 | 6);
            code[k++] = (uint8_t)ops[i].disp;
            code[k++] = (uint8_t)(ops[i].disp >> 8);
        } else {
            code[k++] = ops[i].x;
        }
    }
    code[k++] = 0xF4;
    return k;
}

static void compare_runs(Machine *a, Machine *b, const char *what)
{
    char why[200] = "";
    CHECK(fpu_state_matches(&b->cpu.fpu, &a->cpu.fpu, false, why, sizeof why), "%s: API differs: %s", what, why);
    CHECK(memcmp(a->mem + real_addr(DATA_SEG, 0), b->mem + real_addr(DATA_SEG, 0), DATA_BYTES) == 0,
          "%s: stored operands differ", what);
    CHECK(a->cpu.r[R_AX].w == b->cpu.r[R_AX].w, "%s: FSTSW AX %04X vs %04X", what, a->cpu.r[R_AX].w, b->cpu.r[R_AX].w);
}

static void test_api_matches_instructions(void)
{
    static const uint16_t CWS[] = { 0x037F, 0x027F, 0x007F, 0x077F, 0x0B7F, 0x0F7F, 0x1332, 0x037E };
    static uint8_t code[2048];
    int n = (int)(sizeof SEQUENCE / sizeof SEQUENCE[0]);
    int len = assemble(code, SEQUENCE, n);
    for (size_t c = 0; c < sizeof CWS / sizeof CWS[0]; c++) {
        char what[32];
        snprintf(what, sizeof what, "CW %04X", CWS[c]);
        Machine *a = new_machine(), *b = new_machine();
        setup_data(a, CWS[c]);
        setup_data(b, CWS[c]);
        memcpy(a->mem + real_addr(CODE_SEG, 0), code, (size_t)len);
        cpu_run(a, 10000);
        CHECK(a->cpu.halted, "%s: the instructions ran to HLT", what);
        for (int i = 0; i < n; i++) {
            if (SEQUENCE[i].mem)
                fpu_mem(b, SEQUENCE[i].opcode, SEQUENCE[i].x, DATA_SEG, SEQUENCE[i].disp);
            else
                fpu_reg(b, SEQUENCE[i].opcode, SEQUENCE[i].x);
        }
        compare_runs(a, b, what);
        machine_destroy(a);
        machine_destroy(b);
    }
}

/* The named wrappers are the opcodes they say, and the value helpers read and change the stack. */
static void test_named_api(void)
{
    Machine *a = new_machine(), *b = new_machine();
    setup_data(a, 0x027F);
    setup_data(b, 0x027F);
    const Op ops[] = { M(0xD9, 5, D_CW), M(0xDD, 0, D_PI), M(0xDC, 0, D_TWO), M(0xDE, 6, D_I16), M(0xDB, 0, D_I32),
                       R(0xDE, 0xF1), R(0xD9, 0xFC), M(0xDF, 3, OUT), M(0xD9, 0, D_THIRD32), M(0xD8, 5, D_THIRD32),
                       M(0xDD, 3, OUT + 2) };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
        ops[i].mem ? fpu_mem(a, ops[i].opcode, ops[i].x, DATA_SEG, ops[i].disp) : fpu_reg(a, ops[i].opcode, ops[i].x);
    fpu_fldcw_m(b, DATA_SEG, D_CW);
    fpu_fld_m64(b, DATA_SEG, D_PI);
    fpu_fadd_m64(b, DATA_SEG, D_TWO);
    fpu_fdiv_i16(b, DATA_SEG, D_I16);
    fpu_fild_m32(b, DATA_SEG, D_I32);
    fpu_fdivrp(b, 1);
    fpu_frndint(b);
    fpu_fistp_m16(b, DATA_SEG, OUT);
    fpu_fld_m32(b, DATA_SEG, D_THIRD32);
    fpu_fsubr_m32(b, DATA_SEG, D_THIRD32);
    fpu_fstp_m64(b, DATA_SEG, OUT + 2);
    compare_runs(a, b, "named wrappers");
    CHECK(fpu_top(b) == 0 && b->cpu.fpu.empty == 0xFF, "stack balanced");
    fpu_push(b, 2.5L);
    fpu_push(b, 4.0L);
    fpu_fmulp(b, 1);
    CHECK(fpu_st(b, 0) == 10.0L && !fpu_st_empty(b, 0) && fpu_st_empty(b, 1), "push, push, FMULP = 10");
    CHECK(fpu_pop(b) == 10.0L && fpu_top(b) == 0 && b->cpu.fpu.empty == 0xFF, "pop returns ST(0) and empties it");
    fpu_fld_m64(b, DATA_SEG, D_TWO);
    fpu_fcom_m64(b, DATA_SEG, D_PI);
    CHECK(fpu_c0(b) && !fpu_c3(b) && !fpu_c2(b), "2 < pi sets C0 only");
    machine_destroy(a);
    machine_destroy(b);
}

/* ---- part 2: shadow verification compares the FPU ---- */

/* main: FLD1, FLDPI, call far F, HLT. Functions F follow at their own offsets. */
enum { F_ADD = 0x20, F_CMP = 0x30, F_FTOL = 0x40, F_EMU = 0x80, ISR = 0xC0, ISR_BAD = 0xE0, ISR_WRONG = 0x100,
       D_CHOP = 0x60, D_SAVE = 0x62, D_RESULT = 0x64, D_COUNT = 0x66 };

static const uint8_t MAIN[] = { 0x9B, 0xD9, 0xE8, 0x9B, 0xD9, 0xEB, 0x9A, 0x00, 0x00, CODE_SEG & 0xFF, CODE_SEG >> 8,
                                0xF4 };

static const uint8_t FUNCTIONS[] = {
    [F_ADD] = 0x9B, 0xD8, 0xC1, 0xCB,                       /* FADD ST,ST(1) */
    [F_CMP] = 0x9B, 0xD8, 0xD1, 0xCB,                       /* FCOM ST(1) */
    /* float to int as compilers emit it: save CW, load a chopping CW, FISTP m16, restore CW */
    [F_FTOL] = 0x9B, 0xD9, 0x3E, D_SAVE, 0x00, 0x9B, 0xD9, 0x2E, D_CHOP, 0x00, 0x9B, 0xDF, 0x1E, D_RESULT, 0x00,
               0x9B, 0xD9, 0x2E, D_SAVE, 0x00, 0xCB,
    [F_EMU] = 0xCD, 0x34, 0xC1, 0xCB,                       /* INT 34h = FWAIT; FADD ST,ST(1) */
    /* An emulator with a coprocessor: BX = return address - 2, write FWAIT + escape there, return to it. */
    [ISR] = 0x55, 0x8B, 0xEC, 0x53, 0x1E, 0xC5, 0x5E, 0x02, 0x4B, 0x4B, 0xC6, 0x07, 0x9B, 0x80, 0x47, 0x01, 0xA4,
            0x89, 0x5E, 0x02, 0x1F, 0x5B, 0x5D, 0xCF,
    /* The same, also counting in data: an effect besides the patch */
    [ISR_BAD] = 0x55, 0x8B, 0xEC, 0x53, 0x1E, 0xC5, 0x5E, 0x02, 0x4B, 0x4B, 0xC6, 0x07, 0x9B, 0x80, 0x47, 0x01,
                0xA4, 0x89, 0x5E, 0x02, 0x1F, 0x5B, 0x5D, 0xFF, 0x06, D_COUNT, 0x00, 0xCF,
    /* A wrong patch: 9B DC (FADD ST(1),ST) instead of 9B D8 */
    [ISR_WRONG] = 0x55, 0x8B, 0xEC, 0x53, 0x1E, 0xC5, 0x5E, 0x02, 0x4B, 0x4B, 0xC6, 0x07, 0x9B, 0x80, 0x47, 0x01,
                  0xA8, 0x89, 0x5E, 0x02, 0x1F, 0x5B, 0x5D, 0xCF,
};

static int lifted_runs;

static void lift_add_good(Machine *m) { fpu_fadd_st(m, 1); lifted_runs++; lift_ret_far(m, 0); }
static void lift_add_wrong(Machine *m) { fpu_fsub_st(m, 1); lift_ret_far(m, 0); }
static void lift_add_extra_push(Machine *m) { fpu_fadd_st(m, 1); fpu_fld1(m); lift_ret_far(m, 0); }
static void lift_nothing(Machine *m) { lift_ret_far(m, 0); }
static void lift_cmp_wrong_cc(Machine *m) { fpu_fxam(m); lift_ret_far(m, 0); }
static void lift_cmp(Machine *m) { fpu_fcom_st(m, 1); lift_ret_far(m, 0); }
static void lift_ftol_good(Machine *m)
{
    fpu_fstcw_m(m, DATA_SEG, D_SAVE);
    fpu_fldcw_m(m, DATA_SEG, D_CHOP);
    fpu_fistp_m16(m, DATA_SEG, D_RESULT);
    fpu_fldcw_m(m, DATA_SEG, D_SAVE);
    lift_ret_far(m, 0);
}
/* Forgets the chopping CW: rounds to nearest, so pi becomes 3 either way but 3.5 becomes 4, not 3. */
static void lift_ftol_wrong_rounding(Machine *m)
{
    fpu_fstcw_m(m, DATA_SEG, D_SAVE);
    fpu_fistp_m16(m, DATA_SEG, D_RESULT);
    lift_ret_far(m, 0);
}
/* Executes the patched form's FADD, then writes a differently patched sequence over its own code. */
static void lift_add_writes_code(Machine *m)
{
    fpu_fadd_st(m, 1);
    mem_wr8(m, real_addr(CODE_SEG, F_EMU), 0x9B);
    mem_wr8(m, real_addr(CODE_SEG, F_EMU + 1), 0xD9);
    lift_ret_far(m, 0);
}
static void lift_add_to_st1(Machine *m) { fpu_fadd_to(m, 1); lift_ret_far(m, 0); }

typedef struct Case {
    uint16_t entry;
    LiftedFn fn;
    uint32_t scratch;
    uint16_t isr;
    bool half;        /* main loads 3.5 instead of pi (FLDPI becomes FLD m64) */
    bool prepatch;    /* the entry already holds the patched form before the first call */
    uint16_t emu_mask;
} Case;

typedef struct Result { int mismatches; uint8_t entry[3]; int runs; uint16_t result; } Result;

static Result run_case(Case c)
{
    static LiftedEntry e[1];
    Machine *m = new_machine();
    uint8_t main[sizeof MAIN];
    memcpy(main, MAIN, sizeof main);
    main[7] = (uint8_t)c.entry;
    put16(m, D_CHOP, 0x0F7F);
    memcpy(m->mem + real_addr(CODE_SEG, 0), FUNCTIONS, sizeof FUNCTIONS);
    if (c.half) { /* FLD1, FLD qword [D_TWO + 8] holding 3.5, call far F, HLT */
        double v = 3.5;
        put(m, D_TWO + 8, &v, 8);
        const uint8_t code[] = { 0x9B, 0xD9, 0xE8, 0x9B, 0xDD, 0x06, D_TWO + 8, 0x00, 0x9A, (uint8_t)c.entry, 0x00,
                                 CODE_SEG & 0xFF, CODE_SEG >> 8, 0xF4 };
        memcpy(m->mem + real_addr(CODE_SEG, 0), code, sizeof code);
    } else {
        memcpy(m->mem + real_addr(CODE_SEG, 0), main, sizeof main);
    }
    e[0] = (LiftedEntry){ .module = "TEST.EXE", .off = c.entry, .fn = c.fn, .name = "f",
                          .scratch_regs = LIFT_REG_FLAGS | c.scratch, .emu_mask = c.emu_mask };
    memcpy(e[0].signature, FUNCTIONS + c.entry, LIFT_SIGNATURE_LEN);
    if (c.isr) {
        mem_wr16(m, 0x34 * 4, c.isr);
        mem_wr16(m, 0x34 * 4 + 2, CODE_SEG);
    }
    if (c.prepatch) {
        mem_wr8(m, real_addr(CODE_SEG, c.entry), 0x9B);
        mem_wr8(m, real_addr(CODE_SEG, c.entry + 1), 0xD8);
    }
    lift_add_table(m, e, 1);
    if (!c.prepatch)
        lift_verify_enable(m, 10);
    lift_module_loaded(m, "TEST.EXE", CODE_SEG, sizeof FUNCTIONS);
    lifted_runs = 0;
    for (int i = 0; i < 200 && !m->cpu.halted; i++)
        machine_step(m);
    CHECK(m->cpu.halted, "program reached HLT");
    Result r = { lift_verify_mismatches(m), { 0 }, lifted_runs, mem_rd16(m, real_addr(DATA_SEG, D_RESULT)) };
    for (int i = 0; i < 3; i++)
        r.entry[i] = mem_rd8(m, real_addr(CODE_SEG, c.entry + i));
    machine_destroy(m);
    return r;
}

static int mismatches(Case c) { return run_case(c).mismatches; }

static void test_shadow_compares_fpu(void)
{
    CHECK(mismatches((Case){ .entry = F_ADD, .fn = lift_add_good }) == 0, "faithful FADD lift");
    CHECK(mismatches((Case){ .entry = F_ADD, .fn = lift_add_wrong }) == 1, "FSUB for FADD is a register mismatch");
    CHECK(mismatches((Case){ .entry = F_ADD, .fn = lift_add_extra_push }) == 1, "an extra push is a TOP mismatch");
    CHECK(mismatches((Case){ .entry = F_ADD, .fn = lift_nothing, .scratch = LIFT_REG_FPU }) == 0,
          "LIFT_REG_FPU: the FPU may differ");
    CHECK(mismatches((Case){ .entry = F_CMP, .fn = lift_cmp }) == 0, "faithful FCOM lift");
    CHECK(mismatches((Case){ .entry = F_CMP, .fn = lift_cmp_wrong_cc }) == 1, "other C0-C3 are a mismatch");
    CHECK(mismatches((Case){ .entry = F_CMP, .fn = lift_cmp_wrong_cc, .scratch = LIFT_REG_FPU_CC }) == 0,
          "LIFT_REG_FPU_CC: C0-C3 may differ");
    CHECK(mismatches((Case){ .entry = F_CMP, .fn = lift_nothing, .scratch = LIFT_REG_FPU_CC }) == 0,
          "FCOM leaves only C0-C3");
    Result pi = run_case((Case){ .entry = F_FTOL, .fn = lift_ftol_good });
    CHECK(pi.mismatches == 0 && pi.result == 3, "chopping FISTP lift (pi -> %u)", pi.result);
    Result half = run_case((Case){ .entry = F_FTOL, .fn = lift_ftol_good, .half = true });
    CHECK(half.mismatches == 0 && half.result == 3, "chopping FISTP lift (3.5 -> %u)", half.result);
    CHECK(mismatches((Case){ .entry = F_FTOL, .fn = lift_ftol_wrong_rounding, .half = true }) == 1,
          "a lift that rounds to nearest instead of chopping is caught");
}

/* ---- part 3: emulator sequences ---- */

static void test_emulator_patches(void)
{
    Result r = run_case((Case){ .entry = F_EMU, .fn = lift_add_good, .isr = ISR, .emu_mask = 1 });
    CHECK(r.mismatches == 0, "INT 34h patched by the original, left by the lift: equal");
    CHECK(r.entry[0] == 0x9B && r.entry[1] == 0xD8 && r.entry[2] == 0xC1, "the game continues from the patched code");
    CHECK(mismatches((Case){ .entry = F_EMU, .fn = lift_add_good, .isr = ISR_BAD, .emu_mask = 1 }) == 1,
          "an effect besides the patch is reported");
    CHECK(mismatches((Case){ .entry = F_EMU, .fn = lift_add_to_st1, .isr = ISR_WRONG, .emu_mask = 1 }) == 1,
          "a patch that is not the sequence's x87 form is reported");
    CHECK(mismatches((Case){ .entry = F_EMU, .fn = lift_add_writes_code, .isr = ISR, .emu_mask = 1 }) == 1,
          "lifted code holding another form is reported");

    /* Entry signatures: the file holds CD 34 C1; memory may already hold 9B D8 C1. */
    r = run_case((Case){ .entry = F_EMU, .fn = lift_add_good, .isr = ISR, .prepatch = true, .emu_mask = 1 });
    CHECK(r.runs == 1, "patched entry matches a signature marked in emu_mask (lift ran %d times)", r.runs);
    r = run_case((Case){ .entry = F_EMU, .fn = lift_add_good, .isr = ISR, .prepatch = true, .emu_mask = 0 });
    CHECK(r.runs == 0, "without the mark the patched entry is another build");
    r = run_case((Case){ .entry = F_EMU, .fn = lift_add_good, .isr = ISR, .emu_mask = 1 });
    CHECK(r.runs == 1, "unpatched entry with the mark");

    /* The three forms, both ways. */
    static const struct { uint8_t i[3], p[3]; int len; } FORMS[] = {
        { { 0xCD, 0x34, 0xC1 }, { 0x9B, 0xD8, 0xC1 }, 2 }, { { 0xCD, 0x3B, 0x46 }, { 0x9B, 0xDF, 0x46 }, 2 },
        { { 0xCD, 0x3C, 0x1D }, { 0x9B, 0x3E, 0xDD }, 3 }, { { 0xCD, 0x3C, 0x5B }, { 0x9B, 0x2E, 0xDB }, 3 },
        { { 0xCD, 0x3C, 0x99 }, { 0x9B, 0x36, 0xD9 }, 3 }, { { 0xCD, 0x3C, 0xDD }, { 0x9B, 0x26, 0xDD }, 3 },
        { { 0xCD, 0x3D, 0x00 }, { 0x90, 0x9B, 0x00 }, 2 },
    };
    for (size_t k = 0; k < sizeof FORMS / sizeof FORMS[0]; k++) {
        uint8_t back[3];
        CHECK(lift_emu_int_len(FORMS[k].i, 3) == FORMS[k].len, "INT form %zu length", k);
        CHECK(lift_emu_is_patch_of(FORMS[k].i, FORMS[k].p, FORMS[k].len), "form %zu is the patch", k);
        int len = FORMS[k].len;
        CHECK(lift_emu_to_int(FORMS[k].p, 3, back) == len && memcmp(back, FORMS[k].i, (size_t)len) == 0,
              "form %zu converts back", k);
        CHECK(!lift_emu_is_patch_of(FORMS[k].p, FORMS[k].i, FORMS[k].len), "form %zu: not the other way round", k);
    }
    const uint8_t int34[] = { 0xCD, 0x34 }, wrong[] = { 0x9B, 0xD9 }, int3e[] = { 0xCD, 0x3E }, cut[] = { 0xCD, 0x3C };
    CHECK(!lift_emu_is_patch_of(int34, wrong, 2), "9B D9 is not the patch of CD 34");
    CHECK(lift_emu_int_len(int3e, 2) == 0, "INT 3Eh is no sequence");
    CHECK(lift_emu_int_len(cut, 2) == 0, "a cut-off CD 3C is no sequence");
}

int main(void)
{
    test_api_matches_instructions();
    test_named_api();
    test_shadow_compares_fpu();
    test_emulator_patches();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_lift_fpu: ok");
    return 0;
}
