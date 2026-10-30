/* INN_REPLAY_EXACT (docs/lifting.md, "Device replay"): a fake driver function unmasks the timer IRQ and
 * polls memory a few thousand passes long for its handler's flag, with no event in the loop. With the
 * entry listed, the original takes the interrupt when due and the lifted poll, charging the original's
 * instructions, sees it at the same pass (CX is compared). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devices.h"
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

enum { CODE_SEG = 0x1000, STACK_SEG = 0x2000, P_OFF = 0x40, ISR_OFF = 0x100, FLAG = 0x300, RESULT = 0x302,
       POLLS = 0x4000, PIT_DIVISOR = 0x0400 };

/* main: ds = cs, mask IRQ0, INT 8 -> ISR, PIT channel 0 mode 2 at PIT_DIVISOR, sti, call far P,
 *       [302h] = CX, cli, hlt.
 * P: unmask IRQ0 (the last event), cx = 4000h, then cmp byte [300h],0 / jne / loop; retf.
 * ISR: [300h] = 1, EOI, iret. */
static const uint8_t MAIN[] = {
    0x8C, 0xC8, 0x8E, 0xD8, 0xE4, 0x21, 0x0C, 0x01, 0xE6, 0x21, 0x31, 0xC0, 0x8E, 0xC0, 0x26, 0xC7, 0x06, 0x20,
    0x00, ISR_OFF & 0xFF, ISR_OFF >> 8, 0x26, 0x8C, 0x0E, 0x22, 0x00, 0xB0, 0x34, 0xE6, 0x43, 0xB0,
    PIT_DIVISOR & 0xFF, 0xE6, 0x40, 0xB0, PIT_DIVISOR >> 8, 0xE6, 0x40, 0xFB, 0x9A, P_OFF, 0x00,
    CODE_SEG & 0xFF, CODE_SEG >> 8, 0x89, 0x0E, RESULT & 0xFF, RESULT >> 8, 0xFA, 0xF4,
};
static const uint8_t P[] = {
    0xE4, 0x21, 0x24, 0xFE, 0xE6, 0x21, 0xB9, POLLS & 0xFF, POLLS >> 8, 0x80, 0x3E, FLAG & 0xFF, FLAG >> 8, 0x00,
    0x75, 0x02, 0xE2, 0xF7, 0xCB,
};
static const uint8_t ISR[] = {
    0x50, 0x2E, 0xC6, 0x06, FLAG & 0xFF, FLAG >> 8, 0x01, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF,
};

/* What the lift charges from the OUT up to the first read, and per pass of the loop. */
static uint32_t to_first_read = 2, per_pass = 3;

static void lifted_p(Machine *m)
{
    AL = (uint8_t)(port_in(m, 0x21, 1) & 0xFE);
    port_out(m, 0x21, AL, 1);
    CX = POLLS;
    m->cpu.stall += to_first_read;
    do {
        lift_replay_poll(m);
        if (mem_rd8(m, real_addr(DS, FLAG)))
            break;
        m->cpu.stall += per_pass;
    } while (--CX);
    lift_ret_far(m, 0);
}

typedef struct Run {
    int mismatches, shadow_calls;
    uint16_t cx;
} Run;

static LiftedEntry entries[1];

static Run run(bool exact)
{
    if (exact)
        setenv("INN_REPLAY_EXACT", "x,p", 1);
    else
        unsetenv("INN_REPLAY_EXACT");
    Machine *m = machine_create();
    bios_init(m);
    uint8_t *code = m->mem + real_addr(CODE_SEG, 0);
    memcpy(code, MAIN, sizeof MAIN);
    memcpy(code + P_OFF, P, sizeof P);
    memcpy(code + ISR_OFF, ISR, sizeof ISR);
    entries[0] = (LiftedEntry){ .module = "TEST.EXE", .off = P_OFF, .fn = lifted_p, .name = "p",
                                .scratch_regs = LIFT_REG_AX | LIFT_REG_FLAGS, .replay = true };
    memcpy(entries[0].signature, P, LIFT_SIGNATURE_LEN);
    lift_add_table(m, entries, 1);
    lift_verify_enable(m, 5);
    lift_module_loaded(m, "TEST.EXE", CODE_SEG, 0x200);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    SP = 0x400;
    m->cpu.eip = 0;
    for (int i = 0; i < 200 && !(m->cpu.halted && !(m->cpu.flags & F_IF)); i++)
        machine_run_for(m, 10000);
    LiftState *s = lift_state(m);
    Run r = { lift_verify_mismatches(m), s->tables[s->table_count - 1].stats[0].shadow_calls,
              mem_rd16(m, real_addr(CODE_SEG, RESULT)) };
    CHECK(m->cpu.halted && SP == 0x400, "the program ends after P (halted %d SP %04X)", m->cpu.halted, SP);
    machine_destroy(m);
    unsetenv("INN_REPLAY_EXACT");
    return r;
}

int main(void)
{
    /* Listed: the original sees the tick within the polls, and so does the lift, at the same pass. */
    Run r = run(true);
    CHECK(r.shadow_calls == 1 && r.mismatches == 0, "exact replay matches (%d calls, %d mismatches)", r.shadow_calls,
          r.mismatches);
    CHECK(r.cx != 0 && r.cx < POLLS, "the tick came within the polls (CX %04X)", r.cx);

    /* A lift that charges a pass short polls more often before the tick: CX differs. */
    per_pass = 2;
    r = run(true);
    CHECK(r.mismatches == 1, "a short charge is a mismatch (%d)", r.mismatches);
    per_pass = 3;

    /* Off by one before the first read is caught as well when the tick lands next to a read; at least it
     * must never be taken for a match of a different pass. */
    to_first_read = 2 + 3 * 7;
    r = run(true);
    CHECK(r.mismatches == 1, "a long first charge is a mismatch (%d)", r.mismatches);
    to_first_read = 2;

    /* Not listed: interrupts only follow events (or 100000 instructions without one), so the original
     * polls out without the tick. */
    r = run(false);
    CHECK(r.shadow_calls == 1 && r.mismatches == 0 && r.cx == 0, "gated replay times out alike (%d, %d, CX %04X)",
          r.shadow_calls, r.mismatches, r.cx);

    if (failures)
        fprintf(stderr, "%d failures\n", failures);
    else
        printf("test_replay_exact: ok\n");
    return failures ? 1 : 0;
}
