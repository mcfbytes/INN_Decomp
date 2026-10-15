/* x87 tests: detection probes, every load/store format, rounding and precision control, compares,
 * transcendentals, stack faults, exceptions, FSTENV/FSAVE; real instruction bytes through the CPU. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "machine.h"

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

enum { CODE_SEG = 0x0700, DATA_SEG = 0x0800, STACK_SEG = 0x0600 };

/* ---- a tiny assembler ---- */

typedef struct Code {
    uint8_t b[512];
    int n;
} Code;

static void emit(Code *c, int byte) { c->b[c->n++] = (uint8_t)byte; }

/* <opcode> /reg [disp16] (DS-relative) */
static void mem_op(Code *c, int opcode, int reg, uint16_t disp)
{
    emit(c, opcode);
    emit(c, reg << 3 | 6);
    emit(c, disp & 0xFF);
    emit(c, disp >> 8);
}

static void reg_op(Code *c, int opcode, int modrm)
{
    emit(c, opcode);
    emit(c, modrm);
}

#define FLD_M32(c, d) mem_op(c, 0xD9, 0, d)
#define FST_M32(c, d) mem_op(c, 0xD9, 2, d)
#define FLDCW(c, d) mem_op(c, 0xD9, 5, d)
#define FSTENV(c, d) mem_op(c, 0xD9, 6, d)
#define FSTCW(c, d) mem_op(c, 0xD9, 7, d)
#define FILD_M32(c, d) mem_op(c, 0xDB, 0, d)
#define FISTP_M32(c, d) mem_op(c, 0xDB, 3, d)
#define FLD_M80(c, d) mem_op(c, 0xDB, 5, d)
#define FSTP_M80(c, d) mem_op(c, 0xDB, 7, d)
#define FLD_M64(c, d) mem_op(c, 0xDD, 0, d)
#define FSTP_M64(c, d) mem_op(c, 0xDD, 3, d)
#define FRSTOR(c, d) mem_op(c, 0xDD, 4, d)
#define FSAVE(c, d) mem_op(c, 0xDD, 6, d)
#define FSTSW_M(c, d) mem_op(c, 0xDD, 7, d)
#define FILD_M16(c, d) mem_op(c, 0xDF, 0, d)
#define FISTP_M16(c, d) mem_op(c, 0xDF, 3, d)
#define FBLD(c, d) mem_op(c, 0xDF, 4, d)
#define FILD_M64(c, d) mem_op(c, 0xDF, 5, d)
#define FBSTP(c, d) mem_op(c, 0xDF, 6, d)
#define FISTP_M64(c, d) mem_op(c, 0xDF, 7, d)
#define FADD_M64(c, d) mem_op(c, 0xDC, 0, d)
#define FDIV_M64(c, d) mem_op(c, 0xDC, 6, d)
#define FCOMP_M64(c, d) mem_op(c, 0xDC, 3, d)
#define FIMUL_M16(c, d) mem_op(c, 0xDE, 1, d)
#define FNINIT(c) reg_op(c, 0xDB, 0xE3)
#define FNCLEX(c) reg_op(c, 0xDB, 0xE2)
#define FSTSW_AX(c) reg_op(c, 0xDF, 0xE0)
#define D9(c, x) reg_op(c, 0xD9, x)

static Machine *m;

static void run(Code *c, bool fpu)
{
    m = machine_create();
    m->fpu_present = fpu;
    emit(c, 0xF4); /* HLT */
    memcpy(m->mem + real_addr(CODE_SEG, 0), c->b, (size_t)c->n);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_DS, DATA_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    m->cpu.eip = 0;
    m->cpu.r[R_SP].w = 0xFFFE;
    m->cpu.flags = 0x0002; /* interrupts off: no device is set up */
    cpu_run(m, 2000);
    CHECK(m->cpu.halted, "code did not reach HLT (stopped at %04X)", m->cpu.eip);
}

static uint8_t *data(uint16_t off) { return m->mem + real_addr(DATA_SEG, off); }
static void put_bytes(Machine *mm, uint16_t off, const void *v, size_t n) { memcpy(mm->mem + real_addr(DATA_SEG, off), v, n); }
static double get_f64(uint16_t off) { double v; memcpy(&v, data(off), 8); return v; }
static float get_f32(uint16_t off) { float v; memcpy(&v, data(off), 4); return v; }
static int32_t get_i32(uint16_t off) { int32_t v; memcpy(&v, data(off), 4); return v; }
static uint16_t get_u16(uint16_t off) { return (uint16_t)(data(off)[0] | data(off)[1] << 8); }

/* Data must be placed after machine_create, so cases build code, then run with a setup hook. */
typedef void (*Setup)(Machine *);

static void run_with(Code *c, Setup setup)
{
    m = machine_create();
    m->fpu_present = true;
    if (setup)
        setup(m);
    Machine *keep = m;
    emit(c, 0xF4);
    memcpy(keep->mem + real_addr(CODE_SEG, 0), c->b, (size_t)c->n);
    cpu_set_seg(&keep->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&keep->cpu, S_DS, DATA_SEG);
    cpu_set_seg(&keep->cpu, S_SS, STACK_SEG);
    keep->cpu.eip = 0;
    keep->cpu.r[R_SP].w = 0xFFFE;
    keep->cpu.flags = 0x0002;
    cpu_run(keep, 2000);
    CHECK(keep->cpu.halted, "code did not reach HLT (stopped at %04X)", keep->cpu.eip);
}

static void done(void) { machine_destroy(m); }

static void put_f64(Machine *mm, uint16_t off, double v) { put_bytes(mm, off, &v, 8); }

/* ---- cases ---- */

/* The Intel/Borland probe: FNINIT, FNSTSW to a word preset non-zero, FNSTCW masked to 103Fh. */
static void test_detection(void)
{
    for (int present = 0; present < 2; present++) {
        Code c = {0};
        FNINIT(&c);
        FSTSW_M(&c, 0x10);
        FSTCW(&c, 0x12);
        run(&c, present); /* memory starts zeroed: without an FPU nothing is stored */
        uint16_t sw = get_u16(0x10), cw = get_u16(0x12);
        if (present) {
            CHECK(sw == 0, "FNSTSW after FNINIT %04X", sw);
            CHECK(cw == 0x037F, "FNSTCW after FNINIT %04X", cw);
        } else {
            CHECK(cw == 0, "no FPU: FNSTCW must not store (%04X)", cw);
        }
        done();
    }
    Code c = {0};
    FNINIT(&c);
    FSTSW_AX(&c);
    run(&c, true);
    CHECK(m->cpu.r[R_AX].w == 0, "FNSTSW AX %04X", m->cpu.r[R_AX].w);
    done();
}

static void setup_pi(Machine *mm)
{
    put_f64(mm, 0x00, 3.141592653589793);
    put_f64(mm, 0x08, 2.0);
    put_f64(mm, 0x20, 0.1);
}

static void test_arith(void)
{
    Code c = {0};
    FLD_M64(&c, 0x00);
    FLD_M64(&c, 0x08);
    reg_op(&c, 0xDE, 0xC9); /* FMULP ST(1),ST */
    FSTP_M64(&c, 0x30);
    FLD_M64(&c, 0x08);
    FLD_M64(&c, 0x00);
    reg_op(&c, 0xDE, 0xE9); /* FSUBP ST(1),ST: ST1 = ST1 - ST0 = 2 - pi */
    FSTP_M64(&c, 0x38);
    FLD_M64(&c, 0x08);
    FLD_M64(&c, 0x00);
    reg_op(&c, 0xDE, 0xF9); /* FDIVP ST(1),ST: 2 / pi */
    FSTP_M64(&c, 0x40);
    FLD_M64(&c, 0x08);
    FLD_M64(&c, 0x00);
    reg_op(&c, 0xD8, 0xF1); /* FDIV ST,ST(1): pi / 2 */
    FSTP_M64(&c, 0x48);
    reg_op(&c, 0xDD, 0xD8); /* FSTP ST(0): drop */
    FLD_M64(&c, 0x20);
    FADD_M64(&c, 0x20);
    FADD_M64(&c, 0x20);
    FSTP_M64(&c, 0x50); /* 0.1+0.1+0.1 in extended, then rounded once */
    run_with(&c, setup_pi);
    CHECK(get_f64(0x30) == 2 * 3.141592653589793, "pi*2 = %.17g", get_f64(0x30));
    CHECK(get_f64(0x38) == 2.0 - 3.141592653589793, "2-pi = %.17g", get_f64(0x38));
    CHECK(get_f64(0x40) == 2.0 / 3.141592653589793, "2/pi = %.17g", get_f64(0x40));
    CHECK(get_f64(0x48) == 3.141592653589793 / 2.0, "pi/2 = %.17g", get_f64(0x48));
    long double sum = (long double)0.1 + (long double)0.1 + (long double)0.1;
    CHECK(get_f64(0x50) == (double)sum, "0.1*3 = %.17g", get_f64(0x50));
    CHECK((m->cpu.fpu.sw >> 11 & 7) == 0 && m->cpu.fpu.empty == 0xFF, "stack balanced (sw %04X)", m->cpu.fpu.sw);
    done();
}

static void setup_ints(Machine *mm)
{
    int16_t a = -5;
    put_bytes(mm, 0x00, &a, 2);
    double v[] = {2.5, -2.7, 3.5, 1e10};
    put_bytes(mm, 0x08, v, sizeof v);
    uint16_t cws[] = {0x077F, 0x0F7F, 0x0B7F}; /* RC down, chop, up */
    put_bytes(mm, 0x40, cws, sizeof cws);
    int64_t big = -1234567890123456789LL;
    put_bytes(mm, 0x48, &big, 8);
}

static void test_integers_and_rounding(void)
{
    Code c = {0};
    FILD_M16(&c, 0x00);
    FISTP_M32(&c, 0x60);
    FLD_M64(&c, 0x08);
    FISTP_M32(&c, 0x64); /* 2.5 nearest-even: 2 */
    FLD_M64(&c, 0x18);
    FISTP_M32(&c, 0x68); /* 3.5: 4 */
    FLDCW(&c, 0x40);
    FLD_M64(&c, 0x10);
    FISTP_M32(&c, 0x6C); /* -2.7 down: -3 */
    FLDCW(&c, 0x42);
    FLD_M64(&c, 0x10);
    FISTP_M32(&c, 0x70); /* chop: -2 */
    FLDCW(&c, 0x44);
    FLD_M64(&c, 0x10);
    FISTP_M32(&c, 0x74); /* up: -2 */
    FLD_M64(&c, 0x20);
    FISTP_M16(&c, 0x78); /* 1e10 does not fit: integer indefinite, IE */
    FILD_M64(&c, 0x48);
    FISTP_M64(&c, 0x80);
    FSTSW_AX(&c);
    run_with(&c, setup_ints);
    CHECK(get_i32(0x60) == -5, "FILD/FISTP -5: %d", get_i32(0x60));
    CHECK(get_i32(0x64) == 2, "2.5 -> %d", get_i32(0x64));
    CHECK(get_i32(0x68) == 4, "3.5 -> %d", get_i32(0x68));
    CHECK(get_i32(0x6C) == -3, "RC down -2.7 -> %d", get_i32(0x6C));
    CHECK(get_i32(0x70) == -2, "RC chop -2.7 -> %d", get_i32(0x70));
    CHECK(get_i32(0x74) == -2, "RC up -2.7 -> %d", get_i32(0x74));
    CHECK(get_u16(0x78) == 0x8000, "overflowing FISTP m16 %04X", get_u16(0x78));
    int64_t big;
    memcpy(&big, data(0x80), 8);
    CHECK(big == -1234567890123456789LL, "FILD/FISTP m64 %lld", (long long)big);
    CHECK(m->cpu.r[R_AX].w & FPU_IE, "IE after overflowing FISTP (sw %04X)", m->cpu.r[R_AX].w);
    done();
}

static void setup_ext(Machine *mm)
{
    /* 1/3 in extended precision: 3FFD AAAAAAAAAAAAAAAB */
    static const uint8_t third[10] = {0xAB, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xFD, 0x3F};
    put_bytes(mm, 0x00, third, 10);
    static const uint8_t bcd[10] = {0x89, 0x67, 0x45, 0x23, 0x01, 0, 0, 0, 0, 0x80}; /* -123456789 */
    put_bytes(mm, 0x10, bcd, 10);
    put_f64(mm, 0x20, 1.0);
    put_f64(mm, 0x28, 3.0);
    uint16_t cw24 = 0x007F; /* PC = 24 bits */
    put_bytes(mm, 0x30, &cw24, 2);
}

static void test_extended_and_bcd(void)
{
    Code c = {0};
    FLD_M80(&c, 0x00);
    FSTP_M80(&c, 0x40);
    FBLD(&c, 0x10);
    FBSTP(&c, 0x50);
    FLD_M64(&c, 0x20);
    FDIV_M64(&c, 0x28);
    FSTP_M80(&c, 0x60); /* 1/3 computed: must equal the exact extended pattern */
    FLDCW(&c, 0x30);
    FLD_M64(&c, 0x20);
    FDIV_M64(&c, 0x28);
    FSTP_M64(&c, 0x70); /* PC=24: the float value */
    run_with(&c, setup_ext);
    CHECK(!memcmp(data(0x40), data(0x00), 10), "m80 round trip");
    CHECK(!memcmp(data(0x50), data(0x10), 10), "BCD round trip");
    CHECK(!memcmp(data(0x60), data(0x00), 10), "1/3 in extended precision");
    CHECK(get_f64(0x70) == (double)(1.0f / 3.0f), "PC=24 1/3 = %.17g", get_f64(0x70));
    done();

    uint8_t b[10];
    fpu_to_ext80(1.0L, b);
    static const uint8_t one[10] = {0, 0, 0, 0, 0, 0, 0, 0x80, 0xFF, 0x3F};
    CHECK(!memcmp(b, one, 10), "1.0 as extended");
    static const uint8_t denorm[10] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    long double d = fpu_from_ext80(denorm);
    fpu_to_ext80(d, b);
    CHECK(d > 0 && !memcmp(b, denorm, 10), "smallest denormal round trip");
}

static void setup_cmp(Machine *mm)
{
    put_f64(mm, 0x00, 1.0);
    put_f64(mm, 0x08, 2.0);
}

static void test_compare_and_examine(void)
{
    Code c = {0};
    FLD_M64(&c, 0x00);
    FCOMP_M64(&c, 0x08); /* 1 < 2: C0 */
    FSTSW_AX(&c);
    emit(&c, 0x89), emit(&c, 0x06), emit(&c, 0x20), emit(&c, 0x00); /* mov [20h],ax */
    FLD_M64(&c, 0x08);
    FCOMP_M64(&c, 0x08); /* equal: C3 */
    FSTSW_M(&c, 0x22);
    D9(&c, 0xE5); /* FXAM on empty: C3 C0 */
    FSTSW_M(&c, 0x24);
    D9(&c, 0xEE); /* FLDZ */
    D9(&c, 0xE5); /* zero: C3 */
    FSTSW_M(&c, 0x26);
    run_with(&c, setup_cmp);
    uint16_t cc = FPU_C0 | FPU_C1 | FPU_C2 | FPU_C3;
    CHECK((get_u16(0x20) & cc) == FPU_C0, "1 vs 2 %04X", get_u16(0x20));
    CHECK((get_u16(0x22) & cc) == FPU_C3, "2 vs 2 %04X", get_u16(0x22));
    CHECK((get_u16(0x24) & cc & ~FPU_C1) == (FPU_C3 | FPU_C0), "FXAM empty %04X", get_u16(0x24));
    CHECK((get_u16(0x26) & cc) == FPU_C3, "FXAM +0 %04X", get_u16(0x26));
    done();
}

static void setup_trans(Machine *mm)
{
    put_f64(mm, 0x00, 2.0);
    put_f64(mm, 0x08, 8.0);
    put_f64(mm, 0x10, 7.0);
    put_f64(mm, 0x18, 3.0);
    put_f64(mm, 0x20, 0.5);
}

static void test_transcendental(void)
{
    Code c = {0};
    FLD_M64(&c, 0x00);
    D9(&c, 0xFA); /* FSQRT */
    FSTP_M64(&c, 0x40);
    D9(&c, 0xE8); /* FLD1 */
    D9(&c, 0xE8);
    D9(&c, 0xF3); /* FPATAN: atan2(1, 1) */
    FSTP_M64(&c, 0x48);
    D9(&c, 0xE8);
    FLD_M64(&c, 0x08);
    D9(&c, 0xF1); /* FYL2X: 1 * log2(8) */
    FSTP_M64(&c, 0x50);
    FLD_M64(&c, 0x18); /* 3 */
    FLD_M64(&c, 0x10); /* 7 */
    D9(&c, 0xF8); /* FPREM: 7 mod 3 = 1, quotient 2 */
    FSTSW_M(&c, 0x58);
    FSTP_M64(&c, 0x60);
    reg_op(&c, 0xDD, 0xD8);
    FLD_M64(&c, 0x18); /* 3 */
    FLD_M64(&c, 0x20); /* 0.5 */
    D9(&c, 0xFD); /* FSCALE: 0.5 * 2^3 */
    FSTP_M64(&c, 0x68);
    reg_op(&c, 0xDD, 0xD8);
    FLD_M64(&c, 0x20);
    D9(&c, 0xF0); /* F2XM1(0.5) */
    FSTP_M64(&c, 0x70);
    FLD_M64(&c, 0x08);
    D9(&c, 0xF4); /* FXTRACT 8: exponent 3, significand 1 */
    FSTP_M64(&c, 0x78);
    FSTP_M64(&c, 0x80);
    D9(&c, 0xEB); /* FLDPI */
    D9(&c, 0xFB); /* FSINCOS: ST0 = cos, ST1 = sin */
    FSTP_M64(&c, 0x88);
    FSTP_M64(&c, 0x90);
    D9(&c, 0xEB);
    D9(&c, 0xF2); /* FPTAN pushes 1 */
    FSTP_M64(&c, 0x98);
    FSTP_M64(&c, 0xA0);
    run_with(&c, setup_trans);
    CHECK(get_f64(0x40) == sqrt(2.0), "sqrt 2 %.17g", get_f64(0x40));
    CHECK(fabs(get_f64(0x48) - M_PI / 4) < 1e-16, "atan2(1,1) %.17g", get_f64(0x48));
    CHECK(get_f64(0x50) == 3.0, "log2 8 %.17g", get_f64(0x50));
    CHECK(get_f64(0x60) == 1.0, "FPREM 7,3 %.17g", get_f64(0x60));
    uint16_t sw = get_u16(0x58);
    CHECK(!(sw & FPU_C2) && (sw & FPU_C3) && !(sw & FPU_C1) && !(sw & FPU_C0), "FPREM quotient bits %04X", sw);
    CHECK(get_f64(0x68) == 4.0, "FSCALE %.17g", get_f64(0x68));
    CHECK(get_f64(0x70) == (double)(sqrtl(2.0L) - 1), "F2XM1 %.17g", get_f64(0x70));
    CHECK(get_f64(0x78) == 1.0 && get_f64(0x80) == 3.0, "FXTRACT %g %g", get_f64(0x78), get_f64(0x80));
    CHECK(get_f64(0x88) == -1.0 && fabs(get_f64(0x90)) < 1e-18, "FSINCOS pi %g %g", get_f64(0x88), get_f64(0x90));
    CHECK(get_f64(0x98) == 1.0 && fabs(get_f64(0xA0)) < 1e-18, "FPTAN pi %g %g", get_f64(0x98), get_f64(0xA0));
    CHECK(m->cpu.fpu.empty == 0xFF, "stack balanced (%02X)", m->cpu.fpu.empty);
    done();
}

static void setup_exc(Machine *mm)
{
    put_f64(mm, 0x00, 1.0);
    put_f64(mm, 0x08, 0.0);
    uint16_t cw = 0x037B; /* ZE unmasked */
    put_bytes(mm, 0x10, &cw, 2);
}

static void test_exceptions(void)
{
    Code c = {0};
    reg_op(&c, 0xD8, 0xC1); /* FADD ST,ST(1) on an empty stack: underflow */
    FSTSW_M(&c, 0x20);
    FST_M32(&c, 0x22);      /* the masked response left the indefinite */
    FNINIT(&c);
    for (int i = 0; i < 9; i++)
        D9(&c, 0xE8); /* nine FLD1: the ninth overflows */
    FSTSW_M(&c, 0x28);
    FNINIT(&c);
    FLD_M64(&c, 0x00);
    FDIV_M64(&c, 0x08); /* 1/0 masked: +inf, ZE */
    FSTP_M64(&c, 0x30);
    FSTSW_M(&c, 0x38);
    FNINIT(&c);
    FLDCW(&c, 0x10);
    FLD_M64(&c, 0x00);
    FDIV_M64(&c, 0x08); /* unmasked: ES and B */
    FSTSW_M(&c, 0x3A);
    FNCLEX(&c);
    FSTSW_M(&c, 0x3C);
    run_with(&c, setup_exc);
    uint16_t sw = get_u16(0x20);
    CHECK((sw & (FPU_IE | FPU_SF | FPU_C1)) == (FPU_IE | FPU_SF), "underflow sw %04X", sw);
    CHECK(isnan(get_f32(0x22)) && signbit(get_f32(0x22)), "indefinite %g", get_f32(0x22));
    sw = get_u16(0x28);
    CHECK((sw & (FPU_IE | FPU_SF | FPU_C1)) == (FPU_IE | FPU_SF | FPU_C1), "overflow sw %04X", sw);
    CHECK(isinf(get_f64(0x30)) && get_f64(0x30) > 0, "1/0 = %g", get_f64(0x30));
    CHECK((get_u16(0x38) & (FPU_ZE | FPU_ES)) == FPU_ZE, "masked ZE sw %04X", get_u16(0x38));
    CHECK((get_u16(0x3A) & (FPU_ZE | FPU_ES | FPU_B)) == (FPU_ZE | FPU_ES | FPU_B), "unmasked ZE sw %04X",
          get_u16(0x3A));
    CHECK(!(get_u16(0x3C) & (FPU_ZE | FPU_ES | FPU_B)), "FNCLEX sw %04X", get_u16(0x3C));
    done();
}

static void setup_env(Machine *mm) { put_f64(mm, 0x00, 42.0); }

static void test_environment(void)
{
    Code c = {0};
    D9(&c, 0xE8);       /* FLD1 at physical register 7 */
    FSTENV(&c, 0x20);   /* 14-byte real-mode layout */
    FLD_M64(&c, 0x00);  /* ST0 = 42, ST1 = 1 */
    FSAVE(&c, 0x40);    /* 94 bytes, then FNINIT */
    FSTSW_M(&c, 0x10);
    FRSTOR(&c, 0x40);
    FSTP_M64(&c, 0xA0);
    FSTP_M64(&c, 0xA8);
    run_with(&c, setup_env);
    CHECK(get_u16(0x20) == 0x037F, "env CW %04X", get_u16(0x20));
    CHECK((get_u16(0x22) >> 11 & 7) == 7, "env SW TOP %04X", get_u16(0x22));
    CHECK(get_u16(0x24) == 0x3FFF, "env TW %04X", get_u16(0x24));
    uint32_t ip = get_u16(0x26) | (uint32_t)(get_u16(0x28) >> 12) << 16;
    CHECK(ip == (uint32_t)CODE_SEG * 16, "env IP %05X", ip);
    CHECK((get_u16(0x28) & 0x7FF) == 0x1E8, "env opcode %03X", get_u16(0x28) & 0x7FF);
    CHECK((get_u16(0x10) >> 11 & 7) == 0, "FSAVE reinitialises (sw %04X)", get_u16(0x10));
    CHECK(get_f64(0xA0) == 42.0 && get_f64(0xA8) == 1.0, "FRSTOR %g %g", get_f64(0xA0), get_f64(0xA8));
    done();
}

static void setup_imul(Machine *mm)
{
    put_f64(mm, 0x00, 1.5);
    int16_t k = -4;
    put_bytes(mm, 0x08, &k, 2);
}

static void test_fwait_prefix_and_int_ops(void)
{
    Code c = {0};
    emit(&c, 0x9B); /* FWAIT */
    FLD_M64(&c, 0x00);
    emit(&c, 0x9B);
    FIMUL_M16(&c, 0x08);
    FSTP_M64(&c, 0x10);
    run_with(&c, setup_imul);
    CHECK(get_f64(0x10) == -6.0, "1.5 * -4 = %g", get_f64(0x10));
    done();
}

int main(void)
{
    test_detection();
    test_arith();
    test_integers_and_rounding();
    test_extended_and_bcd();
    test_compare_and_examine();
    test_transcendental();
    test_exceptions();
    test_environment();
    test_fwait_prefix_and_int_ops();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("fpu: all tests passed");
    return 0;
}
