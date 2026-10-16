/* Host input: native scancodes one IRQ at a time, mouse events at a safe point, emulated path unchanged. */
#include <stdio.h>
#include <string.h>

#include "devices.h"
#include "host_input.h"
#include "lift.h"
#include "native.h"
#include "native_internal.h"

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

enum { CODE_SEG = 0x1000, STACK_SEG = 0x2000, STACK_TOP = 0x800, HANDLER_OFF = 0x20, IMAGE = 0x40 };

static Machine *native_bios(void)
{
    Machine *m = machine_create();
    bios_init(m);
    NativeConfig cfg = { .deterministic = true, .strict = true, .call_cost_ns = 1000, .yield_every_us = 4000 };
    native_init(m, &cfg);
    native_set_platform(m, NULL);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    SP = STACK_TOP;
    m->cpu.flags = 0x0202;
    native_set_active(m, true);
    native_pic_set_mask(m, 0, 0xFD); /* IRQ 1 only; the timer must not nest into these checks */
    return m;
}

static void shutdown(Machine *m)
{
    bios_shutdown(m);
    machine_destroy(m);
}

/* ---- keyboard queue ---- */

static uint8_t got[8];
static int got_n;
static bool same_byte, do_eoi;

static CallbackResult kbd_irq(Machine *m)
{
    uint8_t b = host_input_kbd_data(m);
    uint8_t again = host_input_kbd_data(m);
    same_byte = b == again;
    if (got_n < (int)sizeof got)
        got[got_n++] = b;
    if (do_eoi)
        native_pic_eoi(m, 0, -1);
    return CB_DONE;
}

static void test_key_order(void)
{
    Machine *m = native_bios();
    got_n = 0;
    do_eoi = true;
    callback_register(m, "host_kbd", kbd_irq, 9);

    native_safepoint(m);
    CHECK(got_n == 0, "an empty queue raises nothing");

    host_input_key(m, 0xE048, true);
    host_input_key(m, 0xE048, false);
    CHECK(!(native_state(m)->pic[0].irr & 0x02), "the byte stays queued until a safe point latches it");
    native_safepoint(m);
    native_safepoint(m);
    native_safepoint(m);
    native_safepoint(m);
    CHECK(got_n == 4 && got[0] == 0xE0 && got[1] == 0x48 && got[2] == 0xE0 && got[3] == 0xC8,
          "grey Up down/up is E0 48 E0 C8, got %d bytes %02X %02X %02X %02X", got_n, got[0], got[1], got[2], got[3]);
    CHECK(same_byte, "a second read before EOI returns the same byte");

    host_input_key(m, 0x1E, true);
    host_input_key(m, 0x1E, false);
    native_safepoint(m);
    native_safepoint(m);
    CHECK(got_n == 6 && got[4] == 0x1E && got[5] == 0x9E, "make/break of 1E is one byte each (%02X %02X)", got[4], got[5]);
    shutdown(m);
}

static void test_held_until_eoi(void)
{
    Machine *m = native_bios();
    got_n = 0;
    do_eoi = false;
    callback_register(m, "host_kbd_hold", kbd_irq, 9);
    host_input_key(m, 0xE048, true);
    native_safepoint(m);
    CHECK(got_n == 1 && got[0] == 0xE0 && (native_state(m)->pic[0].isr & 0x02),
          "first byte delivered, IRQ 1 still in service (n=%d isr=%02X)", got_n, native_state(m)->pic[0].isr);
    native_safepoint(m);
    CHECK(got_n == 1, "no EOI, so the next byte stays queued (n=%d)", got_n);
    native_pic_eoi(m, 0, -1);
    CHECK(got_n == 1 && !(native_state(m)->pic[0].isr & 0x02), "EOI alone does not latch the next byte");
    native_safepoint(m);
    CHECK(got_n == 2 && got[1] == 0x48, "the safe point after EOI delivers 48 (n=%d got %02X)", got_n, got[1]);
    shutdown(m);
}

static void test_bios_translation(void)
{
    Machine *m = native_bios();
    uint16_t key = 0;
    host_input_key(m, 0xE048, true);
    CHECK(!bios_kbd_peek(m, &key), "queued, not yet in the BIOS buffer");
    native_safepoint(m);
    CHECK(!bios_kbd_peek(m, &key), "the E0 prefix is not a key");
    native_safepoint(m);
    CHECK(bios_kbd_peek(m, &key) && key == 0x48E0, "grey Up -> %04X", key);
    bios_kbd_pop(m, &key);

    host_input_key(m, 0x1E, true);
    native_safepoint(m);
    CHECK(bios_kbd_peek(m, &key) && key == 0x1E61, "'a' -> %04X", key);
    bios_kbd_pop(m, &key);

    host_input_key(m, 0x1E, true);
    host_input_key(m, 0x1E, false);
    native_safepoint(m);
    native_safepoint(m);
    CHECK(bios_kbd_peek(m, &key) && key == 0x1E61, "down+up leaves the make (%04X)", key);
    bios_kbd_pop(m, &key);
    CHECK(!bios_kbd_peek(m, &key), "the break adds nothing");
    shutdown(m);
}

/* ---- mouse ---- */

static int mouse_calls;
static uint16_t seen_ax, seen_bx, seen_cx, seen_dx, seen_si, seen_di, seen_bp, seen_ds, seen_es, seen_sp, seen_flags;
static LiftedEntry mouse_entry;

static void on_mouse(Machine *m)
{
    mouse_calls++;
    seen_ax = AX;
    seen_bx = BX;
    seen_cx = CX;
    seen_dx = DX;
    seen_si = SI;
    seen_di = DI;
    seen_bp = BP;
    seen_ds = DS;
    seen_es = ES;
    seen_sp = SP;
    seen_flags = (uint16_t)m->cpu.flags;
    AX = BX = CX = DX = SI = DI = BP = 0xBEEF;
    cpu_set_seg(&m->cpu, S_DS, 0x0BAD);
    cpu_set_seg(&m->cpu, S_ES, 0x0BAD);
    m->cpu.flags |= F_CF;
    lift_ret_far(m, 0);
}

static void expect_caller(Machine *m, const char *what)
{
    CHECK(AX == 0xA001 && BX == 0xA002 && CX == 0xA003 && DX == 0xA004 && SI == 0xA005 && DI == 0xA006 &&
              BP == 0xA007 && DS == 0xA008 && ES == 0xA009 && SP == STACK_TOP && m->cpu.flags == 0x0202 &&
              m->cpu.seg[S_CS] == 0x3000 && (m->cpu.eip & 0xFFFF) == 0x42,
          "%s: caller restored AX=%04X SP=%04X FL=%04X CS:IP=%04X:%04X DS=%04X", what, AX, SP,
          (unsigned)m->cpu.flags, m->cpu.seg[S_CS], (unsigned)(m->cpu.eip & 0xFFFF), DS);
}

static void test_mouse_native(void)
{
    Machine *m = native_bios();
    native_pic_set_mask(m, 0, 0xFF);
    static uint8_t image[IMAGE];
    memset(image, 0x90, sizeof image);
    /* Not a bare RETF: that is a trivial return and never reaches the lifted body. */
    static const uint8_t body[] = { 0xB8, 0x11, 0x11, 0xCB };
    memcpy(image + HANDLER_OFF, body, sizeof body);
    memcpy(m->mem + real_addr(CODE_SEG, 0), image, sizeof image);
    mouse_entry = (LiftedEntry){ .module = "TEST.EXE", .off = HANDLER_OFF, .fn = on_mouse, .name = "mouse",
                                  .scratch_regs = LIFT_REG_FLAGS, .no_shadow = true };
    memcpy(mouse_entry.signature, image + HANDLER_OFF, LIFT_SIGNATURE_LEN);
    lift_add_table(m, &mouse_entry, 1);
    lift_module_loaded(m, "TEST.EXE", CODE_SEG, sizeof image);

    AX = 0x000C;
    CX = 0x0007;
    DX = HANDLER_OFF;
    cpu_set_seg(&m->cpu, S_ES, CODE_SEG);
    guest_int(m, 0x33);

    AX = 0xA001;
    BX = 0xA002;
    CX = 0xA003;
    DX = 0xA004;
    SI = 0xA005;
    DI = 0xA006;
    BP = 0xA007;
    cpu_set_seg(&m->cpu, S_DS, 0xA008);
    cpu_set_seg(&m->cpu, S_ES, 0xA009);
    cpu_set_seg(&m->cpu, S_CS, 0x3000);
    m->cpu.eip = 0x42;
    m->cpu.flags = 0x0202;
    mouse_calls = 0;
    seen_ax = 0xFFFF;

    host_input_mouse_motion(m, 100, 50, 320, 200);
    host_input_mouse_button(m, 0, true);
    CHECK(mouse_queued(m) == 2, "both events wait for a safe point (%d)", mouse_queued(m));

    host_input_poll(m);
    CHECK(mouse_calls == 1 && mouse_queued(m) == 1, "one event per poll (calls=%d queued=%d)", mouse_calls,
          mouse_queued(m));
    CHECK(seen_ax == 1 && seen_bx == 0 && seen_cx == 200 && seen_dx == 50 && seen_si == 0xFF88 && seen_di == 0xFF9C,
          "move regs AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X", seen_ax, seen_bx, seen_cx, seen_dx, seen_si,
          seen_di);
    CHECK(seen_ds == 0xA008 && seen_es == 0xA009 && seen_bp == 0xA007 && seen_sp == STACK_TOP - 24 &&
              seen_flags == 0x0002,
          "handler saw caller DS/ES and the trampoline frame (DS=%04X SP=%04X FL=%04X)", seen_ds, seen_sp, seen_flags);
    expect_caller(m, "after move");

    host_input_poll(m);
    CHECK(mouse_calls == 2 && mouse_queued(m) == 0, "press delivered next (calls=%d queued=%d)", mouse_calls,
          mouse_queued(m));
    CHECK(seen_ax == 2 && seen_bx == 1 && seen_cx == 200 && seen_dx == 50 && seen_si == 0xFF88 && seen_di == 0xFF9C,
          "press regs AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X", seen_ax, seen_bx, seen_cx, seen_dx, seen_si,
          seen_di);
    expect_caller(m, "after press");
    shutdown(m);
}

/* ---- emulated path ---- */

enum { HY_CODE = 0x0700, HY_STACK = 0x0600, HY_TOP = 0x0FFE, HY_RESULT = 0x500 };

static void load_spin(Machine *m, const uint8_t *code, size_t len)
{
    memcpy(m->mem + real_addr(HY_CODE, 0), code, len);
    cpu_set_seg(&m->cpu, S_CS, HY_CODE);
    cpu_set_seg(&m->cpu, S_SS, HY_STACK);
    cpu_set_seg(&m->cpu, S_DS, 0);
    m->cpu.eip = 0;
    SP = HY_TOP;
    m->cpu.flags = 0x0202;
}

static void test_hybrid(void)
{
    static const uint8_t SPIN[] = { 0xEB, 0xFE };
    Machine *m = machine_create();
    bios_init(m);
    load_spin(m, SPIN, sizeof SPIN);
    host_input_key(m, 0x1E, true);
    host_input_key(m, 0x1E, false);
    host_input_poll(m);
    CHECK(!(native_state(m)->pic[0].irr & 0x02) && !(native_state(m)->pic[0].isr & 0x02),
          "emulated mode does not raise native IRQ 1");
    machine_run_for(m, 20000);
    uint16_t key = 0;
    CHECK(bios_kbd_peek(m, &key) && key == 0x1E61, "hybrid 'a' -> %04X", key);

    static const uint8_t CODE[] = {
        0xB8, 0x00, 0x00, 0xCD, 0x33,       /* reset */
        0x0E, 0x07,                         /* push cs; pop es */
        0xB8, 0x0C, 0x00, 0xB9, 0x03, 0x00, /* AX=0Ch, CX=move|left press */
        0xBA, 0x20, 0x00, 0xCD, 0x33,       /* DX=handler; int 33h */
        0xEB, 0xFE,
    };
    static const uint8_t HANDLER[] = { 0xA3, 0x00, 0x05, 0x89, 0x0E, 0x02, 0x05, 0xCB };
    load_spin(m, CODE, sizeof CODE);
    memcpy(m->mem + real_addr(HY_CODE, 0x20), HANDLER, sizeof HANDLER);
    machine_run_for(m, 1000);
    host_input_mouse_motion(m, 100, 50, 320, 200);
    machine_run_for(m, 1000);
    CHECK(mem_rd16(m, HY_RESULT) == 1 && mem_rd16(m, HY_RESULT + 2) == 200, "hybrid move AX=%X CX=%u",
          mem_rd16(m, HY_RESULT), mem_rd16(m, HY_RESULT + 2));
    host_input_mouse_button(m, 0, true);
    machine_run_for(m, 1000);
    CHECK(mem_rd16(m, HY_RESULT) == 2, "hybrid press AX=%X", mem_rd16(m, HY_RESULT));
    CHECK(m->cpu.seg[S_CS] == HY_CODE, "back in the program");
    shutdown(m);
}

int main(void)
{
    test_key_order();
    test_held_until_eoi();
    test_bios_translation();
    test_mouse_native();
    test_hybrid();
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        printf("test_host_input: all passed\n");
    return failures ? 1 : 0;
}
