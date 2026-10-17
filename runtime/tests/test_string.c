/* REP string instructions: random MOVS, STOS, SCAS, CMPS (byte, word, dword; up and down; with
 * segment overrides and 16-bit offsets that wrap) against a per-element model, run in one go and in
 * slices of a few instruction slots; each iteration costs a slot and a sliced REP resumes at its
 * first prefix. */
#include <stdio.h>
#include <stdlib.h>
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

enum { CODE_SEG = 0x0700, SPAN = 0x20000 };

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

typedef struct Case {
    uint8_t code[8];
    int len;
    uint8_t op, rep, size;
    int override; /* segment register for the source, -1 for DS */
    uint16_t ds, es, si, di, cx;
    uint32_t eax;
    bool down;
} Case;

typedef struct Model {
    uint16_t si, di, cx;
    uint32_t flags;
    uint32_t iterations;
} Model;

static uint32_t rd(const uint8_t *mem, uint32_t a, int size)
{
    uint32_t v = 0;
    for (int k = 0; k < size; k++)
        v |= (uint32_t)mem[(a + (uint32_t)k) & 0xFFFFF] << 8 * k;
    return v;
}

static void wr(uint8_t *mem, uint32_t a, int size, uint32_t v)
{
    for (int k = 0; k < size; k++)
        mem[(a + (uint32_t)k) & 0xFFFFF] = (uint8_t)(v >> 8 * k);
}

static uint32_t cmp_flags(uint32_t a, uint32_t b, int size)
{
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : (1u << 8 * size) - 1, sign = 1u << (8 * size - 1);
    a &= mask, b &= mask;
    uint32_t r = (a - b) & mask, f = 0;
    if (a < b) f |= F_CF;
    if (r == 0) f |= F_ZF;
    if (r & sign) f |= F_SF;
    if ((a ^ b) & (a ^ r) & sign) f |= F_OF;
    if ((a ^ b ^ r) & 0x10) f |= F_AF;
    if (!__builtin_parity(r & 0xFF)) f |= F_PF;
    return f;
}

/* What the instruction does, one element at a time, on a flat copy of memory. */
static Model model(const Case *t, uint8_t *mem, uint32_t flags)
{
    Model r = { .si = t->si, .di = t->di, .cx = t->cx, .flags = flags };
    uint16_t src_seg = t->override == S_ES ? t->es : t->override == S_CS ? CODE_SEG : t->ds;
    int step = t->down ? -t->size : t->size;
    while (r.cx) {
        uint32_t s = ((uint32_t)src_seg << 4) + r.si, d = ((uint32_t)t->es << 4) + r.di;
        bool compare = false;
        switch (t->op & 0xFE) {
        case 0xA4: wr(mem, d, t->size, rd(mem, s, t->size)); r.si += step; r.di += step; break;
        case 0xAA: wr(mem, d, t->size, t->eax); r.di += step; break;
        case 0xA6:
            r.flags = (r.flags & ~F_ARITH) | cmp_flags(rd(mem, s, t->size), rd(mem, d, t->size), t->size);
            r.si += step, r.di += step, compare = true;
            break;
        case 0xAE:
            r.flags = (r.flags & ~F_ARITH) | cmp_flags(t->eax, rd(mem, d, t->size), t->size);
            r.di += step, compare = true;
            break;
        }
        r.cx--, r.iterations++;
        bool zf = (r.flags & F_ZF) != 0;
        if (compare && ((t->rep == 0xF3 && !zf) || (t->rep == 0xF2 && zf)))
            break;
    }
    return r;
}

static const uint8_t OPS[] = { 0xA4, 0xA5, 0xA6, 0xA7, 0xAA, 0xAB, 0xAE, 0xAF };

static uint16_t near_edge(void)
{
    switch (rnd() % 4) {
    case 0: return (uint16_t)(0xFFFF - rnd() % 64);
    case 1: return (uint16_t)(rnd() % 64);
    default: return (uint16_t)rnd();
    }
}

static Case make_case(void)
{
    Case t = { .override = -1 };
    t.op = OPS[rnd() % sizeof OPS];
    t.rep = rnd() & 1 ? 0xF3 : 0xF2;
    int wide = (t.op & 1) ? (int)(rnd() % 3) : 0; /* 0: byte or word, 1: word, 2: dword */
    t.size = !(t.op & 1) ? 1 : wide == 2 ? 4 : 2;
    if (rnd() % 4 == 0)
        t.override = rnd() & 1 ? S_ES : S_CS;
    /* Segments keep every element below the VGA window, so the fast path applies when no wrap. */
    t.ds = (uint16_t)(0x1000 + rnd() % 0x6000);
    t.es = rnd() % 3 == 0 ? t.ds : (uint16_t)(0x1000 + rnd() % 0x6000);
    t.si = near_edge(), t.di = near_edge();
    t.cx = (uint16_t)(rnd() % 5 == 0 ? rnd() % 4 : rnd() % 3000);
    t.eax = rnd();
    t.down = rnd() & 1;
    int n = 0;
    if (t.override >= 0 && rnd() & 1)
        t.code[n++] = t.override == S_ES ? 0x26 : 0x2E;
    t.code[n++] = t.rep;
    if (t.override >= 0 && n == 1)
        t.code[n++] = t.override == S_ES ? 0x26 : 0x2E;
    if (t.size == 4)
        t.code[n++] = 0x66;
    t.code[n++] = t.op;
    t.code[n++] = 0xF4; /* HLT */
    t.len = n;
    return t;
}

static void load(Machine *m, const Case *t, const uint8_t *image)
{
    memcpy(m->mem, image, 0xA0000);
    memcpy(m->mem + real_addr(CODE_SEG, 0), t->code, (size_t)t->len);
    cpu_reset(m);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_DS, t->ds);
    cpu_set_seg(&m->cpu, S_ES, t->es);
    cpu_set_seg(&m->cpu, S_SS, 0x0600);
    m->cpu.r[R_SP].w = 0xFFFE;
    m->cpu.r[R_SI].w = t->si, m->cpu.r[R_DI].w = t->di, m->cpu.r[R_CX].w = t->cx;
    m->cpu.r[R_AX].e = t->eax;
    m->cpu.flags = 0x0002 | (t->down ? F_DF : 0) | (rnd() & F_ARITH);
}

static int compare(const char *how, int i, Machine *m, const Case *t, const Model *want, const uint8_t *mem)
{
    int bad = 0;
    if (m->cpu.r[R_SI].w != want->si || m->cpu.r[R_DI].w != want->di || m->cpu.r[R_CX].w != want->cx)
        bad++;
    if ((m->cpu.flags & F_ARITH) != (want->flags & F_ARITH))
        bad++;
    if (memcmp(m->mem, mem, 0xA0000) != 0)
        bad++;
    if (!m->cpu.halted)
        bad++;
    CHECK(!bad, "%s case %d: op %02X rep %02X size %d %s cx %04X si %04X di %04X: got si %04X di %04X cx %04X "
          "flags %03X%s%s, want si %04X di %04X cx %04X flags %03X", how, i, t->op, t->rep, t->size,
          t->down ? "down" : "up", t->cx, t->si, t->di, m->cpu.r[R_SI].w, m->cpu.r[R_DI].w, m->cpu.r[R_CX].w,
          (unsigned)(m->cpu.flags & F_ARITH), memcmp(m->mem, mem, 0xA0000) ? ", memory differs" : "",
          m->cpu.halted ? "" : ", not halted", want->si, want->di, want->cx, (unsigned)(want->flags & F_ARITH));
    return bad;
}

int main(void)
{
    Machine *m = machine_create();
    uint8_t *image = malloc(0xA0000), *expect = malloc(0x100000);
    if (!image || !expect)
        return 1;
    for (uint32_t i = 0; i < 0xA0000; i++)
        image[i] = (uint8_t)(rnd() % 7 == 0 ? 0x41 : rnd()); /* repeats, so compares run on */
    uint64_t total = 0;
    int cases = 0;
    for (int i = 0; i < 3000 && failures < 10; i++, cases++) {
        Case t = make_case();
        load(m, &t, image);
        uint32_t flags = m->cpu.flags;
        memcpy(expect, m->mem, 0x100000);
        Model want = model(&t, expect, flags);
        total += want.iterations;

        /* In one go: one slot per iteration (at least one), then the HLT. */
        uint64_t before = m->cpu.instructions;
        cpu_run(m, 1u << 20);
        compare("whole", i, m, &t, &want, expect);
        uint64_t slots = m->cpu.instructions - before;
        uint64_t want_slots = (want.iterations ? want.iterations : 1) + 1;
        CHECK(slots == want_slots, "case %d: %llu slots, want %llu", i, (unsigned long long)slots,
              (unsigned long long)want_slots);

        /* In slices of 1 to 7 slots, as machine_step hands them out. A sliced REP leaves EIP on its
         * first prefix with CX, SI and DI current. */
        load(m, &t, image);
        m->cpu.flags = flags;
        int guard = 0;
        while (!m->cpu.halted && guard++ < 100000) {
            uint64_t budget = 1 + rnd() % 7;
            cpu_run(m, budget);
            if (!m->cpu.halted && m->cpu.eip != 0 && m->cpu.eip != (uint32_t)t.len - 1)
                CHECK(false, "case %d: stopped inside the instruction at IP %04X", i, m->cpu.eip);
        }
        compare("sliced", i, m, &t, &want, expect);
    }

    /* An interrupt between two slices of a REP returns to its first prefix. */
    {
        Case t = { .op = 0xAA, .rep = 0xF3, .size = 1, .override = -1, .ds = 0x2000, .es = 0x2000, .di = 0x100,
                   .cx = 1000, .eax = 0x5A, .code = { 0xF3, 0xAA, 0xF4 }, .len = 3 };
        load(m, &t, image);
        m->cpu.flags = 0x0002;
        cpu_run(m, 10);
        CHECK(m->cpu.eip == 0 && m->cpu.r[R_CX].w == 990 && m->cpu.r[R_DI].w == 0x10A,
              "after 10 slots: IP %04X CX %u DI %04X", m->cpu.eip, m->cpu.r[R_CX].w, m->cpu.r[R_DI].w);
        cpu_interrupt(m, 0x60); /* the vector holds whatever: only the pushed return address matters */
        uint16_t sp = m->cpu.r[R_SP].w;
        CHECK(mem_rd16(m, real_addr(0x0600, sp)) == 0, "pushed IP %04X, want 0", mem_rd16(m, real_addr(0x0600, sp)));
    }

    machine_destroy(m);
    free(image);
    free(expect);
    if (failures) {
        fprintf(stderr, "test_string: %d failures\n", failures);
        return 1;
    }
    printf("test_string: ok, %d cases, %llu iterations\n", cases, (unsigned long long)total);
    return 0;
}
