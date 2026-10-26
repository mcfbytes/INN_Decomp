/* x87 floating-point unit (387 instruction set) for the interpreter. docs/runtime.md, "FPU". */
#ifndef FPU_H
#define FPU_H

#include <stdbool.h>
#include <stdint.h>

/* Registers are host long doubles rounded to the 64-bit x87 significand; 80-bit memory images are
 * packed bit by bit, so FLD/FSTP m80 round-trip on every host. */
typedef struct Fpu {
    long double st[8];  /* physical registers; ST(i) is st[(top + i) & 7] */
    uint8_t empty;      /* bit n: physical register n is empty (tag 11) */
    uint16_t cw, sw;    /* sw keeps TOP in bits 11-13 */
    uint16_t fop;       /* last non-control opcode, 11 bits */
    uint16_t fcs, fds;  /* last instruction / operand segment */
    uint32_t fip, fdp;  /* their offsets */
} Fpu;

/* Status word bits. */
enum {
    FPU_IE = 0x0001, FPU_DE = 0x0002, FPU_ZE = 0x0004, FPU_OE = 0x0008, FPU_UE = 0x0010,
    FPU_PE = 0x0020, FPU_SF = 0x0040, FPU_ES = 0x0080, FPU_C0 = 0x0100, FPU_C1 = 0x0200,
    FPU_C2 = 0x0400, FPU_C3 = 0x4000, FPU_B = 0x8000,
};

typedef struct Machine Machine;

/* One escape instruction (D8-DF), decoded by the CPU. */
typedef struct FpuInsn {
    uint8_t opcode, modrm;
    bool is_reg;       /* mod == 3 */
    uint32_t addr;     /* linear operand address when !is_reg */
    uint16_t seg, off; /* operand segment and offset, for FSTENV */
    bool op32;         /* operand-size prefix: 32-bit FSTENV/FSAVE layout */
    uint16_t cs;
    uint32_t ip;       /* instruction start, including prefixes */
} FpuInsn;

void fpu_init(Fpu *f); /* FNINIT */
void fpu_exec(Machine *m, const FpuInsn *in);
void fpu_wait(Machine *m); /* FWAIT: signals a pending unmasked exception */

/* 80-bit extended <-> host conversions (exposed for tests and verification). */
long double fpu_from_ext80(const uint8_t b[10]);
void fpu_to_ext80(long double v, uint8_t b[10]);

/* Shadow verification's comparison (docs/lifting.md, "Floating point"): TOP, empty registers, non-empty ones
 * as 80-bit images, CW, SW exception/SF/ES bits, C0-C3 unless cc_may_differ; why names the difference. */
bool fpu_state_matches(const Fpu *lifted, const Fpu *original, bool cc_may_differ, char *why, unsigned cap);

/* ---- x87 for lifted C: each call runs the interpreter's code for one instruction after an FWAIT (bit-exact
 * results, C0-C3, exceptions, stack); nothing without fpu_present. Operands are seg:off; ST(i) from TOP. */
void fpu_mem(Machine *m, uint8_t opcode, int reg, uint16_t seg, uint16_t off); /* <opcode> /reg m */
void fpu_reg(Machine *m, uint8_t opcode, uint8_t modrm);                        /* <opcode> modrm, mod 3 */
void fpu_fwait(Machine *m);

/* ST(i) as a host value (rounded to the x87 significand already), whether it is empty, and TOP. */
long double fpu_st(const Machine *m, int i);
bool fpu_st_empty(const Machine *m, int i);
int fpu_top(const Machine *m);
uint16_t fpu_sw(const Machine *m);
uint16_t fpu_cw(const Machine *m);
/* Pushes a host value as FLD would push an 80-bit operand holding it (rounded to 64 bits; a full stack
 * is a stack fault). fpu_pop returns ST(0) and pops it as FSTP ST(0). */
void fpu_push(Machine *m, long double v);
long double fpu_pop(Machine *m);

#define FPU_MEM_(name, opcode, reg) \
    static inline void fpu_##name(Machine *m, uint16_t seg, uint16_t off) { fpu_mem(m, opcode, reg, seg, off); }
#define FPU_REG_(name, opcode, modrm) static inline void fpu_##name(Machine *m) { fpu_reg(m, opcode, modrm); }
#define FPU_STI_(name, opcode, modrm) \
    static inline void fpu_##name(Machine *m, int i) { fpu_reg(m, opcode, (uint8_t)((modrm) + (i & 7))); }

/* Loads: reals (FLD m32/m64/m80), integers (FILD m16/m32/m64), packed BCD (FBLD). */
FPU_MEM_(fld_m32, 0xD9, 0) FPU_MEM_(fld_m64, 0xDD, 0) FPU_MEM_(fld_m80, 0xDB, 5)
FPU_MEM_(fild_m16, 0xDF, 0) FPU_MEM_(fild_m32, 0xDB, 0) FPU_MEM_(fild_m64, 0xDF, 5) FPU_MEM_(fbld, 0xDF, 4)
/* Stores, rounded by CW (precision of the format, rounding control for integers and narrower reals). */
FPU_MEM_(fst_m32, 0xD9, 2) FPU_MEM_(fstp_m32, 0xD9, 3) FPU_MEM_(fst_m64, 0xDD, 2) FPU_MEM_(fstp_m64, 0xDD, 3)
FPU_MEM_(fstp_m80, 0xDB, 7)
FPU_MEM_(fist_m16, 0xDF, 2) FPU_MEM_(fistp_m16, 0xDF, 3) FPU_MEM_(fist_m32, 0xDB, 2) FPU_MEM_(fistp_m32, 0xDB, 3)
FPU_MEM_(fistp_m64, 0xDF, 7) FPU_MEM_(fbstp, 0xDF, 6)
/* ST(0) op= memory: real m32 (D8), real m64 (DC), integer m16 (DE), integer m32 (DA). sub: ST(0) - m,
 * subr: m - ST(0); com/comp set C0-C3 (comp pops). Precision control applies as on the 387. */
#define FPU_ARITH_MEM_(sfx, opcode)                                                                         \
    FPU_MEM_(fadd_##sfx, opcode, 0) FPU_MEM_(fmul_##sfx, opcode, 1) FPU_MEM_(fcom_##sfx, opcode, 2)           \
    FPU_MEM_(fcomp_##sfx, opcode, 3) FPU_MEM_(fsub_##sfx, opcode, 4) FPU_MEM_(fsubr_##sfx, opcode, 5)         \
    FPU_MEM_(fdiv_##sfx, opcode, 6) FPU_MEM_(fdivr_##sfx, opcode, 7)
FPU_ARITH_MEM_(m32, 0xD8)
FPU_ARITH_MEM_(m64, 0xDC)
FPU_ARITH_MEM_(i16, 0xDE)
FPU_ARITH_MEM_(i32, 0xDA)
/* Control and status in memory: FLDCW, FSTCW, FSTSW, FLDENV/FSTENV, FRSTOR/FSAVE (16-bit layouts). */
FPU_MEM_(fldcw_m, 0xD9, 5) FPU_MEM_(fstcw_m, 0xD9, 7) FPU_MEM_(fstsw_m, 0xDD, 7)
FPU_MEM_(fldenv, 0xD9, 4) FPU_MEM_(fstenv, 0xD9, 6) FPU_MEM_(frstor, 0xDD, 4) FPU_MEM_(fsave, 0xDD, 6)

/* Registers: FLD ST(i), FXCH, FST/FSTP ST(i), FFREE; ST(0) op= ST(i) (D8); ST(i) op= ST(0) (DC) and the
 * popping forms (DE), Intel's operand order: fsubp(i) is ST(i) = ST(i) - ST(0), fsubrp(i) ST(0) - ST(i). */
FPU_STI_(fld_st, 0xD9, 0xC0) FPU_STI_(fxch, 0xD9, 0xC8) FPU_STI_(fst_st, 0xDD, 0xD0) FPU_STI_(fstp_st, 0xDD, 0xD8)
FPU_STI_(ffree, 0xDD, 0xC0)
FPU_STI_(fadd_st, 0xD8, 0xC0) FPU_STI_(fmul_st, 0xD8, 0xC8) FPU_STI_(fcom_st, 0xD8, 0xD0) FPU_STI_(fcomp_st, 0xD8, 0xD8)
FPU_STI_(fsub_st, 0xD8, 0xE0) FPU_STI_(fsubr_st, 0xD8, 0xE8) FPU_STI_(fdiv_st, 0xD8, 0xF0)
FPU_STI_(fdivr_st, 0xD8, 0xF8)
FPU_STI_(fadd_to, 0xDC, 0xC0) FPU_STI_(fmul_to, 0xDC, 0xC8) FPU_STI_(fsubr_to, 0xDC, 0xE0) FPU_STI_(fsub_to, 0xDC, 0xE8)
FPU_STI_(fdivr_to, 0xDC, 0xF0) FPU_STI_(fdiv_to, 0xDC, 0xF8)
FPU_STI_(faddp, 0xDE, 0xC0) FPU_STI_(fmulp, 0xDE, 0xC8) FPU_STI_(fsubrp, 0xDE, 0xE0) FPU_STI_(fsubp, 0xDE, 0xE8)
FPU_STI_(fdivrp, 0xDE, 0xF0) FPU_STI_(fdivp, 0xDE, 0xF8)
FPU_STI_(fucom, 0xDD, 0xE0) FPU_STI_(fucomp, 0xDD, 0xE8)
FPU_REG_(fcompp, 0xDE, 0xD9) FPU_REG_(fucompp, 0xDA, 0xE9)
/* D9 group: sign, tests, constants, transcendentals (libm, as fpu.c computes them), rounding, stack. */
FPU_REG_(fchs, 0xD9, 0xE0) FPU_REG_(fabs, 0xD9, 0xE1) FPU_REG_(ftst, 0xD9, 0xE4) FPU_REG_(fxam, 0xD9, 0xE5)
FPU_REG_(fld1, 0xD9, 0xE8) FPU_REG_(fldl2t, 0xD9, 0xE9) FPU_REG_(fldl2e, 0xD9, 0xEA) FPU_REG_(fldpi, 0xD9, 0xEB)
FPU_REG_(fldlg2, 0xD9, 0xEC) FPU_REG_(fldln2, 0xD9, 0xED) FPU_REG_(fldz, 0xD9, 0xEE)
FPU_REG_(f2xm1, 0xD9, 0xF0) FPU_REG_(fyl2x, 0xD9, 0xF1) FPU_REG_(fptan, 0xD9, 0xF2) FPU_REG_(fpatan, 0xD9, 0xF3)
FPU_REG_(fxtract, 0xD9, 0xF4) FPU_REG_(fprem1, 0xD9, 0xF5) FPU_REG_(fdecstp, 0xD9, 0xF6) FPU_REG_(fincstp, 0xD9, 0xF7)
FPU_REG_(fprem, 0xD9, 0xF8) FPU_REG_(fyl2xp1, 0xD9, 0xF9) FPU_REG_(fsqrt, 0xD9, 0xFA) FPU_REG_(fsincos, 0xD9, 0xFB)
FPU_REG_(frndint, 0xD9, 0xFC) FPU_REG_(fscale, 0xD9, 0xFD) FPU_REG_(fsin, 0xD9, 0xFE) FPU_REG_(fcos, 0xD9, 0xFF)
FPU_REG_(fninit, 0xDB, 0xE3) FPU_REG_(fnclex, 0xDB, 0xE2)
/* FSTSW AX: AX = SW. */
FPU_REG_(fstsw_ax, 0xDF, 0xE0)

#undef FPU_MEM_
#undef FPU_REG_
#undef FPU_STI_
#undef FPU_ARITH_MEM_

/* Condition codes as the last compare left them (FCOM: C0 ST(0) < operand, C3 equal, all three
 * unordered); SAHF maps C0 to CF, C2 to PF and C3 to ZF. */
static inline bool fpu_c0(const Machine *m) { return fpu_sw(m) & FPU_C0; }
static inline bool fpu_c2(const Machine *m) { return fpu_sw(m) & FPU_C2; }
static inline bool fpu_c3(const Machine *m) { return fpu_sw(m) & FPU_C3; }

#endif
