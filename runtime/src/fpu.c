/* x87 FPU: the 387 instruction set (D8-DF escapes and FWAIT) for the interpreter. Precision,
 * rounding and exception choices are documented in docs/runtime.md, "FPU". */
#include <float.h>
#include <math.h>
#include <string.h>

#include "fpu.h"
#include "machine.h"

enum { TAG_VALID = 0, TAG_ZERO = 1, TAG_SPECIAL = 2, TAG_EMPTY = 3 };
enum { RC_NEAREST = 0, RC_DOWN = 1, RC_UP = 2, RC_CHOP = 3 };
enum { CW_INIT = 0x037F, EXC_MASK = 0x3F, CC_MASK = FPU_C0 | FPU_C1 | FPU_C2 | FPU_C3 };

static Fpu *fpu_of(Machine *m) { return &m->cpu.fpu; }

void fpu_init(Fpu *f)
{
    f->cw = CW_INIT;
    f->sw = 0;
    f->empty = 0xFF;
    f->fop = 0;
    f->fcs = f->fds = 0;
    f->fip = f->fdp = 0;
    for (int i = 0; i < 8; i++)
        f->st[i] = 0;
}

/* ---- numbers ---- */

static long double indefinite(void) { return copysignl(NAN, -1.0L); }

static long double round_to_bits(long double x, int bits, int rc)
{
    if (!isfinite(x) || x == 0)
        return x;
    int e;
    long double s = ldexpl(frexpl(x, &e), bits);
    switch (rc) {
    case RC_DOWN: s = floorl(s); break;
    case RC_UP: s = ceill(s); break;
    case RC_CHOP: s = truncl(s); break;
    default: s = nearbyintl(s); break;
    }
    return ldexpl(s, e - bits);
}

/* Keeps a host result at the x87's 64-bit significand (a no-op on x86 hosts). */
static long double r64(long double x)
{
#if LDBL_MANT_DIG == 64
    return x;
#else
    return round_to_bits(x, 64, RC_NEAREST);
#endif
}

static int rc_of(const Fpu *f) { return f->cw >> 10 & 3; }

/* Precision control for the basic arithmetic results. */
static long double round_pc(const Fpu *f, long double x)
{
    switch (f->cw >> 8 & 3) {
    case 0: return round_to_bits(x, 24, rc_of(f));
    case 2: return round_to_bits(x, 53, rc_of(f));
    default: return r64(x);
    }
}

static long double round_int(long double x, int rc)
{
    switch (rc) {
    case RC_DOWN: return floorl(x);
    case RC_UP: return ceill(x);
    case RC_CHOP: return truncl(x);
    default: return nearbyintl(x);
    }
}

long double fpu_from_ext80(const uint8_t b[10])
{
    uint64_t mant = 0;
    for (int i = 7; i >= 0; i--)
        mant = mant << 8 | b[i];
    int exp = (b[9] & 0x7F) << 8 | b[8];
    bool neg = b[9] & 0x80;
    long double v;
    if (exp == 0x7FFF)
        v = (mant << 1) == 0 ? INFINITY : NAN;
    else if (exp != 0 && !(mant >> 63))
        v = NAN; /* unnormal: the 387 treats it as an invalid operand */
    else
        v = ldexpl((long double)mant, (exp ? exp : 1) - 16383 - 63);
    return neg ? -v : v;
}

void fpu_to_ext80(long double v, uint8_t b[10])
{
    uint64_t mant;
    int exp;
    if (isnan(v)) {
        mant = 0xC000000000000000ull;
        exp = 0x7FFF;
    } else if (isinf(v)) {
        mant = 0x8000000000000000ull;
        exp = 0x7FFF;
    } else if (v == 0) {
        mant = 0;
        exp = 0;
    } else {
        int e;
        long double fr = frexpl(fabsl(v), &e); /* v = fr * 2^e, fr in [0.5, 1) */
        exp = e - 1 + 16383;
        if (exp <= 0) { /* denormal: fixed exponent field 0 means 2^-16382 */
            mant = (uint64_t)ldexpl(fabsl(v), 16382 + 63);
            exp = 0;
        } else if (exp >= 0x7FFF) {
            mant = 0x8000000000000000ull;
            exp = 0x7FFF;
        } else {
            mant = (uint64_t)ldexpl(fr, 64);
        }
    }
    for (int i = 0; i < 8; i++)
        b[i] = (uint8_t)(mant >> (8 * i));
    b[8] = (uint8_t)exp;
    b[9] = (uint8_t)(exp >> 8 | (signbit(v) ? 0x80 : 0));
}

static int tag_of(long double v)
{
    switch (fpclassify(v)) {
    case FP_ZERO: return TAG_ZERO;
    case FP_NORMAL: return fabsl(v) < ldexpl(1.0L, -16382) ? TAG_SPECIAL : TAG_VALID;
    default: return TAG_SPECIAL;
    }
}

/* ---- stack and exceptions ---- */

static int top(const Fpu *f) { return f->sw >> 11 & 7; }
static void set_top(Fpu *f, int t) { f->sw = (uint16_t)((f->sw & ~0x3800) | (t & 7) << 11); }
static int phys(const Fpu *f, int i) { return (top(f) + i) & 7; }
static bool is_empty(const Fpu *f, int i) { return f->empty >> phys(f, i) & 1; }

static void update_es(Fpu *f)
{
    if (f->sw & ~f->cw & EXC_MASK)
        f->sw |= FPU_ES | FPU_B;
    else
        f->sw &= (uint16_t)~(FPU_ES | FPU_B);
}

/* Records exceptions; true when all of them are masked, so the masked response applies. */
static bool raise_exc(Fpu *f, uint16_t flags)
{
    f->sw |= flags;
    update_es(f);
    return !(flags & ~f->cw & EXC_MASK);
}

static bool stack_fault(Fpu *f, bool overflow)
{
    f->sw = (uint16_t)((f->sw & ~FPU_C1) | (overflow ? FPU_C1 : 0));
    return raise_exc(f, FPU_IE | FPU_SF);
}

static long double get_st(const Fpu *f, int i) { return f->st[phys(f, i)]; }

static void set_st(Fpu *f, int i, long double v)
{
    int p = phys(f, i);
    f->st[p] = v;
    f->empty &= (uint8_t)~(1u << p);
}

/* Reads ST(i); an empty register is a stack underflow whose masked response is the indefinite. */
static bool read_st(Fpu *f, int i, long double *out)
{
    if (!is_empty(f, i)) {
        *out = get_st(f, i);
        return true;
    }
    if (!stack_fault(f, false))
        return false;
    *out = indefinite();
    return true;
}

static bool push(Fpu *f, long double v)
{
    int t = (top(f) - 1) & 7;
    if (!(f->empty >> t & 1)) {
        if (!stack_fault(f, true))
            return false;
        v = indefinite();
    }
    set_top(f, t);
    set_st(f, 0, v);
    return true;
}

static void pop(Fpu *f)
{
    f->empty |= (uint8_t)(1u << top(f));
    set_top(f, top(f) + 1);
}

/* ---- memory operands ---- */

static void read_bytes(Machine *m, uint32_t addr, uint8_t *b, int n)
{
    for (int i = 0; i < n; i++)
        b[i] = mem_rd8(m, addr + (uint32_t)i);
}

static void write_bytes(Machine *m, uint32_t addr, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++)
        mem_wr8(m, addr + (uint32_t)i, b[i]);
}

static uint64_t read_le(Machine *m, uint32_t addr, int n)
{
    uint8_t b[8];
    read_bytes(m, addr, b, n);
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | b[i];
    return v;
}

static void write_le(Machine *m, uint32_t addr, uint64_t v, int n)
{
    for (int i = 0; i < n; i++)
        mem_wr8(m, addr + (uint32_t)i, (uint8_t)(v >> (8 * i)));
}

static long double load_real(Machine *m, uint32_t addr, int size)
{
    if (size == 4) {
        uint32_t bits = (uint32_t)read_le(m, addr, 4);
        float v;
        memcpy(&v, &bits, 4);
        return v;
    }
    if (size == 8) {
        uint64_t bits = read_le(m, addr, 8);
        double v;
        memcpy(&v, &bits, 8);
        return v;
    }
    uint8_t b[10];
    read_bytes(m, addr, b, 10);
    return fpu_from_ext80(b);
}

static long double load_int(Machine *m, uint32_t addr, int size)
{
    uint64_t v = read_le(m, addr, size);
    if (size == 2)
        return (int16_t)v;
    if (size == 4)
        return (int32_t)v;
    return (long double)(int64_t)v;
}

/* Overflow/underflow flags for a value narrowed to float or double. */
static bool store_range_ok(Fpu *f, long double v, long double narrowed)
{
    if (isinf(narrowed) && isfinite(v))
        return raise_exc(f, FPU_OE | FPU_PE);
    if (narrowed == 0 && v != 0)
        return raise_exc(f, FPU_UE | FPU_PE);
    return true;
}

static bool store_real(Machine *m, Fpu *f, uint32_t addr, int size, long double v)
{
    if (size == 4) {
        float x = (float)round_to_bits(v, 24, rc_of(f));
        if (!store_range_ok(f, v, x))
            return false;
        uint32_t bits;
        memcpy(&bits, &x, 4);
        write_le(m, addr, bits, 4);
    } else if (size == 8) {
        double x = (double)round_to_bits(v, 53, rc_of(f));
        if (!store_range_ok(f, v, x))
            return false;
        uint64_t bits;
        memcpy(&bits, &x, 8);
        write_le(m, addr, bits, 8);
    } else {
        uint8_t b[10];
        fpu_to_ext80(v, b);
        write_bytes(m, addr, b, 10);
    }
    return true;
}

static bool store_int(Machine *m, Fpu *f, uint32_t addr, int size, long double v)
{
    long double r = round_int(v, rc_of(f));
    long double lim = size == 2 ? 32768.0L : size == 4 ? 2147483648.0L : 9223372036854775808.0L;
    uint64_t out;
    if (isnan(r) || r >= lim || r < -lim) {
        if (!raise_exc(f, FPU_IE))
            return false;
        out = 1ull << (size * 8 - 1); /* integer indefinite */
    } else {
        out = (uint64_t)(int64_t)r;
    }
    write_le(m, addr, out, size);
    return true;
}

static long double load_bcd(Machine *m, uint32_t addr)
{
    uint8_t b[10];
    read_bytes(m, addr, b, 10);
    long double v = 0;
    for (int i = 8; i >= 0; i--)
        v = v * 100 + (b[i] >> 4) * 10 + (b[i] & 15);
    return (b[9] & 0x80) ? -v : v;
}

static bool store_bcd(Machine *m, Fpu *f, uint32_t addr, long double v)
{
    uint8_t b[10] = {0};
    long double r = round_int(v, rc_of(f));
    if (isnan(r) || fabsl(r) >= 1e18L) {
        if (!raise_exc(f, FPU_IE))
            return false;
        b[7] = 0xC0, b[8] = 0xFF, b[9] = 0xFF; /* packed BCD indefinite */
    } else {
        uint64_t n = (uint64_t)fabsl(r);
        for (int i = 0; i < 9; i++, n /= 100)
            b[i] = (uint8_t)((n % 100 / 10) << 4 | n % 10);
        b[9] = signbit(r) ? 0x80 : 0;
    }
    write_bytes(m, addr, b, 10);
    return true;
}

/* ---- arithmetic ---- */

/* op: 0 add, 1 mul, 4 a-b, 5 b-a, 6 a/b, 7 b/a. Applies exceptions and precision control. */
static bool arith(Fpu *f, int op, long double a, long double b, long double *out)
{
    if (op == 5 || op == 7) {
        long double t = a;
        a = b;
        b = t;
        op--;
    }
    long double r;
    if (isnan(a) || isnan(b)) {
        *out = isnan(a) ? a : b;
        return true;
    }
    switch (op) {
    case 0:
        if (isinf(a) && isinf(b) && signbit(a) != signbit(b))
            goto invalid;
        r = a + b;
        break;
    case 1:
        if ((isinf(a) && b == 0) || (isinf(b) && a == 0))
            goto invalid;
        r = a * b;
        break;
    case 4:
        if (isinf(a) && isinf(b) && signbit(a) == signbit(b))
            goto invalid;
        r = a - b;
        break;
    default:
        if ((a == 0 && b == 0) || (isinf(a) && isinf(b)))
            goto invalid;
        if (b == 0) {
            if (!raise_exc(f, FPU_ZE))
                return false;
            *out = (signbit(a) != signbit(b)) ? -INFINITY : INFINITY;
            return true;
        }
        r = a / b;
        break;
    }
    r = round_pc(f, r);
    if (isinf(r) && isfinite(a) && isfinite(b) && !raise_exc(f, FPU_OE | FPU_PE))
        return false;
    if (r != 0 && fabsl(r) < ldexpl(1.0L, -16382) && !raise_exc(f, FPU_UE | FPU_PE))
        return false;
    *out = r;
    return true;
invalid:
    if (!raise_exc(f, FPU_IE))
        return false;
    *out = indefinite();
    return true;
}

/* Sets C3 C2 C0 from a comparison of a with b; NaN gives "unordered" (and IE unless quiet). */
static bool compare(Fpu *f, long double a, long double b, bool quiet)
{
    f->sw &= (uint16_t)~CC_MASK;
    if (isnan(a) || isnan(b)) {
        if (!quiet && !raise_exc(f, FPU_IE))
            return false;
        f->sw |= FPU_C3 | FPU_C2 | FPU_C0;
    } else if (a < b) {
        f->sw |= FPU_C0;
    } else if (a == b) {
        f->sw |= FPU_C3;
    }
    return true;
}

static void set_cc(Fpu *f, uint16_t cc) { f->sw = (uint16_t)((f->sw & ~CC_MASK) | cc); }

static void fxam(Fpu *f)
{
    long double v = get_st(f, 0);
    uint16_t cc = signbit(v) ? FPU_C1 : 0;
    if (is_empty(f, 0))
        cc |= FPU_C3 | FPU_C0;
    else if (isnan(v))
        cc |= FPU_C0;
    else if (isinf(v))
        cc |= FPU_C2 | FPU_C0;
    else if (v == 0)
        cc |= FPU_C3;
    else if (tag_of(v) == TAG_SPECIAL)
        cc |= FPU_C3 | FPU_C2;
    else
        cc |= FPU_C2;
    set_cc(f, cc);
}

/* FPREM (IEEE = false: truncating quotient) and FPREM1 on ST(0) by ST(1). */
static void fprem(Fpu *f, bool ieee)
{
    long double a, b;
    if (!read_st(f, 0, &a) || !read_st(f, 1, &b))
        return;
    if (isnan(a) || isnan(b) || isinf(a) || b == 0) {
        if (raise_exc(f, FPU_IE))
            set_st(f, 0, isnan(a) ? a : isnan(b) ? b : indefinite());
        return;
    }
    if (a == 0 || isinf(b)) {
        set_cc(f, 0);
        return;
    }
    int ea, eb;
    frexpl(a, &ea);
    frexpl(b, &eb);
    if (ea - eb >= 64) {
        /* Partial reduction, as the 387 does: remove a multiple of b scaled to 2^(d - 32). */
        long double bs = ldexpl(fabsl(b), ea - eb - 32);
        set_st(f, 0, fmodl(a, bs));
        set_cc(f, FPU_C2);
        return;
    }
    int q;
    long double r = remquol(a, b, &q);
    if (!ieee) {
        r = fmodl(a, b);
        long double qt = truncl((a - r) / b);
        q = (int)fmodl(fabsl(qt), 8.0L);
    } else {
        q = q < 0 ? -q : q;
    }
    set_st(f, 0, r);
    uint16_t cc = 0;
    if (q & 4) cc |= FPU_C0;
    if (q & 2) cc |= FPU_C3;
    if (q & 1) cc |= FPU_C1;
    set_cc(f, cc);
}

/* Operand range of FSIN, FCOS, FSINCOS and FPTAN: |x| < 2^63, else C2 = 1 and no change. */
static bool trig_in_range(Fpu *f, long double x)
{
    if (isfinite(x) && fabsl(x) < 9223372036854775808.0L) {
        f->sw &= (uint16_t)~FPU_C2;
        return true;
    }
    if (isinf(x)) {
        if (raise_exc(f, FPU_IE))
            set_st(f, 0, indefinite());
        return false;
    }
    if (isnan(x))
        return false;
    f->sw |= FPU_C2;
    return false;
}

static void d9_reg(Machine *m, Fpu *f, uint8_t modrm)
{
    int i = modrm & 7;
    long double a, b;
    static const long double consts[7] = {
        1.0L,
        3.321928094887362347870319429489390175864831393L, /* log2(10) */
        1.442695040888963407359924681001892137426645954L, /* log2(e) */
        3.141592653589793238462643383279502884197169399L, /* pi */
        0.301029995663981195213738894724493026768189881L, /* log10(2) */
        0.693147180559945309417232121458176568075500134L, /* ln(2) */
        0.0L,
    };
    switch (modrm) {
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: case 0xC6: case 0xC7:
        if (read_st(f, i, &a))
            push(f, a);
        return;
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        if (is_empty(f, 0) || is_empty(f, i)) {
            if (!stack_fault(f, false))
                return;
            if (is_empty(f, 0))
                set_st(f, 0, indefinite());
            if (is_empty(f, i))
                set_st(f, i, indefinite());
        }
        a = get_st(f, 0);
        set_st(f, 0, get_st(f, i));
        set_st(f, i, a);
        f->sw &= (uint16_t)~FPU_C1;
        return;
    case 0xD0: return; /* FNOP */
    case 0xE0: if (read_st(f, 0, &a)) set_st(f, 0, -a); return;
    case 0xE1: if (read_st(f, 0, &a)) set_st(f, 0, fabsl(a)); return;
    case 0xE4: if (read_st(f, 0, &a)) compare(f, a, 0, false); return;
    case 0xE5: fxam(f); return;
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xED: case 0xEE:
        push(f, r64(consts[modrm - 0xE8]));
        return;
    case 0xF0: /* F2XM1 */
        if (read_st(f, 0, &a))
            set_st(f, 0, r64(expm1l(a * consts[5])));
        return;
    case 0xF1: /* FYL2X */
    case 0xF9: /* FYL2XP1 */
        if (!read_st(f, 0, &a) || !read_st(f, 1, &b))
            return;
        if (modrm == 0xF1 ? a < 0 : a < -1) {
            if (!raise_exc(f, FPU_IE))
                return;
            set_st(f, 1, indefinite());
        } else if (modrm == 0xF1 && a == 0 && !isnan(b)) {
            if (!raise_exc(f, FPU_ZE))
                return;
            set_st(f, 1, signbit(b) ? INFINITY : -INFINITY);
        } else {
            set_st(f, 1, r64(b * (modrm == 0xF1 ? log2l(a) : log1pl(a) * consts[2])));
        }
        pop(f);
        return;
    case 0xF2: /* FPTAN */
        if (!read_st(f, 0, &a) || !trig_in_range(f, a))
            return;
        set_st(f, 0, r64(tanl(a)));
        push(f, 1.0L);
        return;
    case 0xF3: /* FPATAN */
        if (!read_st(f, 0, &a) || !read_st(f, 1, &b))
            return;
        set_st(f, 1, r64(atan2l(b, a)));
        pop(f);
        return;
    case 0xF4: { /* FXTRACT */
        if (!read_st(f, 0, &a))
            return;
        if (a == 0) {
            if (!raise_exc(f, FPU_ZE))
                return;
            set_st(f, 0, -INFINITY);
            push(f, a);
            return;
        }
        if (!isfinite(a)) {
            set_st(f, 0, isinf(a) ? INFINITY : a);
            push(f, a);
            return;
        }
        int e;
        long double fr = frexpl(a, &e);
        set_st(f, 0, (long double)(e - 1));
        push(f, fr * 2);
        return;
    }
    case 0xF5: fprem(f, true); return;
    case 0xF6: set_top(f, top(f) - 1); f->sw &= (uint16_t)~FPU_C1; return;
    case 0xF7: set_top(f, top(f) + 1); f->sw &= (uint16_t)~FPU_C1; return;
    case 0xF8: fprem(f, false); return;
    case 0xFA: /* FSQRT */
        if (!read_st(f, 0, &a))
            return;
        if (a < 0) {
            if (raise_exc(f, FPU_IE))
                set_st(f, 0, indefinite());
            return;
        }
        set_st(f, 0, round_pc(f, sqrtl(a)));
        return;
    case 0xFB: /* FSINCOS */
        if (!read_st(f, 0, &a) || !trig_in_range(f, a))
            return;
        set_st(f, 0, r64(sinl(a)));
        push(f, r64(cosl(a)));
        return;
    case 0xFC:
        if (read_st(f, 0, &a))
            set_st(f, 0, round_int(a, rc_of(f)));
        return;
    case 0xFD: /* FSCALE */
        if (!read_st(f, 0, &a) || !read_st(f, 1, &b))
            return;
        if (isfinite(b)) {
            long double n = truncl(b);
            n = n > 40000 ? 40000 : n < -40000 ? -40000 : n;
            set_st(f, 0, r64(ldexpl(a, (int)n)));
        } else if (!isnan(b)) {
            set_st(f, 0, signbit(b) ? (isinf(a) ? indefinite() : copysignl(0, a))
                                    : (a == 0 ? indefinite() : copysignl(INFINITY, a)));
        }
        return;
    case 0xFE:
    case 0xFF:
        if (!read_st(f, 0, &a) || !trig_in_range(f, a))
            return;
        set_st(f, 0, r64(modrm == 0xFE ? sinl(a) : cosl(a)));
        return;
    default:
        if (modrm >= 0xD8 && modrm <= 0xDF) { /* FSTP1: undocumented alias of FSTP ST(i) */
            if (read_st(f, 0, &a)) {
                set_st(f, i, a);
                pop(f);
            }
            return;
        }
        log_msg("fpu: unimplemented D9 %02X at %04X:%04X", modrm, m->cpu.seg[S_CS], m->cpu.eip & 0xFFFF);
        return;
    }
}

/* ---- environment ---- */

static uint16_t tag_word(const Fpu *f)
{
    uint16_t tw = 0;
    for (int p = 0; p < 8; p++)
        tw |= (uint16_t)((f->empty >> p & 1 ? TAG_EMPTY : tag_of(f->st[p])) << (2 * p));
    return tw;
}

static int env_size(const FpuInsn *in) { return in->op32 ? 28 : 14; }

static void store_env(Machine *m, Fpu *f, const FpuInsn *in)
{
    uint32_t a = in->addr;
    uint32_t ip = ((uint32_t)f->fcs << 4) + f->fip, dp = ((uint32_t)f->fds << 4) + f->fdp;
    if (in->op32) {
        uint32_t v[7] = {f->cw, f->sw, tag_word(f), ip & 0xFFFF, (ip >> 16) << 12 | (f->fop & 0x7FF),
                         dp & 0xFFFF, (dp >> 16) << 12};
        for (int i = 0; i < 7; i++)
            write_le(m, a + 4u * (uint32_t)i, v[i] | 0xFFFF0000u * (i < 3), 4);
    } else {
        uint16_t v[7] = {f->cw, f->sw, tag_word(f), (uint16_t)ip,
                         (uint16_t)((ip >> 16) << 12 | (f->fop & 0x7FF)), (uint16_t)dp,
                         (uint16_t)((dp >> 16) << 12)};
        for (int i = 0; i < 7; i++)
            write_le(m, a + 2u * (uint32_t)i, v[i], 2);
    }
}

static void load_env(Machine *m, Fpu *f, const FpuInsn *in)
{
    uint32_t a = in->addr;
    int step = in->op32 ? 4 : 2;
    f->cw = (uint16_t)read_le(m, a, 2);
    f->sw = (uint16_t)read_le(m, a + (uint32_t)step, 2);
    uint16_t tw = (uint16_t)read_le(m, a + 2u * (uint32_t)step, 2);
    f->empty = 0;
    for (int p = 0; p < 8; p++)
        if ((tw >> (2 * p) & 3) == TAG_EMPTY)
            f->empty |= (uint8_t)(1u << p);
    uint32_t ip_lo = (uint32_t)read_le(m, a + 3u * (uint32_t)step, 2);
    uint32_t ip_hi = (uint32_t)read_le(m, a + 4u * (uint32_t)step, step);
    uint32_t dp_lo = (uint32_t)read_le(m, a + 5u * (uint32_t)step, 2);
    uint32_t dp_hi = (uint32_t)read_le(m, a + 6u * (uint32_t)step, step);
    uint32_t ip = ip_lo | (ip_hi >> 12) << 16, dp = dp_lo | (dp_hi >> 12) << 16;
    f->fop = (uint16_t)(ip_hi & 0x7FF);
    f->fcs = (uint16_t)(ip >> 4), f->fip = ip & 0xF;
    f->fds = (uint16_t)(dp >> 4), f->fdp = dp & 0xF;
    update_es(f);
}

/* ---- dispatch ---- */

static void arith_mem(Machine *m, Fpu *f, const FpuInsn *in, int op)
{
    bool is_int = in->opcode == 0xDA || in->opcode == 0xDE;
    int size = in->opcode == 0xD8 ? 4 : in->opcode == 0xDA ? 4 : in->opcode == 0xDC ? 8 : 2;
    long double b = is_int ? load_int(m, in->addr, size) : load_real(m, in->addr, size), a, r;
    if (!read_st(f, 0, &a))
        return;
    if (op == 2 || op == 3) {
        if (compare(f, a, b, false) && op == 3)
            pop(f);
        return;
    }
    if (arith(f, op, a, b, &r))
        set_st(f, 0, r);
}

/* D8 and DC/DE register forms. dest_st0: D8 (ST(0) op= ST(i)); otherwise ST(i) op= ST(0). */
static void arith_reg(Fpu *f, int op, int i, bool dest_st0, bool pop_after)
{
    long double st0, sti, r;
    if (!read_st(f, 0, &st0) || !read_st(f, i, &sti))
        return;
    if (op == 2 || op == 3) {
        if (compare(f, st0, sti, false) && op == 3)
            pop(f);
        return;
    }
    if (dest_st0) {
        if (arith(f, op, st0, sti, &r))
            set_st(f, 0, r);
    } else {
        if (op >= 4)
            op ^= 1; /* DC/DE swap the direction of SUB/SUBR and DIV/DIVR */
        if (!arith(f, op, sti, st0, &r))
            return;
        set_st(f, i, r);
        if (pop_after)
            pop(f);
    }
}

static void store_and_pop(Fpu *f, bool ok, bool do_pop)
{
    if (ok && do_pop)
        pop(f);
}

/* Control instructions that do not update the last-instruction pointers. */
static bool is_control(const FpuInsn *in)
{
    int reg = in->modrm >> 3 & 7;
    if (in->opcode == 0xD9 && !in->is_reg && reg >= 4)
        return true; /* FLDENV FLDCW FSTENV FSTCW */
    if (in->opcode == 0xDB && in->is_reg && in->modrm >= 0xE0 && in->modrm <= 0xE4)
        return true; /* FENI FDISI FCLEX FINIT FSETPM */
    if (in->opcode == 0xDD && !in->is_reg && (reg == 4 || reg == 6 || reg == 7))
        return true; /* FRSTOR FSAVE FSTSW */
    return in->opcode == 0xDF && in->modrm == 0xE0; /* FSTSW AX */
}

/* A waiting instruction with an unmasked exception pending signals FERR# (IRQ 13 on an AT). */
static bool signal_pending(Machine *m, Fpu *f)
{
    if (!(f->sw & FPU_ES))
        return false;
    pic_raise_irq(m, 13);
    pic_lower_irq(m, 13);
    return true;
}

void fpu_wait(Machine *m)
{
    signal_pending(m, fpu_of(m));
}

void fpu_exec(Machine *m, const FpuInsn *in)
{
    Fpu *f = fpu_of(m);
    int reg = in->modrm >> 3 & 7, i = in->modrm & 7;
    bool control = is_control(in);
    if (!control) {
        signal_pending(m, f);
        f->fop = (uint16_t)((in->opcode & 7) << 8 | in->modrm);
        f->fcs = in->cs;
        f->fip = in->ip;
        if (!in->is_reg) {
            f->fds = in->seg;
            f->fdp = in->off;
        }
    }
    long double a;
    switch (in->opcode) {
    case 0xD8:
        if (in->is_reg)
            arith_reg(f, reg, i, true, false);
        else
            arith_mem(m, f, in, reg);
        break;
    case 0xD9:
        if (in->is_reg) {
            d9_reg(m, f, in->modrm);
            break;
        }
        switch (reg) {
        case 0: push(f, load_real(m, in->addr, 4)); break;
        case 2: case 3:
            if (read_st(f, 0, &a))
                store_and_pop(f, store_real(m, f, in->addr, 4, a), reg == 3);
            break;
        case 4: load_env(m, f, in); break;
        case 5: f->cw = (uint16_t)read_le(m, in->addr, 2); update_es(f); break;
        case 6: store_env(m, f, in); f->cw |= EXC_MASK; update_es(f); break;
        case 7: write_le(m, in->addr, f->cw, 2); break;
        default: goto unimplemented;
        }
        break;
    case 0xDA:
    case 0xDE:
        if (!in->is_reg) {
            arith_mem(m, f, in, reg);
        } else if (in->opcode == 0xDA && in->modrm == 0xE9) { /* FUCOMPP */
            long double b;
            if (read_st(f, 0, &a) && read_st(f, 1, &b) && compare(f, a, b, true)) {
                pop(f);
                pop(f);
            }
        } else if (in->opcode == 0xDE && in->modrm == 0xD9) { /* FCOMPP */
            long double b;
            if (read_st(f, 0, &a) && read_st(f, 1, &b) && compare(f, a, b, false)) {
                pop(f);
                pop(f);
            }
        } else if (in->opcode == 0xDE && reg != 2 && reg != 3) {
            arith_reg(f, reg, i, false, true);
        } else {
            goto unimplemented;
        }
        break;
    case 0xDB:
        if (in->is_reg) {
            switch (in->modrm) {
            case 0xE0: case 0xE1: case 0xE4: break; /* FENI, FDISI (8087 only), FSETPM: no-ops on a 387 */
            case 0xE2: f->sw &= (uint16_t)~(EXC_MASK | FPU_SF | FPU_ES | FPU_B); break;
            case 0xE3: fpu_init(f); break;
            default: goto unimplemented;
            }
            break;
        }
        switch (reg) {
        case 0: push(f, load_int(m, in->addr, 4)); break;
        case 2: case 3:
            if (read_st(f, 0, &a))
                store_and_pop(f, store_int(m, f, in->addr, 4, a), reg == 3);
            break;
        case 5: push(f, load_real(m, in->addr, 10)); break;
        case 7:
            if (read_st(f, 0, &a))
                store_and_pop(f, store_real(m, f, in->addr, 10, a), true);
            break;
        default: goto unimplemented;
        }
        break;
    case 0xDC:
        if (in->is_reg)
            arith_reg(f, reg, i, reg == 2 || reg == 3, false);
        else
            arith_mem(m, f, in, reg);
        break;
    case 0xDD:
        if (in->is_reg) {
            switch (reg) {
            case 0: f->empty |= (uint8_t)(1u << phys(f, i)); break; /* FFREE */
            case 1: d9_reg(m, f, (uint8_t)(0xC8 | i)); break;       /* FXCH4 alias */
            case 2: case 3:                                         /* FST/FSTP ST(i) */
                if (read_st(f, 0, &a)) {
                    set_st(f, i, a);
                    if (reg == 3)
                        pop(f);
                }
                break;
            case 4: case 5: { /* FUCOM/FUCOMP */
                long double b;
                if (read_st(f, 0, &a) && read_st(f, i, &b) && compare(f, a, b, true) && reg == 5)
                    pop(f);
                break;
            }
            default: goto unimplemented;
            }
            break;
        }
        switch (reg) {
        case 0: push(f, load_real(m, in->addr, 8)); break;
        case 2: case 3:
            if (read_st(f, 0, &a))
                store_and_pop(f, store_real(m, f, in->addr, 8, a), reg == 3);
            break;
        case 4: { /* FRSTOR */
            load_env(m, f, in);
            uint32_t p = in->addr + (uint32_t)env_size(in);
            for (int k = 0; k < 8; k++)
                f->st[phys(f, k)] = load_real(m, p + 10u * (uint32_t)k, 10);
            update_es(f);
            break;
        }
        case 6: { /* FNSAVE */
            store_env(m, f, in);
            uint32_t p = in->addr + (uint32_t)env_size(in);
            for (int k = 0; k < 8; k++)
                store_real(m, f, p + 10u * (uint32_t)k, 10, get_st(f, k));
            fpu_init(f);
            break;
        }
        case 7: write_le(m, in->addr, f->sw, 2); break;
        default: goto unimplemented;
        }
        break;
    case 0xDF:
        if (in->is_reg) {
            if (in->modrm == 0xE0) {
                m->cpu.r[R_AX].w = f->sw;
            } else if (reg == 0) { /* FFREEP (undocumented) */
                f->empty |= (uint8_t)(1u << phys(f, i));
                pop(f);
            } else if (reg == 1) {
                d9_reg(m, f, (uint8_t)(0xC8 | i)); /* FXCH7 alias */
            } else if (reg == 2 || reg == 3) {     /* FSTP8/FSTP9 aliases */
                if (read_st(f, 0, &a)) {
                    set_st(f, i, a);
                    pop(f);
                }
            } else {
                goto unimplemented;
            }
            break;
        }
        switch (reg) {
        case 0: push(f, load_int(m, in->addr, 2)); break;
        case 2: case 3:
            if (read_st(f, 0, &a))
                store_and_pop(f, store_int(m, f, in->addr, 2, a), reg == 3);
            break;
        case 4: push(f, load_bcd(m, in->addr)); break;
        case 5: push(f, load_int(m, in->addr, 8)); break;
        case 6:
            if (read_st(f, 0, &a))
                store_and_pop(f, store_bcd(m, f, in->addr, a), true);
            break;
        case 7:
            if (read_st(f, 0, &a))
                store_and_pop(f, store_int(m, f, in->addr, 8, a), true);
            break;
        default: goto unimplemented;
        }
        break;
    }
    return;
unimplemented:
    log_msg("fpu: unimplemented %02X %02X at %04X:%04X", in->opcode, in->modrm, in->cs, in->ip & 0xFFFF);
}
