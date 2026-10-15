/* Device tests: PIC priority and EOI, PIT rates and latching, keyboard to INT 16h, VGA DAC
 * and memory layouts, UART loopback and link pacing, mouse handler trampoline. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "exec_stub.h"

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            failures++;                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                      \
            fputc('\n', stderr);                               \
        }                                                      \
    } while (0)

enum { CODE_SEG = 0x0700, STACK_SEG = 0x0600, STACK_TOP = 0x0FFE, RESULT = 0x500 };

static Machine *boot(void)
{
    Machine *m = machine_create();
    bios_init(m);
    return m;
}

/* Loads code at CODE_SEG:0000 with a private stack and interrupts enabled. */
static void load(Machine *m, const uint8_t *code, size_t len)
{
    memcpy(m->mem + real_addr(CODE_SEG, 0), code, len);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    cpu_set_seg(&m->cpu, S_DS, 0);
    m->cpu.eip = 0;
    SP = STACK_TOP;
    m->cpu.flags = 0x0202;
}

static void finish(Machine *m)
{
    bios_shutdown(m);
    machine_destroy(m);
}

static void test_pic(void)
{
    Machine *m = boot();
    port_out(m, 0x21, 0x00, 1);
    pic_raise_irq(m, 4);
    pic_raise_irq(m, 1);
    CHECK(m->irq_pending, "irq pending after raise");
    CHECK(pic_acknowledge(m) == 0x09, "IRQ1 beats IRQ4");
    CHECK(!m->irq_pending, "IRQ4 blocked while IRQ1 in service");
    port_out(m, 0x20, 0x0B, 1);
    CHECK(port_in(m, 0x20, 1) == 0x02, "ISR shows IRQ1");
    port_out(m, 0x20, 0x20, 1);
    CHECK(m->irq_pending, "IRQ4 deliverable after EOI");
    CHECK(pic_acknowledge(m) == 0x0C, "then IRQ4");
    port_out(m, 0x20, 0x20, 1);

    pic_raise_irq(m, 1); /* already high: no new edge */
    CHECK(!m->irq_pending, "edge-triggered: no re-trigger while line high");
    pic_lower_irq(m, 1);
    port_out(m, 0x21, 0x02, 1);
    pic_raise_irq(m, 1);
    CHECK(!m->irq_pending, "masked IRQ1 not deliverable");
    port_out(m, 0x21, 0x00, 1);
    CHECK(m->irq_pending, "unmasking delivers");
    pic_acknowledge(m);
    port_out(m, 0x20, 0x20, 1);

    pic_raise_irq(m, 12);
    CHECK(pic_acknowledge(m) == 0x74, "slave IRQ12 vector 74h");
    pic_raise_irq(m, 3);
    CHECK(!m->irq_pending, "IRQ3 below cascade in service");
    port_out(m, 0xA0, 0x20, 1);
    port_out(m, 0x20, 0x20, 1);
    CHECK(pic_acknowledge(m) == 0x0B, "IRQ3 after both EOIs");
    finish(m);
}

static uint32_t bda_ticks(Machine *m) { return mem_rd32(m, 0x46C); }

static void test_pit(void)
{
    static const uint8_t SPIN[] = { 0xEB, 0xFE }; /* jmp $ */
    Machine *m = boot();
    load(m, SPIN, sizeof SPIN);
    uint32_t t0 = bda_ticks(m);
    machine_run_for(m, 1000000);
    uint32_t n = bda_ticks(m) - t0;
    CHECK(n >= 17 && n <= 19, "18.2 Hz default timer, got %u ticks/s", n);

    port_out(m, 0x43, 0x34, 1); /* ch0, lo/hi, mode 2 */
    port_out(m, 0x40, 1193 & 0xFF, 1);
    port_out(m, 0x40, 1193 >> 8, 1);
    t0 = bda_ticks(m);
    machine_run_for(m, 100000);
    n = bda_ticks(m) - t0;
    CHECK(n >= 98 && n <= 102, "1 kHz reprogrammed timer, got %u in 100 ms", n);
    CHECK(pit_timer_hz(m) > 999 && pit_timer_hz(m) < 1001, "pit_timer_hz");

    machine_run_for(m, 300);
    port_out(m, 0x43, 0x00, 1); /* latch ch0 */
    machine_run_for(m, 200);    /* the latched value must not move */
    uint16_t c = (uint16_t)(port_in(m, 0x40, 1) | port_in(m, 0x40, 1) << 8);
    CHECK(c >= 1 && c <= 1193, "latched count in range, got %u", c);

    port_out(m, 0x43, 0xB6, 1); /* ch2 square wave for the speaker */
    port_out(m, 0x42, 0xA9, 1);
    port_out(m, 0x42, 0x04, 1);
    port_out(m, 0x61, 0x03, 1);
    double hz = 0;
    CHECK(pit_speaker(m, &hz) && hz > 1000 && hz < 1002, "speaker at ~1001 Hz, got %f", hz);
    finish(m);
}

/* mov ah,AH; int 16h; mov [500h],ax; jmp $ */
static uint16_t read_key(Machine *m, uint8_t fn, const uint16_t *keys, int nkeys, bool shift)
{
    const uint8_t code[] = { 0xB4, fn, 0xCD, 0x16, 0xA3, 0x00, 0x05, 0xEB, 0xFE };
    load(m, code, sizeof code);
    mem_wr16(m, RESULT, 0xDEAD);
    machine_run_for(m, 2000);
    if (shift)
        kbd_host_key(m, 0x2A, true);
    for (int i = 0; i < nkeys; i++) {
        kbd_host_key(m, keys[i], true);
        kbd_host_key(m, keys[i], false);
    }
    if (shift)
        kbd_host_key(m, 0x2A, false);
    machine_run_for(m, 20000);
    return mem_rd16(m, RESULT);
}

static void test_keyboard(void)
{
    Machine *m = boot();
    const uint16_t a = 0x1E, up = 0xE048, enter_kp = 0xE01C;
    uint16_t k = read_key(m, 0x00, &a, 1, false);
    CHECK(k == 0x1E61, "INT 16h 'a' -> %04X", k);
    k = read_key(m, 0x00, &a, 1, true);
    CHECK(k == 0x1E41, "INT 16h shift+'a' -> %04X", k);
    k = read_key(m, 0x10, &up, 1, false);
    CHECK(k == 0x48E0, "INT 16h AH=10h grey Up -> %04X", k);
    k = read_key(m, 0x00, &up, 1, false);
    CHECK(k == 0x4800, "INT 16h AH=00h grey Up -> %04X", k);
    k = read_key(m, 0x00, &enter_kp, 1, false);
    CHECK(k == 0x1C0D, "INT 16h keypad Enter -> %04X", k);
    uint16_t key;
    CHECK(!bios_kbd_peek(m, &key), "buffer drained");
    CHECK(bios_kbd_push(m, 0x3B00) && bios_kbd_pop(m, &key) && key == 0x3B00, "push/pop");
    finish(m);
}

static void test_vga(void)
{
    Machine *m = boot();
    port_out(m, 0x3C8, 5, 1);
    const uint8_t rgb[6] = { 10, 20, 30, 63, 0, 1 };
    for (int i = 0; i < 6; i++)
        port_out(m, 0x3C9, rgb[i] | 0xC0, 1); /* top bits ignored: the DAC is 6-bit */
    CHECK(port_in(m, 0x3C8, 1) == 7, "write index auto-increments");
    port_out(m, 0x3C7, 5, 1);
    for (int i = 0; i < 6; i++) {
        uint8_t v = (uint8_t)port_in(m, 0x3C9, 1);
        CHECK(v == rgb[i], "DAC read %d: %u != %u", i, v, rgb[i]);
    }
    CHECK(port_in(m, 0x3C7, 1) == 3, "DAC state after read");

    static uint32_t frame[VGA_MAX_W * VGA_MAX_H];
    int w, h;
    vga_render(m, frame, &w, &h);
    CHECK(w == 640 && h == 400, "text mode frame %dx%d", w, h);

    vga_set_mode(m, 0x13, true);
    port_out(m, 0x3C8, 5, 1);
    port_out(m, 0x3C9, 63, 1), port_out(m, 0x3C9, 0, 1), port_out(m, 0x3C9, 0, 1);
    mem_wr8(m, 0xA0000 + 321, 5);
    CHECK(mem_rd8(m, 0xA0000 + 321) == 5, "chained readback");
    vga_render(m, frame, &w, &h);
    CHECK(w == 320 && h == 200, "mode 13h frame %dx%d", w, h);
    CHECK(frame[321] == 0xFFFF0000u, "mode 13h pixel (1,1) red, got %08X", frame[321]);

    port_out(m, 0x3C4, 0x0604, 2); /* mode X: chain4 off */
    port_out(m, 0x3C4, 0x0F02, 2);
    for (int i = 0; i < 80; i++)
        mem_wr8(m, 0xA0000 + 80 * 10 + i, 0); /* clear row 10 */
    port_out(m, 0x3C4, 0x0402, 2); /* plane 2 only */
    mem_wr8(m, 0xA0000 + 80 * 10 + 1, 5);
    vga_render(m, frame, &w, &h);
    CHECK(frame[10 * 320 + 6] == 0xFFFF0000u && frame[10 * 320 + 5] != 0xFFFF0000u, "mode X plane write");

    vga_set_mode(m, 0x12, true);
    port_out(m, 0x3CE, 0x0205, 2); /* write mode 2 */
    port_out(m, 0x3CE, 0x8008, 2); /* bit mask: leftmost pixel */
    mem_rd8(m, 0xA0000);           /* load latches */
    mem_wr8(m, 0xA0000, 0x0C);
    vga_render(m, frame, &w, &h);
    CHECK(w == 640 && h == 480, "mode 12h frame %dx%d", w, h);
    CHECK(frame[0] == 0xFFFF5555u && frame[1] == 0xFF000000u, "planar write mode 2, got %08X", frame[0]);
    finish(m);
}

static void run_until(Machine *m, uint64_t us) { machine_run_for(m, us); }

static void test_uart(void)
{
    static const uint8_t SPIN[] = { 0xEB, 0xFE };
    Machine *m = boot();
    load(m, SPIN, sizeof SPIN);
    port_out(m, 0x3FB, 0x83, 1); /* DLAB, 8N1 */
    port_out(m, 0x3F8, 12, 1);   /* 9600 baud */
    port_out(m, 0x3F9, 0, 1);
    port_out(m, 0x3FB, 0x03, 1);
    port_out(m, 0x3FC, 0x10 | 0x0B, 1); /* loopback, OUT2, RTS, DTR */
    CHECK((port_in(m, 0x3FE, 1) & 0xB0) == 0xB0, "loopback MSR mirrors MCR");
    port_out(m, 0x3F8, 'Z', 1);
    CHECK(!(port_in(m, 0x3FD, 1) & 0x01), "no data before a byte time elapses");
    run_until(m, 2000);
    CHECK(port_in(m, 0x3FD, 1) & 0x01, "data ready after loopback");
    CHECK(port_in(m, 0x3F8, 1) == 'Z', "loopback byte");

    port_out(m, 0x3FC, 0x0B, 1);
    CHECK(uart_attach_pipe(m, UART_COM1) == 0, "attach pipe");
    run_until(m, 200);
    CHECK((port_in(m, 0x3FE, 1) & 0x80), "DCD high on a connected link");
    port_out(m, 0x21, 0x00, 1);
    port_out(m, 0x3F9, 0x01, 1); /* IER: rx data */
    uart_test_give(m, UART_COM1, (const uint8_t *)"OK", 2);
    m->cpu.flags &= ~F_IF; /* keep the guest from taking the IRQ so we can inspect it */
    run_until(m, 1100);
    CHECK((port_in(m, 0x3FA, 1) & 0x0F) == 0x04, "IIR rx data");
    CHECK(m->irq_pending && pic_acknowledge(m) == 0x0C, "IRQ4 raised");
    port_out(m, 0x20, 0x20, 1);
    CHECK(port_in(m, 0x3F8, 1) == 'O', "first byte");
    CHECK(!(port_in(m, 0x3FD, 1) & 0x01), "second byte paced at the baud rate");
    run_until(m, 1100);
    CHECK(port_in(m, 0x3F8, 1) == 'K', "second byte");

    port_out(m, 0x3F8, 'A', 1);
    CHECK(port_in(m, 0x3FD, 1) & 0x20, "THRE rises once A moves to the shift register");
    port_out(m, 0x3F8, 'B', 1);
    port_out(m, 0x3F8, 'C', 1); /* FIFO off: holding register overrun drops C */
    run_until(m, 3000);
    uint8_t out[4];
    int n = uart_test_take(m, UART_COM1, out, sizeof out);
    CHECK(n == 2 && out[0] == 'A' && out[1] == 'B', "guest bytes reach the link (n=%d)", n);
    port_out(m, 0x3FA, 0x07, 1); /* FIFO on */
    CHECK((port_in(m, 0x3FA, 1) & 0xC0) == 0xC0, "IIR FIFO bits");
    finish(m);
}

/* Handler: mov [500h],ax; mov [502h],cx; retf. Installer: AX=0Ch, CX=mask, ES:DX. */
static void test_mouse(void)
{
    static const uint8_t CODE[] = {
        0xB8, 0x00, 0x00, 0xCD, 0x33,       /* reset */
        0x0E, 0x07,                         /* push cs; pop es */
        0xB8, 0x0C, 0x00, 0xB9, 0x03, 0x00, /* AX=0Ch, CX=move|left press */
        0xBA, 0x20, 0x00, 0xCD, 0x33,       /* DX=handler at 20h; int 33h */
        0xEB, 0xFE,
    };
    static const uint8_t HANDLER[] = { 0xA3, 0x00, 0x05, 0x89, 0x0E, 0x02, 0x05, 0xCB };
    Machine *m = boot();
    load(m, CODE, sizeof CODE);
    memcpy(m->mem + real_addr(CODE_SEG, 0x20), HANDLER, sizeof HANDLER);
    machine_run_for(m, 1000);
    mouse_host_motion(m, 100, 50, 320, 200);
    machine_run_for(m, 1000);
    CHECK(mem_rd16(m, RESULT) == 1 && mem_rd16(m, RESULT + 2) == 200, "move event: AX=%X CX=%u",
          mem_rd16(m, RESULT), mem_rd16(m, RESULT + 2));
    mouse_host_button(m, 0, true);
    machine_run_for(m, 1000);
    CHECK(mem_rd16(m, RESULT) == 2, "press event AX=%X", mem_rd16(m, RESULT));
    CHECK(!mouse_cursor_visible(m), "cursor hidden after reset");
    CHECK(m->cpu.seg[S_CS] == CODE_SEG, "back in the program");
    finish(m);
}

/* The TSNEXEC stand-in answers INT 14h with its export table; the game port reports no joystick. */
static void test_exec_stub_and_gameport(void)
{
    Machine *m = boot();
    exec_stub_install(m, NULL, NULL);
    exec_stub_set_previous(m, "SierraLand");
    static const uint8_t code[] = {
        0xCD, 0x14,                   /* int 14h: DX:AX = table */
        0x8E, 0xC2, 0x89, 0xC3,       /* mov es,dx; mov bx,ax */
        0x26, 0xFF, 0x1F,             /* call far es:[bx]: GetStatus */
        0xA3, 0x00, 0x05,             /* mov [500h],ax */
        0x26, 0xFF, 0x5F, 0x2C,       /* call far es:[bx+2Ch]: GetPreviousProgram */
        0xA3, 0x02, 0x05,             /* mov [502h],ax */
        0x89, 0x16, 0x04, 0x05,       /* mov [504h],dx */
        0xBA, 0x01, 0x02, 0xEC,       /* mov dx,201h; in al,dx */
        0xA2, 0x06, 0x05,             /* mov [506h],al */
        0xEB, 0xFE,                   /* jmp $ */
    };
    load(m, code, sizeof code);
    machine_run_for(m, 1000);
    CHECK(mem_rd16(m, RESULT) == 0x8003, "GetStatus %04X", mem_rd16(m, RESULT));
    char name[16] = "";
    mem_read_cstr(m, mem_rd16(m, RESULT + 4), mem_rd16(m, RESULT + 2), name, sizeof name);
    CHECK(!strcmp(name, "SierraLand"), "GetPreviousProgram '%s'", name);
    CHECK(m->mem[RESULT + 6] == 0xFF, "game port %02X", m->mem[RESULT + 6]);
    exec_stub_shutdown(m);
    finish(m);
}

int main(void)
{
    test_pic();
    test_pit();
    test_keyboard();
    test_vga();
    test_uart();
    test_mouse();
    test_exec_stub_and_gameport();
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        printf("test_devices: all passed\n");
    return failures ? 1 : 0;
}
