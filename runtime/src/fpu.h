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

/* 80-bit extended <-> host conversions (exposed for tests). */
long double fpu_from_ext80(const uint8_t b[10]);
void fpu_to_ext80(long double v, uint8_t b[10]);

#endif
