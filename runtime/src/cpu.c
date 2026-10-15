/* 386 real-mode interpreter, the reference semantics for every execution tier. Escapes go to the 387
 * (fpu.c); with Machine.fpu_present off they only decode their operand, so probes find no FPU. */
#include <stdio.h>
#include <string.h>

#include "lift.h"
#include "machine.h"

typedef struct Insn {
    uint32_t start_eip;
    int seg; /* override, or S_NONE */
    bool op32, addr32, lock;
    uint8_t rep; /* 0, 0xF2 or 0xF3 */
} Insn;

typedef struct Operand {
    bool is_reg;
    int index;      /* register number when is_reg */
    uint32_t addr;  /* linear address when memory */
    uint32_t off;   /* effective offset, for LEA */
    int reg_field;  /* ModRM reg bits */
} Operand;

static const uint8_t parity_table[256] = {
#define P2(n) n, n ^ 1, n ^ 1, n
#define P4(n) P2(n), P2(n ^ 1), P2(n ^ 1), P2(n)
#define P6(n) P4(n), P4(n ^ 1), P4(n ^ 1), P4(n)
    P6(1), P6(0), P6(0), P6(1)
#undef P2
#undef P4
#undef P6
};

enum { CR0_REAL_MODE = 0x7FFEFFF0 }; /* as the 386EX reports it after reset: PE and PG clear */

static inline uint32_t size_mask(int size) { return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFFu; }
static inline uint32_t sign_bit(int size) { return 1u << (size * 8 - 1); }

/* ---- registers ---- */

void cpu_set_seg(Cpu *c, int sreg, uint16_t value)
{
    c->seg[sreg] = value;
    c->seg_base[sreg] = (uint32_t)value << 4;
}

static uint32_t get_reg(Machine *m, int size, int index)
{
    Cpu *c = &m->cpu;
    if (size == 1)
        return index < 4 ? c->r[index].b.l : c->r[index - 4].b.h;
    if (size == 2)
        return c->r[index].w;
    return c->r[index].e;
}

static void set_reg(Machine *m, int size, int index, uint32_t v)
{
    Cpu *c = &m->cpu;
    if (size == 1) {
        if (index < 4)
            c->r[index].b.l = (uint8_t)v;
        else
            c->r[index - 4].b.h = (uint8_t)v;
    } else if (size == 2) {
        c->r[index].w = (uint16_t)v;
    } else {
        c->r[index].e = v;
    }
}

/* ---- fetch ---- */

static inline uint8_t fetch8(Machine *m)
{
    Cpu *c = &m->cpu;
    uint8_t v = mem_rd8(m, c->seg_base[S_CS] + (c->eip & 0xFFFF));
    c->eip = (c->eip + 1) & 0xFFFF;
    return v;
}

static inline uint16_t fetch16(Machine *m)
{
    uint16_t lo = fetch8(m);
    return (uint16_t)(lo | fetch8(m) << 8);
}

static inline uint32_t fetch32(Machine *m)
{
    uint32_t lo = fetch16(m);
    return lo | (uint32_t)fetch16(m) << 16;
}

static uint32_t fetch_imm(Machine *m, int size)
{
    return size == 1 ? fetch8(m) : size == 2 ? fetch16(m) : fetch32(m);
}

/* ---- stack ---- */

void cpu_push16(Machine *m, uint16_t v)
{
    Cpu *c = &m->cpu;
    c->r[R_SP].w -= 2;
    mem_wr16(m, c->seg_base[S_SS] + c->r[R_SP].w, v);
}

uint16_t cpu_pop16(Machine *m)
{
    Cpu *c = &m->cpu;
    uint16_t v = mem_rd16(m, c->seg_base[S_SS] + c->r[R_SP].w);
    c->r[R_SP].w += 2;
    return v;
}

static void push32(Machine *m, uint32_t v)
{
    Cpu *c = &m->cpu;
    c->r[R_SP].w -= 4;
    mem_wr32(m, c->seg_base[S_SS] + c->r[R_SP].w, v);
}

static uint32_t pop32(Machine *m)
{
    Cpu *c = &m->cpu;
    uint32_t v = mem_rd32(m, c->seg_base[S_SS] + c->r[R_SP].w);
    c->r[R_SP].w += 4;
    return v;
}

static void push_op(Machine *m, const Insn *in, uint32_t v)
{
    if (in->op32)
        push32(m, v);
    else
        cpu_push16(m, (uint16_t)v);
}

static uint32_t pop_op(Machine *m, const Insn *in)
{
    return in->op32 ? pop32(m) : cpu_pop16(m);
}

/* ---- effective addresses ---- */

static uint32_t seg_base_for(Machine *m, const Insn *in, int default_seg)
{
    return m->cpu.seg_base[in->seg != S_NONE ? in->seg : default_seg];
}

static uint32_t decode_sib(Machine *m, int mod, int *default_seg)
{
    Cpu *c = &m->cpu;
    uint8_t sib = fetch8(m);
    int scale = sib >> 6, index = sib >> 3 & 7, base = sib & 7;
    uint32_t addr = 0;
    if (base == R_BP && mod == 0) {
        addr = fetch32(m);
    } else {
        addr = c->r[base].e;
        if (base == R_SP || base == R_BP)
            *default_seg = S_SS;
    }
    /* With no index the 386 scales the base instead (an undefined encoding; hardware-verified). */
    if (index != R_SP)
        addr += c->r[index].e << scale;
    else if (!(base == R_BP && mod == 0))
        addr <<= scale;
    return addr;
}

static Operand decode_modrm(Machine *m, const Insn *in)
{
    Cpu *c = &m->cpu;
    uint8_t modrm = fetch8(m);
    Operand op = {.reg_field = modrm >> 3 & 7};
    int mod = modrm >> 6, rm = modrm & 7;
    if (mod == 3) {
        op.is_reg = true;
        op.index = rm;
        return op;
    }
    int default_seg = S_DS;
    uint32_t off;
    if (!in->addr32) {
        static const int base_reg[8] = {R_BX, R_BX, R_BP, R_BP, R_SI, R_DI, R_BP, R_BX};
        static const int index_reg[8] = {R_SI, R_DI, R_SI, R_DI, -1, -1, -1, -1};
        if (mod == 0 && rm == 6) {
            off = fetch16(m);
        } else {
            off = c->r[base_reg[rm]].w;
            if (index_reg[rm] >= 0)
                off += c->r[index_reg[rm]].w;
            if (rm == 2 || rm == 3 || rm == 6)
                default_seg = S_SS;
        }
        if (mod == 1)
            off += (uint32_t)(int8_t)fetch8(m);
        else if (mod == 2)
            off += fetch16(m);
        off &= 0xFFFF;
    } else {
        if (rm == 4) {
            off = decode_sib(m, mod, &default_seg);
        } else if (mod == 0 && rm == 5) {
            off = fetch32(m);
        } else {
            off = c->r[rm].e;
            if (rm == R_BP)
                default_seg = S_SS;
        }
        if (mod == 1)
            off += (uint32_t)(int8_t)fetch8(m);
        else if (mod == 2)
            off += fetch32(m);
    }
    op.off = off;
    op.addr = seg_base_for(m, in, default_seg) + off;
    return op;
}

/* Linear address delta bytes into a memory operand; 16-bit offsets wrap within the segment. */
static uint32_t operand_part(const Insn *in, const Operand *op, uint32_t delta)
{
    uint32_t off = op->off + delta;
    return op->addr - op->off + (in->addr32 ? off : off & 0xFFFF);
}

static uint32_t read_mem(Machine *m, int size, uint32_t addr)
{
    return size == 1 ? mem_rd8(m, addr) : size == 2 ? mem_rd16(m, addr) : mem_rd32(m, addr);
}

static void write_mem(Machine *m, int size, uint32_t addr, uint32_t v)
{
    if (size == 1)
        mem_wr8(m, addr, (uint8_t)v);
    else if (size == 2)
        mem_wr16(m, addr, (uint16_t)v);
    else
        mem_wr32(m, addr, v);
}

static uint32_t read_rm(Machine *m, int size, const Operand *op)
{
    return op->is_reg ? get_reg(m, size, op->index) : read_mem(m, size, op->addr);
}

static void write_rm(Machine *m, int size, const Operand *op, uint32_t v)
{
    if (op->is_reg)
        set_reg(m, size, op->index, v);
    else
        write_mem(m, size, op->addr, v);
}

/* ---- flags ---- */

static inline void set_flag(Cpu *c, uint32_t flag, bool on)
{
    if (on)
        c->flags |= flag;
    else
        c->flags &= ~flag;
}

static void set_szp(Cpu *c, int size, uint32_t res)
{
    res &= size_mask(size);
    set_flag(c, F_ZF, res == 0);
    set_flag(c, F_SF, (res & sign_bit(size)) != 0);
    set_flag(c, F_PF, parity_table[res & 0xFF]);
}

enum { ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP };

static uint32_t alu(Machine *m, int op, int size, uint32_t a, uint32_t b)
{
    Cpu *c = &m->cpu;
    uint32_t mask = size_mask(size), sb = sign_bit(size);
    uint64_t wide;
    uint32_t res, carry_in;
    a &= mask;
    b &= mask;
    switch (op) {
    case ALU_ADD:
    case ALU_ADC:
        carry_in = op == ALU_ADC && (c->flags & F_CF) ? 1 : 0;
        wide = (uint64_t)a + b + carry_in;
        res = (uint32_t)wide & mask;
        set_flag(c, F_CF, wide > mask);
        set_flag(c, F_OF, ((a ^ res) & (b ^ res) & sb) != 0);
        set_flag(c, F_AF, ((a ^ b ^ res) & 0x10) != 0);
        break;
    case ALU_SUB:
    case ALU_SBB:
    case ALU_CMP:
        carry_in = op == ALU_SBB && (c->flags & F_CF) ? 1 : 0;
        res = (a - b - carry_in) & mask;
        set_flag(c, F_CF, (uint64_t)a < (uint64_t)b + carry_in);
        set_flag(c, F_OF, ((a ^ b) & (a ^ res) & sb) != 0);
        set_flag(c, F_AF, ((a ^ b ^ res) & 0x10) != 0);
        break;
    case ALU_OR: res = a | b; goto logic;
    case ALU_AND: res = a & b; goto logic;
    case ALU_XOR: res = a ^ b;
    logic:
        c->flags &= ~(F_CF | F_OF | F_AF);
        break;
    default:
        fatal("alu: bad op %d", op);
    }
    set_szp(c, size, res);
    return res;
}

static uint32_t inc_dec(Machine *m, int size, uint32_t a, bool dec)
{
    Cpu *c = &m->cpu;
    bool cf = (c->flags & F_CF) != 0;
    uint32_t res = alu(m, dec ? ALU_SUB : ALU_ADD, size, a, 1);
    set_flag(c, F_CF, cf);
    return res;
}

static uint32_t shift_rotate(Machine *m, int op, int size, uint32_t v, unsigned count)
{
    Cpu *c = &m->cpu;
    unsigned bits = (unsigned)size * 8;
    uint32_t mask = size_mask(size), sb = sign_bit(size);
    count &= 0x1F;
    if (count == 0)
        return v & mask;
    v &= mask;
    uint32_t res = v;
    bool cf = (c->flags & F_CF) != 0;
    switch (op) {
    case 0: /* rol */
        for (unsigned i = 0; i < count; i++)
            res = ((res << 1) | (res >> (bits - 1))) & mask;
        cf = res & 1;
        set_flag(c, F_OF, ((res & sb) != 0) != cf);
        break;
    case 1: /* ror */
        for (unsigned i = 0; i < count; i++)
            res = ((res >> 1) | ((res & 1) << (bits - 1))) & mask;
        cf = (res & sb) != 0;
        set_flag(c, F_OF, (((res ^ (res << 1)) & sb) != 0));
        break;
    case 2: /* rcl */
        for (unsigned i = 0; i < count % (bits + 1); i++) {
            bool out = (res & sb) != 0;
            res = ((res << 1) | (cf ? 1 : 0)) & mask;
            cf = out;
        }
        set_flag(c, F_OF, ((res & sb) != 0) != cf);
        break;
    case 3: /* rcr */
        set_flag(c, F_OF, ((res & sb) != 0) != cf);
        for (unsigned i = 0; i < count % (bits + 1); i++) {
            bool out = res & 1;
            res = (res >> 1) | (cf ? sb : 0);
            cf = out;
        }
        break;
    /* Past the operand width only a multiple of the width keeps the edge bit (hardware-verified). */
    case 4: /* shl */
    case 6:
        cf = count <= bits ? (v >> (bits - count)) & 1 : count % bits == 0 && (v & 1);
        res = count < bits ? (v << count) & mask : 0;
        set_flag(c, F_OF, ((res & sb) != 0) != cf);
        set_szp(c, size, res);
        set_flag(c, F_AF, false);
        break;
    case 5: /* shr */
        cf = count <= bits ? (v >> (count - 1)) & 1 : count % bits == 0 && (v & sb);
        res = count < bits ? v >> count : 0;
        set_flag(c, F_OF, (v & sb) != 0);
        set_szp(c, size, res);
        set_flag(c, F_AF, false);
        break;
    case 7: { /* sar */
        int32_t sv = (int32_t)(v << (32 - bits)) >> (32 - bits);
        unsigned n = count < bits ? count : bits - 1;
        cf = count < bits ? (sv >> (count - 1)) & 1 : sv < 0;
        res = (uint32_t)(sv >> n) & mask;
        set_flag(c, F_OF, false);
        set_szp(c, size, res);
        set_flag(c, F_AF, false);
        break;
    }
    }
    set_flag(c, F_CF, cf);
    return res;
}

static bool condition(const Cpu *c, int cc)
{
    uint32_t f = c->flags;
    bool r;
    switch (cc >> 1) {
    case 0: r = f & F_OF; break;
    case 1: r = f & F_CF; break;
    case 2: r = f & F_ZF; break;
    case 3: r = f & (F_CF | F_ZF); break;
    case 4: r = f & F_SF; break;
    case 5: r = f & F_PF; break;
    case 6: r = ((f & F_SF) != 0) != ((f & F_OF) != 0); break;
    default: r = (f & F_ZF) || (((f & F_SF) != 0) != ((f & F_OF) != 0)); break;
    }
    return (cc & 1) ? !r : r;
}

/* Bits 12-14 (IOPL, NT) stay writable so CPU probes see a 386; AC and ID never stick. */
static void write_flags(Cpu *c, uint32_t v, bool op32)
{
    uint32_t writable = 0x7FD5;
    c->flags = (op32 ? (c->flags & ~writable & 0xFFFF0000u) : (c->flags & 0xFFFF0000u)) |
               (v & writable) | 2;
}

/* ---- control transfer ---- */

static void far_jump(Machine *m, uint16_t seg, uint32_t off)
{
    cpu_set_seg(&m->cpu, S_CS, seg);
    m->cpu.eip = off & 0xFFFF;
}

/* The vector is read before the frame is pushed, which matters when the stack overlaps the IVT. */
void cpu_interrupt(Machine *m, uint8_t vector)
{
    Cpu *c = &m->cpu;
    uint16_t off = mem_rd16(m, vector * 4u), seg = mem_rd16(m, vector * 4u + 2);
    cpu_push16(m, (uint16_t)c->flags);
    cpu_push16(m, c->seg[S_CS]);
    cpu_push16(m, (uint16_t)c->eip);
    c->flags &= ~(F_IF | F_TF);
    far_jump(m, seg, off);
    c->halted = false;
    lift_coverage_hit(m, LIFT_COVERAGE_INT);
}

static void raise_fault(Machine *m, const Insn *in, uint8_t vector)
{
    m->cpu.eip = in->start_eip;
    cpu_interrupt(m, vector);
}

static void invalid_opcode(Machine *m, const Insn *in, unsigned opcode)
{
    log_msg("cpu: invalid opcode %02X at %04X:%04X", opcode, m->cpu.seg[S_CS], in->start_eip);
    raise_fault(m, in, 6);
}

/* ---- multiply and divide ---- */

static void mul_group(Machine *m, const Insn *in, int size, int sub, uint32_t src)
{
    Cpu *c = &m->cpu;
    bool overflow;
    if (sub == 4 || sub == 5) {
        bool is_signed = sub == 5;
        if (size == 1) {
            uint16_t res = is_signed ? (uint16_t)((int8_t)c->r[R_AX].b.l * (int8_t)src)
                                     : (uint16_t)(c->r[R_AX].b.l * (uint8_t)src);
            c->r[R_AX].w = res;
            overflow = is_signed ? (int16_t)res != (int8_t)res : (res >> 8) != 0;
        } else if (size == 2) {
            uint32_t res = is_signed ? (uint32_t)((int16_t)c->r[R_AX].w * (int16_t)src)
                                     : (uint32_t)c->r[R_AX].w * (uint16_t)src;
            c->r[R_AX].w = (uint16_t)res;
            c->r[R_DX].w = (uint16_t)(res >> 16);
            overflow = is_signed ? (int32_t)res != (int16_t)res : (res >> 16) != 0;
        } else {
            uint64_t res = is_signed ? (uint64_t)((int64_t)(int32_t)c->r[R_AX].e * (int32_t)src)
                                     : (uint64_t)c->r[R_AX].e * src;
            c->r[R_AX].e = (uint32_t)res;
            c->r[R_DX].e = (uint32_t)(res >> 32);
            overflow = is_signed ? (int64_t)res != (int32_t)res : (res >> 32) != 0;
        }
        set_flag(c, F_CF, overflow);
        set_flag(c, F_OF, overflow);
        return;
    }
    if ((src & size_mask(size)) == 0) {
        raise_fault(m, in, 0);
        return;
    }
    bool is_signed = sub == 7;
    if (size == 1) {
        uint16_t num = c->r[R_AX].w;
        if (is_signed) {
            int q = (int16_t)num / (int8_t)src, r = (int16_t)num % (int8_t)src;
            if (q > 127 || q < -128) { raise_fault(m, in, 0); return; }
            c->r[R_AX].b.l = (uint8_t)q;
            c->r[R_AX].b.h = (uint8_t)r;
        } else {
            unsigned q = num / (uint8_t)src, r = num % (uint8_t)src;
            if (q > 0xFF) { raise_fault(m, in, 0); return; }
            c->r[R_AX].b.l = (uint8_t)q;
            c->r[R_AX].b.h = (uint8_t)r;
        }
    } else if (size == 2) {
        uint32_t num = (uint32_t)c->r[R_DX].w << 16 | c->r[R_AX].w;
        if (is_signed) {
            int32_t q = (int32_t)num / (int16_t)src, r = (int32_t)num % (int16_t)src;
            if (q > 32767 || q < -32768) { raise_fault(m, in, 0); return; }
            c->r[R_AX].w = (uint16_t)q;
            c->r[R_DX].w = (uint16_t)r;
        } else {
            uint32_t q = num / (uint16_t)src, r = num % (uint16_t)src;
            if (q > 0xFFFF) { raise_fault(m, in, 0); return; }
            c->r[R_AX].w = (uint16_t)q;
            c->r[R_DX].w = (uint16_t)r;
        }
    } else {
        uint64_t num = (uint64_t)c->r[R_DX].e << 32 | c->r[R_AX].e;
        if (is_signed) {
            int64_t q = (int64_t)num / (int32_t)src, r = (int64_t)num % (int32_t)src;
            if (q > INT32_MAX || q < INT32_MIN) { raise_fault(m, in, 0); return; }
            c->r[R_AX].e = (uint32_t)q;
            c->r[R_DX].e = (uint32_t)r;
        } else {
            uint64_t q = num / src, r = num % src;
            if (q > 0xFFFFFFFFu) { raise_fault(m, in, 0); return; }
            c->r[R_AX].e = (uint32_t)q;
            c->r[R_DX].e = (uint32_t)r;
        }
    }
}

static uint32_t imul_trunc(Machine *m, int size, uint32_t a, uint32_t b)
{
    Cpu *c = &m->cpu;
    int64_t res;
    bool overflow;
    if (size == 2) {
        res = (int64_t)(int16_t)a * (int16_t)b;
        overflow = res != (int16_t)res;
    } else {
        res = (int64_t)(int32_t)a * (int32_t)b;
        overflow = res != (int32_t)res;
    }
    set_flag(c, F_CF, overflow);
    set_flag(c, F_OF, overflow);
    return (uint32_t)res & size_mask(size);
}

/* ---- string instructions ---- */

static uint32_t string_index(Machine *m, const Insn *in, int reg)
{
    return in->addr32 ? m->cpu.r[reg].e : m->cpu.r[reg].w;
}

static void string_step(Machine *m, const Insn *in, int reg, int size)
{
    int32_t delta = (m->cpu.flags & F_DF) ? -size : size;
    if (in->addr32)
        m->cpu.r[reg].e += (uint32_t)delta;
    else
        m->cpu.r[reg].w = (uint16_t)(m->cpu.r[reg].w + delta);
}

static uint32_t count_reg(Machine *m, const Insn *in)
{
    return in->addr32 ? m->cpu.r[R_CX].e : m->cpu.r[R_CX].w;
}

static void set_count_reg(Machine *m, const Insn *in, uint32_t v)
{
    if (in->addr32)
        m->cpu.r[R_CX].e = v;
    else
        m->cpu.r[R_CX].w = (uint16_t)v;
}

static void string_op(Machine *m, const Insn *in, uint8_t opcode)
{
    Cpu *c = &m->cpu;
    int size = (opcode & 1) ? (in->op32 ? 4 : 2) : 1;
    uint32_t src_base = seg_base_for(m, in, S_DS), dst_base = c->seg_base[S_ES];
    bool repeated = in->rep != 0;
    uint32_t count = repeated ? count_reg(m, in) : 1;
    for (; count != 0; count--) {
        uint32_t a, b;
        bool compare = false;
        switch (opcode & 0xFE) {
        case 0x6C: /* ins */
            write_mem(m, size, dst_base + string_index(m, in, R_DI), port_in(m, c->r[R_DX].w, size));
            string_step(m, in, R_DI, size);
            break;
        case 0x6E: /* outs */
            port_out(m, c->r[R_DX].w, read_mem(m, size, src_base + string_index(m, in, R_SI)), size);
            string_step(m, in, R_SI, size);
            break;
        case 0xA4: /* movs */
            write_mem(m, size, dst_base + string_index(m, in, R_DI),
                      read_mem(m, size, src_base + string_index(m, in, R_SI)));
            string_step(m, in, R_SI, size);
            string_step(m, in, R_DI, size);
            break;
        case 0xA6: /* cmps */
            a = read_mem(m, size, src_base + string_index(m, in, R_SI));
            b = read_mem(m, size, dst_base + string_index(m, in, R_DI));
            alu(m, ALU_CMP, size, a, b);
            string_step(m, in, R_SI, size);
            string_step(m, in, R_DI, size);
            compare = true;
            break;
        case 0xAA: /* stos */
            write_mem(m, size, dst_base + string_index(m, in, R_DI), get_reg(m, size, R_AX));
            string_step(m, in, R_DI, size);
            break;
        case 0xAC: /* lods */
            set_reg(m, size, R_AX, read_mem(m, size, src_base + string_index(m, in, R_SI)));
            string_step(m, in, R_SI, size);
            break;
        case 0xAE: /* scas */
            b = read_mem(m, size, dst_base + string_index(m, in, R_DI));
            alu(m, ALU_CMP, size, get_reg(m, size, R_AX), b);
            string_step(m, in, R_DI, size);
            compare = true;
            break;
        }
        if (!repeated)
            return;
        set_count_reg(m, in, count - 1);
        if (compare) {
            bool zf = (c->flags & F_ZF) != 0;
            if ((in->rep == 0xF3 && !zf) || (in->rep == 0xF2 && zf))
                return;
        }
    }
}

/* ---- BCD ---- */

static void bcd_adjust(Machine *m, uint8_t opcode)
{
    Cpu *c = &m->cpu;
    uint8_t al = c->r[R_AX].b.l;
    bool cf = (c->flags & F_CF) != 0, af = (c->flags & F_AF) != 0;
    switch (opcode) {
    case 0x27: /* daa */
    case 0x2F: { /* das */
        bool sub = opcode == 0x2F;
        uint8_t old = al;
        bool old_cf = cf;
        cf = false;
        if ((al & 0x0F) > 9 || af) {
            al = sub ? (uint8_t)(al - 6) : (uint8_t)(al + 6);
            cf = old_cf || (sub ? old < 6 : old > 0xF9);
            af = true;
        } else {
            af = false;
        }
        if (old > 0x99 || old_cf) {
            al = sub ? (uint8_t)(al - 0x60) : (uint8_t)(al + 0x60);
            cf = true;
        }
        c->r[R_AX].b.l = al;
        set_szp(c, 1, al);
        break;
    }
    case 0x37: /* aaa */
    case 0x3F: /* aas */
        if ((al & 0x0F) > 9 || af) {
            if (opcode == 0x37) {
                c->r[R_AX].w += 0x106;
            } else {
                c->r[R_AX].w -= 6;
                c->r[R_AX].b.h -= 1;
            }
            af = cf = true;
        } else {
            af = cf = false;
        }
        c->r[R_AX].b.l &= 0x0F;
        break;
    }
    set_flag(c, F_CF, cf);
    set_flag(c, F_AF, af);
}

/* ---- 0F-prefixed (286/386) ---- */

static uint32_t bit_test(Machine *m, const Insn *in, int size, int sub, const Operand *op, uint32_t bit,
                         bool bit_from_reg)
{
    Cpu *c = &m->cpu;
    uint32_t addr = op->addr;
    if (!op->is_reg && bit_from_reg) {
        int32_t sbit = size == 2 ? (int16_t)bit : (int32_t)bit;
        addr = operand_part(in, op, (uint32_t)((sbit >> (size == 2 ? 4 : 5)) * size));
    }
    bit &= size * 8 - 1;
    uint32_t v = op->is_reg ? get_reg(m, size, op->index) : read_mem(m, size, addr);
    set_flag(c, F_CF, (v >> bit) & 1);
    switch (sub) {
    case 5: v |= 1u << bit; break;
    case 6: v &= ~(1u << bit); break;
    case 7: v ^= 1u << bit; break;
    default: return 0;
    }
    if (op->is_reg)
        set_reg(m, size, op->index, v);
    else
        write_mem(m, size, addr, v);
    return 0;
}

static uint32_t double_shift(Machine *m, int size, uint32_t dst, uint32_t src, unsigned count, bool left)
{
    Cpu *c = &m->cpu;
    unsigned bits = (unsigned)size * 8, top = size == 2 ? 48 : 64;
    count &= 0x1F;
    if (count == 0)
        return dst;
    /* A 16-bit shift by more than 16 keeps shifting in copies of src (hardware-verified). */
    uint64_t wide = size == 2 ? (left ? (uint64_t)dst << 32 | (uint64_t)src << 16 | src
                                      : (uint64_t)src << 32 | (uint64_t)src << 16 | dst)
                              : (left ? (uint64_t)dst << 32 | src : (uint64_t)src << 32 | dst);
    uint32_t res;
    if (left) {
        res = (uint32_t)(wide >> (top - bits - count)) & size_mask(size);
        set_flag(c, F_CF, (wide >> (top - count)) & 1);
    } else {
        res = (uint32_t)(wide >> count) & size_mask(size);
        set_flag(c, F_CF, (wide >> (count - 1)) & 1);
    }
    set_flag(c, F_OF, ((res ^ dst) & sign_bit(size)) != 0);
    set_szp(c, size, res);
    return res;
}

static void load_far_pointer(Machine *m, const Insn *in, int sreg)
{
    int size = in->op32 ? 4 : 2;
    Operand op = decode_modrm(m, in);
    if (op.is_reg) {
        invalid_opcode(m, in, 0xC4);
        return;
    }
    uint32_t off = read_mem(m, size, op.addr);
    uint16_t seg = mem_rd16(m, operand_part(in, &op, (uint32_t)size));
    set_reg(m, size, op.reg_field, off);
    cpu_set_seg(&m->cpu, sreg, seg);
    if (sreg == S_SS)
        m->cpu.irq_inhibit = true;
}

static void exec_0f(Machine *m, Insn *in)
{
    Cpu *c = &m->cpu;
    int size = in->op32 ? 4 : 2;
    uint8_t op2 = fetch8(m);
    Operand op;
    uint32_t v;
    if (op2 >= 0x80 && op2 <= 0x8F) {
        int32_t rel = in->op32 ? (int32_t)fetch32(m) : (int16_t)fetch16(m);
        if (condition(c, op2 & 0xF))
            c->eip = (c->eip + (uint32_t)rel) & 0xFFFF;
        return;
    }
    if (op2 >= 0x90 && op2 <= 0x9F) {
        op = decode_modrm(m, in);
        write_rm(m, 1, &op, condition(c, op2 & 0xF) ? 1 : 0);
        return;
    }
    switch (op2) {
    case 0x01: /* group 7: only SMSW is meaningful in real mode */
        op = decode_modrm(m, in);
        if (op.reg_field == 4)
            write_rm(m, 2, &op, (uint16_t)CR0_REAL_MODE);
        else if (op.reg_field != 6)
            log_msg("cpu: 0F 01 /%d ignored at %04X:%04X", op.reg_field, c->seg[S_CS], in->start_eip);
        return;
    case 0x06: /* clts: CR0 is not modelled */
        return;
    case 0x20: case 0x21: case 0x22: case 0x23: {
        /* MOV to/from CRn/DRn ignores mod; reads give reset values and writes are dropped. */
        uint8_t modrm = fetch8(m);
        int n = modrm >> 3 & 7;
        if (op2 == 0x20)
            c->r[modrm & 7].e = n == 0 ? CR0_REAL_MODE : 0;
        else if (op2 == 0x21)
            c->r[modrm & 7].e = n == 6 ? 0xFFFF0FF0u : 0;
        else if (op2 == 0x22 && n == 0 && (c->r[modrm & 7].e & 1))
            log_msg("cpu: protected mode not supported at %04X:%04X", c->seg[S_CS], in->start_eip);
        return;
    }
    case 0xA0: push_op(m, in, c->seg[S_FS]); return;
    case 0xA1: cpu_set_seg(c, S_FS, (uint16_t)pop_op(m, in)); return;
    case 0xA8: push_op(m, in, c->seg[S_GS]); return;
    case 0xA9: cpu_set_seg(c, S_GS, (uint16_t)pop_op(m, in)); return;
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: {
        static const int sub_of[4] = {4, 5, 6, 7};
        op = decode_modrm(m, in);
        bit_test(m, in, size, sub_of[(op2 >> 3) & 3], &op, get_reg(m, size, op.reg_field), true);
        return;
    }
    case 0xBA:
        op = decode_modrm(m, in);
        v = fetch8(m);
        if (op.reg_field < 4) {
            invalid_opcode(m, in, 0x0F);
            return;
        }
        bit_test(m, in, size, op.reg_field, &op, v, false);
        return;
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: {
        op = decode_modrm(m, in);
        unsigned count = (op2 & 1) ? c->r[R_CX].b.l : fetch8(m);
        uint32_t res = double_shift(m, size, read_rm(m, size, &op), get_reg(m, size, op.reg_field), count,
                                    op2 < 0xA8);
        write_rm(m, size, &op, res);
        return;
    }
    case 0xAF:
        op = decode_modrm(m, in);
        set_reg(m, size, op.reg_field, imul_trunc(m, size, get_reg(m, size, op.reg_field), read_rm(m, size, &op)));
        return;
    case 0xB2: load_far_pointer(m, in, S_SS); return;
    case 0xB4: load_far_pointer(m, in, S_FS); return;
    case 0xB5: load_far_pointer(m, in, S_GS); return;
    case 0xB6: case 0xB7: case 0xBE: case 0xBF: {
        op = decode_modrm(m, in);
        bool word_src = op2 & 1, sign = op2 >= 0xBE;
        v = read_rm(m, word_src ? 2 : 1, &op);
        if (sign)
            v = word_src ? (uint32_t)(int32_t)(int16_t)v : (uint32_t)(int32_t)(int8_t)v;
        set_reg(m, size, op.reg_field, v);
        return;
    }
    case 0xBC: case 0xBD: {
        op = decode_modrm(m, in);
        v = read_rm(m, size, &op);
        set_flag(c, F_ZF, v == 0);
        if (v == 0)
            return;
        int bit = op2 == 0xBC ? __builtin_ctz(v) : 31 - __builtin_clz(v);
        set_reg(m, size, op.reg_field, (uint32_t)bit);
        return;
    }
    default:
        invalid_opcode(m, in, 0x0F00u | op2);
    }
}

/* ---- groups ---- */

static void group_unary(Machine *m, Insn *in, int size)
{
    Cpu *c = &m->cpu;
    Operand op = decode_modrm(m, in);
    uint32_t v = read_rm(m, size, &op);
    switch (op.reg_field) {
    case 0:
    case 1:
        alu(m, ALU_AND, size, v, fetch_imm(m, size));
        break;
    case 2:
        write_rm(m, size, &op, ~v & size_mask(size));
        break;
    case 3:
        write_rm(m, size, &op, alu(m, ALU_SUB, size, 0, v));
        set_flag(c, F_CF, (v & size_mask(size)) != 0);
        break;
    default:
        mul_group(m, in, size, op.reg_field, v);
    }
}

static void group_ff(Machine *m, Insn *in, int size)
{
    Cpu *c = &m->cpu;
    Operand op = decode_modrm(m, in);
    uint32_t v;
    switch (op.reg_field) {
    case 0:
    case 1:
        write_rm(m, size, &op, inc_dec(m, size, read_rm(m, size, &op), op.reg_field == 1));
        return;
    }
    if (size == 1) {
        invalid_opcode(m, in, 0xFE);
        return;
    }
    switch (op.reg_field) {
    case 2:
        v = read_rm(m, size, &op);
        push_op(m, in, c->eip);
        c->eip = v & 0xFFFF;
        lift_coverage_hit(m, LIFT_COVERAGE_CALL);
        break;
    case 3:
        if (op.is_reg) { invalid_opcode(m, in, 0xFF); return; }
        v = read_mem(m, size, op.addr);
        push_op(m, in, c->seg[S_CS]);
        push_op(m, in, c->eip);
        far_jump(m, mem_rd16(m, operand_part(in, &op, (uint32_t)size)), v);
        lift_coverage_hit(m, LIFT_COVERAGE_CALL);
        break;
    case 4:
        c->eip = read_rm(m, size, &op) & 0xFFFF;
        break;
    case 5:
        if (op.is_reg) { invalid_opcode(m, in, 0xFF); return; }
        far_jump(m, mem_rd16(m, operand_part(in, &op, (uint32_t)size)), read_mem(m, size, op.addr));
        break;
    case 6:
        push_op(m, in, read_rm(m, size, &op));
        break;
    default:
        invalid_opcode(m, in, 0xFF);
    }
}

/* FE 38 id: host callback trap planted in the BIOS segment. */
static void group_fe(Machine *m, Insn *in)
{
    Cpu *c = &m->cpu;
    uint8_t modrm = mem_rd8(m, c->seg_base[S_CS] + c->eip);
    if (modrm == 0x38) {
        c->eip = (c->eip + 1) & 0xFFFF;
        uint8_t id = fetch8(m);
        CallbackFn fn = id < m->callback_count ? m->callbacks[id] : NULL;
        if (!fn) {
            log_msg("cpu: unknown callback %u", id);
            return;
        }
        if (fn(m) == CB_RETRY) {
            c->eip = in->start_eip;
            c->yielded = true;
        } else if (m->io_mode != IO_LIVE) {
            lift_io_callback(m, id);
        }
        return;
    }
    group_ff(m, in, 1);
}

/* ---- main decode ---- */

/* LOCK is only legal on read-modify-write instructions with a memory destination. */
static bool lock_allowed(Machine *m, uint8_t opcode)
{
    Cpu *c = &m->cpu;
    uint8_t b1 = mem_rd8(m, c->seg_base[S_CS] + (c->eip & 0xFFFF));
    uint8_t modrm = opcode == 0x0F ? mem_rd8(m, c->seg_base[S_CS] + ((c->eip + 1) & 0xFFFF)) : b1;
    int reg = modrm >> 3 & 7;
    if (modrm >> 6 == 3)
        return false;
    switch (opcode) {
    case 0x0F: return b1 == 0xAB || b1 == 0xB3 || b1 == 0xBB || (b1 == 0xBA && reg >= 5);
    case 0x80: case 0x81: case 0x82: case 0x83: return reg != ALU_CMP;
    case 0x86: case 0x87: return true;
    case 0xF6: case 0xF7: return reg == 2 || reg == 3;
    case 0xFE: case 0xFF: return reg < 2;
    }
    return opcode < 0x40 && (opcode & 6) == 0 && (opcode >> 3) != ALU_CMP;
}

static int op_size(const Insn *in) { return in->op32 ? 4 : 2; }

static void exec_one(Machine *m)
{
    Cpu *c = &m->cpu;
    Insn in = {.start_eip = c->eip, .seg = S_NONE};
    uint8_t opcode;
    Operand op;
    uint32_t v, w;
    int size;

    for (;;) {
        opcode = fetch8(m);
        switch (opcode) {
        case 0x26: in.seg = S_ES; continue;
        case 0x2E: in.seg = S_CS; continue;
        case 0x36: in.seg = S_SS; continue;
        case 0x3E: in.seg = S_DS; continue;
        case 0x64: in.seg = S_FS; continue;
        case 0x65: in.seg = S_GS; continue;
        case 0x66: in.op32 = true; continue;
        case 0x67: in.addr32 = true; continue;
        case 0xF0: in.lock = true; continue;
        case 0xF2: case 0xF3: in.rep = opcode; continue;
        }
        break;
    }

    if (in.lock && !lock_allowed(m, opcode)) {
        invalid_opcode(m, &in, 0xF0);
        return;
    }

    if (opcode < 0x40 && (opcode & 7) < 6) {
        int aluop = opcode >> 3;
        size = (opcode & 1) ? op_size(&in) : 1;
        switch (opcode & 7) {
        case 0: case 1:
            op = decode_modrm(m, &in);
            v = alu(m, aluop, size, read_rm(m, size, &op), get_reg(m, size, op.reg_field));
            if (aluop != ALU_CMP)
                write_rm(m, size, &op, v);
            return;
        case 2: case 3:
            op = decode_modrm(m, &in);
            v = alu(m, aluop, size, get_reg(m, size, op.reg_field), read_rm(m, size, &op));
            if (aluop != ALU_CMP)
                set_reg(m, size, op.reg_field, v);
            return;
        default:
            v = alu(m, aluop, size, get_reg(m, size, R_AX), fetch_imm(m, size));
            if (aluop != ALU_CMP)
                set_reg(m, size, R_AX, v);
            return;
        }
    }

    switch (opcode) {
    case 0x06: push_op(m, &in, c->seg[S_ES]); return;
    case 0x07: cpu_set_seg(c, S_ES, (uint16_t)pop_op(m, &in)); return;
    case 0x0E: push_op(m, &in, c->seg[S_CS]); return;
    case 0x0F: exec_0f(m, &in); return;
    case 0x16: push_op(m, &in, c->seg[S_SS]); return;
    case 0x17: cpu_set_seg(c, S_SS, (uint16_t)pop_op(m, &in)); c->irq_inhibit = true; return;
    case 0x1E: push_op(m, &in, c->seg[S_DS]); return;
    case 0x1F: cpu_set_seg(c, S_DS, (uint16_t)pop_op(m, &in)); return;
    case 0x27: case 0x2F: case 0x37: case 0x3F: bcd_adjust(m, opcode); return;

    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        size = op_size(&in);
        set_reg(m, size, opcode & 7, inc_dec(m, size, get_reg(m, size, opcode & 7), opcode >= 0x48));
        return;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        push_op(m, &in, get_reg(m, op_size(&in), opcode & 7));
        return;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        set_reg(m, op_size(&in), opcode & 7, pop_op(m, &in));
        return;
    case 0x60: { /* pusha */
        uint32_t sp = get_reg(m, op_size(&in), R_SP);
        for (int r = 0; r < 8; r++)
            push_op(m, &in, r == R_SP ? sp : get_reg(m, op_size(&in), r));
        return;
    }
    case 0x61: /* popa */
        w = 0;
        for (int r = 7; r >= 0; r--) {
            v = pop_op(m, &in);
            if (r != R_SP)
                set_reg(m, op_size(&in), r, v);
            else
                w = v;
        }
        /* POPAD on a 16-bit stack loads ESP's high word from the popped image (hardware-verified). */
        if (in.op32)
            c->r[R_SP].e = (w & 0xFFFF0000u) | c->r[R_SP].w;
        return;
    case 0x62: { /* bound */
        size = op_size(&in);
        op = decode_modrm(m, &in);
        if (op.is_reg) {
            invalid_opcode(m, &in, opcode);
            return;
        }
        int32_t idx = size == 2 ? (int16_t)get_reg(m, 2, op.reg_field) : (int32_t)get_reg(m, 4, op.reg_field);
        int32_t lo = size == 2 ? (int16_t)read_mem(m, 2, op.addr) : (int32_t)read_mem(m, 4, op.addr);
        uint32_t hi_addr = operand_part(&in, &op, (uint32_t)size);
        int32_t hi = size == 2 ? (int16_t)read_mem(m, 2, hi_addr) : (int32_t)read_mem(m, 4, hi_addr);
        if (idx < lo || idx > hi)
            raise_fault(m, &in, 5);
        return;
    }
    case 0x68: push_op(m, &in, fetch_imm(m, op_size(&in))); return;
    case 0x6A: push_op(m, &in, (uint32_t)(int32_t)(int8_t)fetch8(m)); return;
    case 0x69: case 0x6B:
        size = op_size(&in);
        op = decode_modrm(m, &in);
        v = read_rm(m, size, &op);
        w = opcode == 0x69 ? fetch_imm(m, size) : (uint32_t)(int32_t)(int8_t)fetch8(m);
        set_reg(m, size, op.reg_field, imul_trunc(m, size, v, w));
        return;
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(m, &in, opcode);
        return;

    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t rel = (int8_t)fetch8(m);
        if (condition(c, opcode & 0xF))
            c->eip = (c->eip + (uint32_t)rel) & 0xFFFF;
        return;
    }
    case 0x80: case 0x81: case 0x82: case 0x83:
        size = (opcode & 1) ? op_size(&in) : 1;
        op = decode_modrm(m, &in);
        v = read_rm(m, size, &op);
        w = opcode == 0x83 ? (uint32_t)(int32_t)(int8_t)fetch8(m) : fetch_imm(m, size);
        v = alu(m, op.reg_field, size, v, w);
        if (op.reg_field != ALU_CMP)
            write_rm(m, size, &op, v);
        return;
    case 0x84: case 0x85:
        size = (opcode & 1) ? op_size(&in) : 1;
        op = decode_modrm(m, &in);
        alu(m, ALU_AND, size, read_rm(m, size, &op), get_reg(m, size, op.reg_field));
        return;
    case 0x86: case 0x87:
        size = (opcode & 1) ? op_size(&in) : 1;
        op = decode_modrm(m, &in);
        v = read_rm(m, size, &op);
        write_rm(m, size, &op, get_reg(m, size, op.reg_field));
        set_reg(m, size, op.reg_field, v);
        return;
    case 0x88: case 0x89:
        size = (opcode & 1) ? op_size(&in) : 1;
        op = decode_modrm(m, &in);
        write_rm(m, size, &op, get_reg(m, size, op.reg_field));
        return;
    case 0x8A: case 0x8B:
        size = (opcode & 1) ? op_size(&in) : 1;
        op = decode_modrm(m, &in);
        set_reg(m, size, op.reg_field, read_rm(m, size, &op));
        return;
    case 0x8C:
        op = decode_modrm(m, &in);
        if (op.reg_field > S_GS) {
            invalid_opcode(m, &in, opcode);
            return;
        }
        write_rm(m, op.is_reg ? op_size(&in) : 2, &op, c->seg[op.reg_field & 7]);
        return;
    case 0x8D:
        op = decode_modrm(m, &in);
        if (op.is_reg) {
            invalid_opcode(m, &in, opcode);
            return;
        }
        set_reg(m, op_size(&in), op.reg_field, op.off);
        return;
    case 0x8E:
        op = decode_modrm(m, &in);
        if (op.reg_field == S_CS || op.reg_field > S_GS) {
            invalid_opcode(m, &in, 0x8E);
            return;
        }
        cpu_set_seg(c, op.reg_field, (uint16_t)read_rm(m, 2, &op));
        if (op.reg_field == S_SS)
            c->irq_inhibit = true;
        return;
    case 0x8F: {
        /* The destination address is computed after the pop increments SP. */
        uint32_t old_sp = c->r[R_SP].e;
        v = pop_op(m, &in);
        op = decode_modrm(m, &in);
        if (op.reg_field != 0) {
            c->r[R_SP].e = old_sp;
            invalid_opcode(m, &in, opcode);
            return;
        }
        write_rm(m, op_size(&in), &op, v);
        return;
    }
    case 0x90:
        return;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        size = op_size(&in);
        v = get_reg(m, size, R_AX);
        set_reg(m, size, R_AX, get_reg(m, size, opcode & 7));
        set_reg(m, size, opcode & 7, v);
        return;
    case 0x98:
        if (in.op32)
            c->r[R_AX].e = (uint32_t)(int32_t)(int16_t)c->r[R_AX].w;
        else
            c->r[R_AX].w = (uint16_t)(int16_t)(int8_t)c->r[R_AX].b.l;
        return;
    case 0x99:
        if (in.op32)
            c->r[R_DX].e = (c->r[R_AX].e & 0x80000000u) ? 0xFFFFFFFFu : 0;
        else
            c->r[R_DX].w = (c->r[R_AX].w & 0x8000) ? 0xFFFF : 0;
        return;
    case 0x9A:
        v = fetch_imm(m, op_size(&in));
        w = fetch16(m);
        push_op(m, &in, c->seg[S_CS]);
        push_op(m, &in, c->eip);
        far_jump(m, (uint16_t)w, v);
        lift_coverage_hit(m, LIFT_COVERAGE_CALL);
        return;
    case 0x9B:
        if (m->fpu_present)
            fpu_wait(m);
        return;
    case 0x9C: push_op(m, &in, c->flags & (in.op32 ? 0x00FCFFFFu : 0xFFFF)); return;
    case 0x9D: write_flags(c, pop_op(m, &in), in.op32); return;
    case 0x9E: c->flags = (c->flags & ~0xD5u) | (c->r[R_AX].b.h & 0xD5u) | 2; return;
    case 0x9F: c->r[R_AX].b.h = (uint8_t)c->flags; return;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        size = (opcode & 1) ? op_size(&in) : 1;
        uint32_t off = in.addr32 ? fetch32(m) : fetch16(m);
        uint32_t addr = seg_base_for(m, &in, S_DS) + off;
        if (opcode < 0xA2)
            set_reg(m, size, R_AX, read_mem(m, size, addr));
        else
            write_mem(m, size, addr, get_reg(m, size, R_AX));
        return;
    }
    case 0xA8: case 0xA9:
        size = (opcode & 1) ? op_size(&in) : 1;
        alu(m, ALU_AND, size, get_reg(m, size, R_AX), fetch_imm(m, size));
        return;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        set_reg(m, 1, opcode & 7, fetch8(m));
        return;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        set_reg(m, op_size(&in), opcode & 7, fetch_imm(m, op_size(&in)));
        return;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        size = (opcode & 1) ? op_size(&in) : 1;
        op = decode_modrm(m, &in);
        unsigned count = opcode <= 0xC1 ? fetch8(m) : opcode <= 0xD1 ? 1 : c->r[R_CX].b.l;
        write_rm(m, size, &op, shift_rotate(m, op.reg_field, size, read_rm(m, size, &op), count));
        return;
    }
    case 0xC2: case 0xC3:
        v = opcode == 0xC2 ? fetch16(m) : 0;
        c->eip = pop_op(m, &in) & 0xFFFF;
        c->r[R_SP].w += (uint16_t)v;
        return;
    case 0xC4: load_far_pointer(m, &in, S_ES); return;
    case 0xC5: load_far_pointer(m, &in, S_DS); return;
    case 0xC6: case 0xC7:
        size = (opcode & 1) ? op_size(&in) : 1;
        op = decode_modrm(m, &in);
        if (op.reg_field != 0) {
            invalid_opcode(m, &in, opcode);
            return;
        }
        write_rm(m, size, &op, fetch_imm(m, size));
        return;
    case 0xC8: { /* enter */
        uint16_t frame = fetch16(m);
        uint8_t level = fetch8(m) & 0x1F;
        size = op_size(&in);
        push_op(m, &in, get_reg(m, size, R_BP));
        uint16_t frame_ptr = c->r[R_SP].w;
        for (int i = 1; i < level; i++) {
            c->r[R_BP].w -= (uint16_t)size;
            push_op(m, &in, read_mem(m, size, c->seg_base[S_SS] + c->r[R_BP].w));
        }
        if (level > 0)
            push_op(m, &in, frame_ptr);
        set_reg(m, size, R_BP, frame_ptr);
        c->r[R_SP].w -= frame;
        return;
    }
    case 0xC9:
        c->r[R_SP].w = c->r[R_BP].w;
        set_reg(m, op_size(&in), R_BP, pop_op(m, &in));
        return;
    case 0xCA: case 0xCB:
        v = opcode == 0xCA ? fetch16(m) : 0;
        w = pop_op(m, &in);
        far_jump(m, (uint16_t)pop_op(m, &in), w);
        c->r[R_SP].w += (uint16_t)v;
        return;
    case 0xCC: cpu_interrupt(m, 3); return;
    case 0xCD: cpu_interrupt(m, fetch8(m)); return;
    case 0xCE:
        if (c->flags & F_OF)
            cpu_interrupt(m, 4);
        return;
    case 0xCF:
        v = pop_op(m, &in);
        w = pop_op(m, &in);
        far_jump(m, (uint16_t)w, v);
        write_flags(c, pop_op(m, &in), in.op32);
        return;
    case 0xD4: { /* aam */
        uint8_t base = fetch8(m);
        if (base == 0) {
            raise_fault(m, &in, 0);
            return;
        }
        uint8_t al = c->r[R_AX].b.l;
        c->r[R_AX].b.h = al / base;
        c->r[R_AX].b.l = al % base;
        set_szp(c, 1, c->r[R_AX].b.l);
        return;
    }
    case 0xD5: { /* aad */
        uint8_t base = fetch8(m);
        c->r[R_AX].w = (uint8_t)(c->r[R_AX].b.l + c->r[R_AX].b.h * base);
        set_szp(c, 1, c->r[R_AX].b.l);
        return;
    }
    case 0xD6: c->r[R_AX].b.l = (c->flags & F_CF) ? 0xFF : 0; return;
    case 0xD7:
        v = in.addr32 ? c->r[R_BX].e + c->r[R_AX].b.l : (uint16_t)(c->r[R_BX].w + c->r[R_AX].b.l);
        c->r[R_AX].b.l = mem_rd8(m, seg_base_for(m, &in, S_DS) + v);
        return;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
        uint8_t modrm = mem_rd8(m, c->seg_base[S_CS] + (c->eip & 0xFFFF));
        Operand op = decode_modrm(m, &in);
        if (!m->fpu_present)
            return;
        uint32_t base = op.addr - op.off;
        FpuInsn fi = {.opcode = opcode, .modrm = modrm, .is_reg = op.is_reg, .addr = op.addr,
                      .seg = (uint16_t)(base >> 4), .off = (uint16_t)op.off, .op32 = in.op32,
                      .cs = c->seg[S_CS], .ip = in.start_eip};
        fpu_exec(m, &fi);
        return;
    }
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: {
        int8_t rel = (int8_t)fetch8(m);
        uint32_t count = count_reg(m, &in);
        bool take;
        if (opcode == 0xE3) {
            take = count == 0;
        } else {
            count--;
            set_count_reg(m, &in, count);
            take = count != 0;
            if (opcode == 0xE0)
                take = take && !(c->flags & F_ZF);
            else if (opcode == 0xE1)
                take = take && (c->flags & F_ZF);
        }
        if (take)
            c->eip = (c->eip + (uint32_t)rel) & 0xFFFF;
        return;
    }
    case 0xE4: case 0xE5:
        size = (opcode & 1) ? op_size(&in) : 1;
        set_reg(m, size, R_AX, port_in(m, fetch8(m), size));
        return;
    case 0xE6: case 0xE7:
        size = (opcode & 1) ? op_size(&in) : 1;
        v = fetch8(m);
        port_out(m, (uint16_t)v, get_reg(m, size, R_AX), size);
        return;
    case 0xE8:
        v = in.op32 ? fetch32(m) : fetch16(m);
        push_op(m, &in, c->eip);
        c->eip = (c->eip + v) & 0xFFFF;
        lift_coverage_hit(m, LIFT_COVERAGE_CALL);
        return;
    case 0xE9:
        v = in.op32 ? fetch32(m) : fetch16(m);
        c->eip = (c->eip + v) & 0xFFFF;
        return;
    case 0xEA:
        v = fetch_imm(m, op_size(&in));
        far_jump(m, fetch16(m), v);
        return;
    case 0xEB: {
        int8_t rel = (int8_t)fetch8(m);
        c->eip = (c->eip + (uint32_t)rel) & 0xFFFF;
        return;
    }
    case 0xEC: case 0xED:
        size = (opcode & 1) ? op_size(&in) : 1;
        set_reg(m, size, R_AX, port_in(m, c->r[R_DX].w, size));
        return;
    case 0xEE: case 0xEF:
        size = (opcode & 1) ? op_size(&in) : 1;
        port_out(m, c->r[R_DX].w, get_reg(m, size, R_AX), size);
        return;
    case 0xF1: /* icebp */
        cpu_interrupt(m, 1);
        return;
    case 0xF4:
        c->halted = true;
        if (m->io_mode != IO_LIVE)
            lift_io_halt(m);
        return;
    case 0xF5: c->flags ^= F_CF; return;
    case 0xF6: group_unary(m, &in, 1); return;
    case 0xF7: group_unary(m, &in, op_size(&in)); return;
    case 0xF8: c->flags &= ~F_CF; return;
    case 0xF9: c->flags |= F_CF; return;
    case 0xFA: c->flags &= ~F_IF; return;
    case 0xFB: c->flags |= F_IF; c->irq_inhibit = true; return;
    case 0xFC: c->flags &= ~F_DF; return;
    case 0xFD: c->flags |= F_DF; return;
    case 0xFE: group_fe(m, &in); return;
    case 0xFF: group_ff(m, &in, op_size(&in)); return;
    default:
        invalid_opcode(m, &in, opcode);
    }
}

/* ---- run loop ---- */

static void trace_insn(Machine *m)
{
    Cpu *c = &m->cpu;
    fprintf(stderr, "%04X:%04X AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X DS=%04X ES=%04X SS=%04X F=%04X\n",
            c->seg[S_CS], c->eip, c->r[R_AX].w, c->r[R_BX].w, c->r[R_CX].w, c->r[R_DX].w, c->r[R_SI].w,
            c->r[R_DI].w, c->r[R_BP].w, c->r[R_SP].w, c->seg[S_DS], c->seg[S_ES], c->seg[S_SS],
            (unsigned)c->flags & 0xFFFF);
}

uint32_t *dosrt_interp_profile; /* INN_INTERP_PROFILE: instructions run per 16-byte paragraph */

uint64_t cpu_run(Machine *m, uint64_t budget)
{
    Cpu *c = &m->cpu;
    uint64_t done = 0;
    c->yielded = false;
    c->stop = false;
    while (done < budget && !m->exit_requested) {
        if (c->irq_inhibit) {
            c->irq_inhibit = false;
        } else if ((c->flags & F_IF) && __builtin_expect(m->irq_gate, 0)) {
            lift_irq_boundary(m);
        } else if ((c->flags & F_IF) && m->irq_pending && !m->irq_hold) {
            int vector = pic_acknowledge(m);
            if (vector >= 0)
                cpu_interrupt(m, (uint8_t)vector);
        }
        if (c->halted)
            break;
        uint32_t linear = c->seg_base[S_CS] + (c->eip & 0xFFFF);
        /* Lifted code runs from machine_step, so this loop stays off Asyncify's unwind path. */
        bool skip_hook = c->hook_skip == linear; /* declined just now: run the original once */
        c->hook_skip = UINT32_MAX;
        if (!skip_hook && lift_hooked(m, linear)) {
            c->hook_pending = true;
            break;
        }
        if (m->trace >= 2)
            trace_insn(m);
        if (__builtin_expect(dosrt_interp_profile != NULL, 0))
            dosrt_interp_profile[(linear & 0xFFFFF) >> 4]++;
        exec_one(m);
        done += 1 + c->stall;
        c->instructions += 1 + c->stall; /* kept current: machine_now_ns reads it mid-slice */
        c->stall = 0;
        if (c->yielded || c->stop)
            break;
    }
    return done;
}

void cpu_reset(Machine *m)
{
    Cpu *c = &m->cpu;
    memset(c, 0, sizeof *c);
    c->flags = 2 | F_IF;
    c->hook_skip = UINT32_MAX;
    fpu_init(&c->fpu);
    for (int s = 0; s < 6; s++)
        cpu_set_seg(c, s, 0);
}

void cb_set_carry(Machine *m, bool carry)
{
    uint32_t flags_addr = m->cpu.seg_base[S_SS] + (uint16_t)(m->cpu.r[R_SP].w + 4);
    uint16_t f = mem_rd16(m, flags_addr);
    mem_wr16(m, flags_addr, carry ? (uint16_t)(f | F_CF) : (uint16_t)(f & ~F_CF));
    set_flag(&m->cpu, F_CF, carry);
}

void cb_set_zero(Machine *m, bool zero)
{
    uint32_t flags_addr = m->cpu.seg_base[S_SS] + (uint16_t)(m->cpu.r[R_SP].w + 4);
    uint16_t f = mem_rd16(m, flags_addr);
    mem_wr16(m, flags_addr, zero ? (uint16_t)(f | F_ZF) : (uint16_t)(f & ~F_ZF));
    set_flag(&m->cpu, F_ZF, zero);
}
