/* Device replay for shadow runs (docs/lifting.md, "Device replay"): a fake driver function that waits
 * two BIOS ticks (INT 1Ah), then for a byte its UART ISR stores, and answers it over the serial link. */
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

enum { CODE_SEG = 0x1000, STACK_SEG = 0x2000, W_OFF = 0x40, ISR_OFF = 0x100, RX_FLAG = 0x300, RX_BYTE = 0x301,
       BYTE_AT_US = 150000, THE_BYTE = 'Q' };

/* main: ds = cs, IRQ4 vector -> ISR, sti, call far W, cli, hlt.
 * W: MCR = DTR|RTS|OUT2, IER = rx, unmask IRQ4; wait until two INT 1Ah ticks passed; then until the ISR
 *    set [300h], reading INT 1Ah each pass; send [301h] + 1; retf (BX kept).
 * ISR: [301h] = RBR, [300h] = 1, EOI, iret. */
static const uint8_t MAIN[] = {
    0x8C, 0xC8, 0x8E, 0xD8, 0x31, 0xC0, 0x8E, 0xC0, 0x26, 0xC7, 0x06, 0x30, 0x00, ISR_OFF & 0xFF, ISR_OFF >> 8,
    0x26, 0x8C, 0x0E, 0x32, 0x00, 0xFB, 0x9A, W_OFF, 0x00, CODE_SEG & 0xFF, CODE_SEG >> 8, 0xFA, 0xF4,
};
static const uint8_t W[] = {
    0x53, 0xBA, 0xFC, 0x03, 0xB0, 0x0B, 0xEE, 0xBA, 0xF9, 0x03, 0xB0, 0x01, 0xEE, 0xE4, 0x21, 0x24, 0xEF, 0xE6,
    0x21, 0xB4, 0x00, 0xCD, 0x1A, 0x89, 0xD3, 0xB4, 0x00, 0xCD, 0x1A, 0x29, 0xDA, 0x83, 0xFA, 0x02, 0x72, 0xF5,
    0x80, 0x3E, 0x00, 0x03, 0x00, 0x75, 0x06, 0xB4, 0x00, 0xCD, 0x1A, 0xEB, 0xF3, 0xA0, 0x01, 0x03, 0xFE, 0xC0,
    0xBA, 0xF8, 0x03, 0xEE, 0x5B, 0xCB,
};
static const uint8_t ISR[] = {
    0x50, 0x52, 0xBA, 0xF8, 0x03, 0xEC, 0x2E, 0xA2, 0x01, 0x03, 0x2E, 0xC6, 0x06, 0x00, 0x03, 0x01, 0xB0, 0x20,
    0xE6, 0x20, 0x5A, 0x58, 0xCF,
};

/* The far side of the line: one byte, available from BYTE_AT_US of emulated time on. */
typedef struct Line {
    Machine *m;
    bool given;
    uint8_t sent[16];
    int sent_len;
} Line;

static int line_send(void *ctx, const uint8_t *buf, int len)
{
    Line *l = ctx;
    for (int i = 0; i < len && l->sent_len < (int)sizeof l->sent; i++)
        l->sent[l->sent_len++] = buf[i];
    return len;
}

static int line_recv(void *ctx, uint8_t *buf, int cap)
{
    Line *l = ctx;
    if (l->given || cap < 1 || l->m->virtual_us < BYTE_AT_US)
        return 0;
    l->given = true;
    buf[0] = THE_BYTE;
    return 1;
}

static bool line_connected(void *ctx) { return true; }
static void line_close(void *ctx) {}

/* ---- lifts of W ---- */

static int wait_ticks = 2, reply_add = 1;
/* Variant: the receive loop reads port 21h instead of INT 1Ah, so IRQ4 follows a port read that lifted C
 * makes and is delivered from inside it. W's "mov ah,0 / int 1Ah" there becomes "in al,21h / nop / nop". */
static bool poll_port;
enum { W_POLL_AT = 43 };

static uint16_t read_ticks(Machine *m)
{
    AH = 0;
    guest_int(m, 0x1A);
    return DX;
}

static void lifted_w(Machine *m)
{
    uint16_t bx = BX;
    AL = 0x0B, DX = 0x3FC;
    port_out(m, DX, AL, 1);
    AL = 0x01, DX = 0x3F9;
    port_out(m, DX, AL, 1);
    AL = (uint8_t)(port_in(m, 0x21, 1) & 0xEF);
    port_out(m, 0x21, AL, 1);
    uint16_t start = read_ticks(m);
    BX = start;
    while ((uint16_t)(read_ticks(m) - start) < wait_ticks)
        ;
    DX = (uint16_t)(DX - start);
    while (mem_rd8(m, real_addr(DS, RX_FLAG)) == 0) {
        if (poll_port)
            AL = (uint8_t)port_in(m, 0x21, 1);
        else
            read_ticks(m);
    }
    AL = (uint8_t)(mem_rd8(m, real_addr(DS, RX_BYTE)) + reply_add);
    DX = 0x3F8;
    port_out(m, DX, AL, 1);
    BX = bx;
    lift_ret_far(m, 0);
}

typedef struct Run {
    int mismatches, shadow_calls, sent_len;
    uint8_t sent0, rx_byte;
    uint64_t virtual_us;
} Run;

static LiftedEntry entries[1];

static Run run(bool verify, bool replay, bool no_shadow)
{
    Machine *m = machine_create();
    bios_init(m);
    static Line line;
    line = (Line){ .m = m };
    uart_attach(m, UART_COM1, (SerialLink){ &line, line_send, line_recv, line_connected, line_close });
    uint8_t *code = m->mem + real_addr(CODE_SEG, 0);
    memcpy(code, MAIN, sizeof MAIN);
    memcpy(code + W_OFF, W, sizeof W);
    if (poll_port)
        memcpy(code + W_OFF + W_POLL_AT, (const uint8_t[]){ 0xE4, 0x21, 0x90, 0x90 }, 4);
    memcpy(code + ISR_OFF, ISR, sizeof ISR);
    entries[0] = (LiftedEntry){ .module = "TEST.EXE", .off = W_OFF, .fn = lifted_w, .name = "w",
                                .scratch_regs = LIFT_REG_AX | LIFT_REG_CX | LIFT_REG_DX | LIFT_REG_FLAGS,
                                .replay = replay, .no_shadow = no_shadow };
    memcpy(entries[0].signature, W, LIFT_SIGNATURE_LEN);
    lift_add_table(m, entries, 1);
    if (verify)
        lift_verify_enable(m, 5);
    lift_module_loaded(m, "TEST.EXE", CODE_SEG, 0x200);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    SP = 0x400;
    m->cpu.eip = 0;
    for (int i = 0; i < 200 && !(m->cpu.halted && !(m->cpu.flags & F_IF)); i++)
        machine_run_for(m, 10000);
    LiftState *s = lift_state(m);
    Run r = { lift_verify_mismatches(m), s->tables[s->table_count - 1].stats[0].shadow_calls, line.sent_len,
              line.sent[0], mem_rd8(m, real_addr(CODE_SEG, RX_BYTE)), m->virtual_us };
    CHECK(m->cpu.halted && SP == 0x400, "the program ends after W (halted %d SP %04X)", m->cpu.halted, SP);
    machine_destroy(m);
    return r;
}

/* ---- host input: a function that polls INT 33h until a button is down ---- */

/* P: mov ax,3 / int 33h / test bl,1 / jz P / retf. The input reaches the original mid-call (the yield hook below
 * stands for the input script); the lifted run gets it from the log, at the same poll. */
static const uint8_t P[] = { 0xB8, 0x03, 0x00, 0xCD, 0x33, 0xF6, 0xC3, 0x01, 0x74, 0xF6, 0xCB };
static const uint8_t MAIN_P[] = { 0x9A, W_OFF, 0x00, CODE_SEG & 0xFF, CODE_SEG >> 8, 0xFA, 0xF4 };
enum { PRESS_AT_US = 3000 };
static bool pressed;
static int idle_polls;

static void press_yield(Machine *m)
{
    if (!pressed && m->virtual_us >= PRESS_AT_US) {
        pressed = true;
        mouse_host_button(m, 0, true);
    }
}

static void lifted_p(Machine *m)
{
    do {
        AX = 3;
        guest_int(m, 0x33);
    } while (!(BX & 1));
    lift_ret_far(m, 0);
}

static Run run_input(bool verify)
{
    Machine *m = machine_create();
    bios_init(m);
    pressed = false;
    uint8_t *code = m->mem + real_addr(CODE_SEG, 0);
    memcpy(code, MAIN_P, sizeof MAIN_P);
    memcpy(code + W_OFF, P, sizeof P);
    entries[0] = (LiftedEntry){ .module = "TEST.EXE", .off = W_OFF, .fn = lifted_p, .name = "p",
                                .scratch_regs = LIFT_REG_AX | LIFT_REG_CX | LIFT_REG_DX | LIFT_REG_FLAGS,
                                .replay = true };
    memcpy(entries[0].signature, P, sizeof P);
    lift_add_table(m, entries, 1);
    lift_set_nested_yield(m, press_yield);
    if (verify)
        lift_verify_enable(m, 5);
    lift_module_loaded(m, "TEST.EXE", CODE_SEG, 0x200);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    SP = 0x400;
    m->cpu.eip = 0;
    for (int i = 0; i < 200 && !(m->cpu.halted && !(m->cpu.flags & F_IF)); i++) {
        press_yield(m);
        machine_run_for(m, 1000);
    }
    LiftState *s = lift_state(m);
    Run r = { lift_verify_mismatches(m), s->tables[s->table_count - 1].stats[0].shadow_calls, 0, 0, 0, m->virtual_us };
    CHECK(m->cpu.halted && SP == 0x400, "the polling program ends (halted %d SP %04X)", m->cpu.halted, SP);
    machine_destroy(m);
    (void)idle_polls;
    return r;
}

int main(void)
{
    /* Not verified: the lift waits and answers like the original. */
    Run r = run(false, false, false);
    CHECK(r.sent_len == 1 && r.sent0 == THE_BYTE + 1 && r.rx_byte == THE_BYTE, "lift answers (%d bytes, %02X)",
          r.sent_len, r.sent0);
    CHECK(r.virtual_us >= BYTE_AT_US, "time passed while waiting (%llu us)", (unsigned long long)r.virtual_us);

    /* Device replay: the original runs with ticks and IRQ4, the lift gets the same interrupts. */
    r = run(true, true, false);
    CHECK(r.shadow_calls == 1 && r.mismatches == 0, "replayed shadow run matches (%d calls, %d mismatches)",
          r.shadow_calls, r.mismatches);
    CHECK(r.sent_len == 1 && r.sent0 == THE_BYTE + 1, "the link sees the answer once (%d bytes, %02X)", r.sent_len,
          r.sent0);
    CHECK(r.rx_byte == THE_BYTE, "the ISR's byte is in memory (%02X)", r.rx_byte);

    poll_port = true;
    r = run(true, true, false);
    CHECK(r.shadow_calls == 1 && r.mismatches == 0 && r.sent_len == 1,
          "IRQ4 after a port read of lifted C (%d calls, %d mismatches, %d bytes)", r.shadow_calls, r.mismatches,
          r.sent_len);
    poll_port = false;

    /* A wrong answer is a port write the original did not make. */
    reply_add = 2;
    r = run(true, true, false);
    CHECK(r.mismatches == 1, "a wrong reply is a mismatch (%d)", r.mismatches);
    CHECK(r.sent_len == 1 && r.sent0 == THE_BYTE + 1, "only the original's reply reaches the link (%d, %02X)",
          r.sent_len, r.sent0);
    reply_add = 1;

    /* Waiting a tick longer reads INT 1Ah more often: the events diverge. */
    wait_ticks = 3;
    r = run(true, true, false);
    CHECK(r.mismatches == 1, "a longer wait is a mismatch (%d)", r.mismatches);
    wait_ticks = 2;

    /* INN_VERIFY_REPLAY turns it on for a NOSHADOW entry. */
    setenv("INN_VERIFY_REPLAY", "x,w", 1);
    r = run(true, false, true);
    CHECK(r.shadow_calls == 1 && r.mismatches == 0, "listed NOSHADOW entry replayed (%d calls, %d mismatches)",
          r.shadow_calls, r.mismatches);
    unsetenv("INN_VERIFY_REPLAY");
    r = run(true, false, true);
    CHECK(r.shadow_calls == 0, "NOSHADOW stays unshadowed (%d)", r.shadow_calls);

    /* Host input mid-call: the mouse driver is restored for the lifted run and the press is applied again where the
     * original got it, so the lifted loop ends at the same poll instead of waiting for input it never gets. */
    r = run_input(false);
    CHECK(r.virtual_us >= PRESS_AT_US, "the polling lift sees the press (%llu us)", (unsigned long long)r.virtual_us);
    r = run_input(true);
    CHECK(r.shadow_calls == 1 && r.mismatches == 0, "host input replayed (%d calls, %d mismatches)", r.shadow_calls,
          r.mismatches);

    if (failures)
        fprintf(stderr, "%d failures\n", failures);
    else
        printf("test_replay: ok\n");
    return failures ? 1 : 0;
}
