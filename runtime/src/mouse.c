/* INT 33h mouse driver (HLE). User event handlers (AX=0Ch/14h) run from IRQ12: host events
 * queue up, IRQ12 enters a ROM trampoline that saves registers, far-calls the handler with the
 * event in AX/BX/CX/DX/SI/DI, restores, EOIs and IRETs. */
#include <stdlib.h>

#include "bios_internal.h"

enum {
    VEC_MOUSE = 0x33, VEC_MOUSE_IRQ = 0x74,
    MOUSE_BUTTONS = 3, EVENT_QUEUE = 32,
    EV_MOVE = 0x01, EV_PRESS0 = 0x02, EV_RELEASE0 = 0x04,
    VIRTUAL_W = 640, VIRTUAL_H_200 = 200,
    DEFAULT_MICKEY_X = 8, DEFAULT_MICKEY_Y = 16, MICKEY_RATIO_UNIT = 8,
    DEFAULT_SENSITIVITY = 50,
    DRIVER_VERSION = 0x0805, DRIVER_TYPE_PS2 = 4,
    RESET_OK = 0xFFFF,
};

typedef struct MouseEvent { uint16_t mask, buttons, x, y; int16_t mx, my; } MouseEvent;

typedef struct Mouse {
    int x, y;                /* virtual coordinates */
    int min_x, max_x, min_y, max_y;
    int virt_h;              /* 200 or 480 depending on the mode at reset */
    uint16_t buttons;
    uint16_t press_count[MOUSE_BUTTONS], release_count[MOUSE_BUTTONS];
    int press_x[MOUSE_BUTTONS], press_y[MOUSE_BUTTONS], release_x[MOUSE_BUTTONS], release_y[MOUSE_BUTTONS];
    int mickey_x, mickey_y;  /* since the last AX=0Bh */
    int16_t raw_mx, raw_my;  /* running totals passed to handlers in SI/DI */
    int ratio_x, ratio_y, sens_x, sens_y, sens_threshold;
    int show_count;          /* cursor shown when 0 */
    bool enabled;
    uint16_t handler_mask, handler_seg, handler_off;
    MouseEvent queue[EVENT_QUEUE];
    int q_head, q_count;
} Mouse;

static Mouse *mouse_state(Machine *m) { return m->mouse; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void reset_driver(Machine *m)
{
    Mouse *s = mouse_state(m);
    s->virt_h = vga_mode(m) == 0x12 ? 480 : VIRTUAL_H_200;
    s->min_x = s->min_y = 0;
    s->max_x = VIRTUAL_W - 1;
    s->max_y = s->virt_h - 1;
    s->x = VIRTUAL_W / 2;
    s->y = s->virt_h / 2;
    s->show_count = -1;
    s->handler_mask = 0;
    s->ratio_x = DEFAULT_MICKEY_X;
    s->ratio_y = DEFAULT_MICKEY_Y;
    s->sens_x = s->sens_y = s->sens_threshold = DEFAULT_SENSITIVITY;
    s->mickey_x = s->mickey_y = 0;
    s->q_count = 0;
    s->enabled = true;
    for (int b = 0; b < MOUSE_BUTTONS; b++)
        s->press_count[b] = s->release_count[b] = 0;
}

/* ---- events ---- */

static void post_event(Machine *m, uint16_t mask)
{
    Mouse *s = mouse_state(m);
    if (!s->enabled || !(mask & s->handler_mask) || s->q_count == EVENT_QUEUE)
        return;
    s->queue[(s->q_head + s->q_count++) % EVENT_QUEUE] = (MouseEvent){
        mask, s->buttons, (uint16_t)s->x, (uint16_t)s->y, s->raw_mx, s->raw_my };
    pic_lower_irq(m, PIC_IRQ_MOUSE);
    pic_raise_irq(m, PIC_IRQ_MOUSE);
}

void mouse_host_motion(Machine *m, int x, int y, int frame_w, int frame_h)
{
    Mouse *s = mouse_state(m);
    if (frame_w <= 0 || frame_h <= 0)
        return;
    int vx = clampi(x * VIRTUAL_W / frame_w, s->min_x, s->max_x);
    int vy = clampi(y * s->virt_h / frame_h, s->min_y, s->max_y);
    if (vx == s->x && vy == s->y)
        return;
    int mx = (vx - s->x) * s->ratio_x / MICKEY_RATIO_UNIT;
    int my = (vy - s->y) * s->ratio_y / MICKEY_RATIO_UNIT;
    s->mickey_x += mx;
    s->mickey_y += my;
    s->raw_mx = (int16_t)(s->raw_mx + mx);
    s->raw_my = (int16_t)(s->raw_my + my);
    s->x = vx;
    s->y = vy;
    post_event(m, EV_MOVE);
}

void mouse_host_button(Machine *m, int button, bool down)
{
    Mouse *s = mouse_state(m);
    if (button < 0 || button >= MOUSE_BUTTONS || (bool)(s->buttons >> button & 1) == down)
        return;
    s->buttons = down ? (uint16_t)(s->buttons | 1u << button) : (uint16_t)(s->buttons & ~(1u << button));
    if (down) {
        s->press_count[button]++;
        s->press_x[button] = s->x, s->press_y[button] = s->y;
    } else {
        s->release_count[button]++;
        s->release_x[button] = s->x, s->release_y[button] = s->y;
    }
    post_event(m, (uint16_t)((down ? EV_PRESS0 : EV_RELEASE0) << (button * 2)));
}

bool mouse_cursor_visible(Machine *m) { return mouse_state(m)->show_count >= 0; }

void mouse_cursor_pos(Machine *m, int frame_w, int frame_h, int *x, int *y)
{
    Mouse *s = mouse_state(m);
    *x = s->x * frame_w / VIRTUAL_W;
    *y = s->y * frame_h / s->virt_h;
}

/* ---- IRQ12 trampoline ---- */

/* Loads the next event into the registers and far-calls the handler, returning just past us. */
static CallbackResult mouse_dispatch(Machine *m)
{
    Mouse *s = mouse_state(m);
    if (!s->q_count || !s->handler_mask)
        return CB_DONE;
    MouseEvent e = s->queue[s->q_head];
    s->q_head = (s->q_head + 1) % EVENT_QUEUE;
    s->q_count--;
    AX = e.mask & s->handler_mask, BX = e.buttons, CX = e.x, DX = e.y;
    SI = (uint16_t)e.mx, DI = (uint16_t)e.my;
    cpu_push16(m, CS);
    cpu_push16(m, (uint16_t)m->cpu.eip);
    cpu_set_seg(&m->cpu, S_CS, s->handler_seg);
    m->cpu.eip = s->handler_off;
    return CB_DONE;
}

/* Back from the handler: more queued events mean another IRQ12 after this one's EOI. */
static CallbackResult mouse_after(Machine *m)
{
    if (mouse_state(m)->q_count) {
        pic_lower_irq(m, PIC_IRQ_MOUSE);
        pic_raise_irq(m, PIC_IRQ_MOUSE);
    }
    return CB_DONE;
}

static void install_trampoline(Machine *m)
{
    static const uint8_t SAVE[] = { 0x60, 0x1E, 0x06 };                 /* pusha; push ds; push es */
    static const uint8_t RESTORE[] = { 0x07, 0x1F, 0x61, 0x50, 0xB0, PIC_EOI_CMD, 0xE6, 0xA0, 0xE6, 0x20,
                                       0x58, 0xCF };                     /* pops, EOI both, iret */
    uint16_t entry = bios_rom_emit(m, SAVE, sizeof SAVE);
    bios_rom_callback(m, "mouse_dispatch", mouse_dispatch);
    bios_rom_callback(m, "mouse_after", mouse_after);
    bios_rom_emit(m, RESTORE, sizeof RESTORE);
    bios_set_vector(m, VEC_MOUSE_IRQ, BIOS_SEG, entry);
}

/* ---- INT 33h ---- */

static void button_info(Machine *m, bool press)
{
    Mouse *s = mouse_state(m);
    int b = BX < MOUSE_BUTTONS ? BX : 0;
    AX = s->buttons;
    BX = press ? s->press_count[b] : s->release_count[b];
    CX = (uint16_t)(press ? s->press_x[b] : s->release_x[b]);
    DX = (uint16_t)(press ? s->press_y[b] : s->release_y[b]);
    if (press)
        s->press_count[b] = 0;
    else
        s->release_count[b] = 0;
}

static void set_range(int *lo, int *hi, int *pos, int16_t a, int16_t b)
{
    *lo = a < b ? a : b;
    *hi = a < b ? b : a;
    *pos = clampi(*pos, *lo, *hi);
}

static void set_handler(Machine *m, bool swap)
{
    Mouse *s = mouse_state(m);
    uint16_t old_mask = s->handler_mask, old_seg = s->handler_seg, old_off = s->handler_off;
    s->handler_mask = CX, s->handler_seg = ES, s->handler_off = DX;
    s->q_count = 0;
    if (swap) {
        CX = old_mask, DX = old_off;
        cpu_set_seg(&m->cpu, S_ES, old_seg);
    }
}

static CallbackResult int33_mouse(Machine *m)
{
    Mouse *s = mouse_state(m);
    switch (AX) {
    case 0x00: case 0x21: reset_driver(m); AX = RESET_OK; BX = 2; break;
    case 0x01: if (s->show_count < 0) s->show_count++; break;
    case 0x02: s->show_count--; break;
    case 0x03: BX = s->buttons; CX = (uint16_t)s->x; DX = (uint16_t)s->y; break;
    case 0x04:
        s->x = clampi((int16_t)CX, s->min_x, s->max_x);
        s->y = clampi((int16_t)DX, s->min_y, s->max_y);
        break;
    case 0x05: button_info(m, true); break;
    case 0x06: button_info(m, false); break;
    case 0x07: set_range(&s->min_x, &s->max_x, &s->x, (int16_t)CX, (int16_t)DX); break;
    case 0x08: set_range(&s->min_y, &s->max_y, &s->y, (int16_t)CX, (int16_t)DX); break;
    case 0x0B:
        CX = (uint16_t)s->mickey_x, DX = (uint16_t)s->mickey_y;
        s->mickey_x = s->mickey_y = 0;
        break;
    case 0x0C: set_handler(m, false); break;
    case 0x0F: if (CX && DX) s->ratio_x = CX, s->ratio_y = DX; break;
    case 0x14: set_handler(m, true); break;
    case 0x15: BX = 0; break; /* state buffer size: nothing to save */
    case 0x1A: s->sens_x = BX, s->sens_y = CX, s->sens_threshold = DX; break;
    case 0x1B: BX = (uint16_t)s->sens_x, CX = (uint16_t)s->sens_y, DX = (uint16_t)s->sens_threshold; break;
    case 0x1F: s->enabled = false; break;
    case 0x20: s->enabled = true; break;
    case 0x24: BX = DRIVER_VERSION; CX = DRIVER_TYPE_PS2 << 8; break;
    case 0x26: BX = 0; CX = (uint16_t)s->max_x; DX = (uint16_t)s->max_y; break;
    default: break; /* cursor shape, pages, light pen: accepted, no effect */
    }
    return CB_DONE;
}

void mouse_init(Machine *m)
{
    Mouse *s = calloc(1, sizeof *s);
    if (!s)
        fatal("out of memory");
    m->mouse = s;
    reset_driver(m);
    install_trampoline(m);
    callback_register(m, "int33", int33_mouse, VEC_MOUSE);
}
