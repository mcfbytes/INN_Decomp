/* Host keyboard and mouse (docs/native.md section 9). Emulated mode forwards to the 8042 and INT 33h;
 * native mode queues scancodes for IRQ 1 and delivers mouse events at safe points. */
#include "host_input.h"

#include "devices.h"
#include "host_kbd_queue.h"
#include "native.h"
#include "native_internal.h"

enum {
    KBD_IRQ = 1, KBD_IRQ_BIT = 1u << KBD_IRQ,
    EXTENDED_PREFIX = 0xE0, RELEASE_BIT = 0x80,
};

/* One mouse delivery at a time: the handler's own safe point must not start the next event. */
static bool delivering;

static bool enqueue_byte(HostKbdQueue *q, uint8_t b)
{
    if (q->count == HOST_KBD_QUEUE)
        return false;
    q->bytes[(q->head + q->count++) % HOST_KBD_QUEUE] = b;
    return true;
}

void host_input_key(Machine *m, uint16_t set1_scancode, bool down)
{
    if (!m->native) {
        kbd_host_key(m, set1_scancode, down);
        return;
    }
    HostKbdQueue *q = kbd_host_queue(m);
    if (!q)
        return;
    bool queued = false;
    if ((set1_scancode >> 8) == EXTENDED_PREFIX)
        queued |= enqueue_byte(q, EXTENDED_PREFIX);
    queued |= enqueue_byte(q, (uint8_t)((set1_scancode & 0x7F) | (down ? 0 : RELEASE_BIT)));
    if (queued)
        native_wake(m);
}

void host_input_mouse_motion(Machine *m, int x, int y, int frame_w, int frame_h)
{
    if (!m->native) {
        mouse_host_motion(m, x, y, frame_w, frame_h);
        return;
    }
    int before = mouse_queued(m);
    mouse_host_motion(m, x, y, frame_w, frame_h);
    if (mouse_queued(m) != before)
        native_wake(m);
}

void host_input_mouse_button(Machine *m, int button, bool down)
{
    if (!m->native) {
        mouse_host_button(m, button, down);
        return;
    }
    int before = mouse_queued(m);
    mouse_host_button(m, button, down);
    if (mouse_queued(m) != before)
        native_wake(m);
}

/* The byte stays latched until the ISR has read it and IRQ 1 has left service (EOI).
 * saw_service distinguishes "not yet delivered" from "delivered and EOIed": both have ISR clear. */
static void release_latch(Machine *m, HostKbdQueue *q)
{
    uint8_t isr = native_state(m)->pic[0].isr;
    if (isr & KBD_IRQ_BIT)
        q->saw_service = true;
    if (q->has_latch && q->latch_read && q->saw_service && !(isr & KBD_IRQ_BIT)) {
        q->has_latch = false;
        q->latch_read = false;
        q->saw_service = false;
        native_irq_lower(m, KBD_IRQ);
    }
}

static void latch_next(Machine *m, HostKbdQueue *q)
{
    if (q->has_latch || !q->count)
        return;
    q->latched = q->bytes[q->head];
    q->head = (q->head + 1) % HOST_KBD_QUEUE;
    q->count--;
    q->has_latch = true;
    q->latch_read = false;
    q->saw_service = false;
    native_irq_raise(m, KBD_IRQ);
}

void host_input_poll(Machine *m)
{
    if (!m->native)
        return;
    HostKbdQueue *q = kbd_host_queue(m);
    if (q) {
        release_latch(m, q);
        latch_next(m, q);
    }
    if ((m->cpu.flags & F_IF) && !delivering) {
        delivering = true;
        mouse_native_deliver(m);
        delivering = false;
    }
}

uint8_t host_input_kbd_data(Machine *m)
{
    native_safepoint(m);
    HostKbdQueue *q = kbd_host_queue(m);
    if (!q || !q->has_latch)
        return 0;
    if (native_state(m)->pic[0].isr & KBD_IRQ_BIT)
        q->saw_service = true;
    q->latch_read = true;
    return q->latched;
}

void host_input_kbd_ack(Machine *m)
{
    native_safepoint(m);
}
