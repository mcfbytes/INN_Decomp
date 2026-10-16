/* The 8259 pair as plain data (docs/native.md section 6): request, in-service and mask bits for 16
 * lines, slave cascaded on master line 2, fully nested fixed priority, specific and non-specific EOI. */
#include "devices.h"
#include "host_kbd_queue.h"
#include "lift_internal.h"
#include "native_internal.h"

enum {
    IRQ_LINES = 16, CASCADE_IRQ = 2, CASCADE_BIT = 1u << CASCADE_IRQ,
    MASTER_VECTOR_BASE = 0x08, SLAVE_VECTOR_BASE = 0x70, NO_LINE = 8,
};

/* Lowest set bit, the highest priority under fixed priority; NO_LINE when none. */
static int highest_priority(uint8_t bits)
{
    return bits ? __builtin_ctz(bits) : NO_LINE;
}

/* The request a chip signals now: unmasked, and above every line in service. */
static int chip_pending(const NativePic *p, uint8_t extra_irr)
{
    int req = highest_priority((uint8_t)((p->irr | extra_irr) & ~p->imr));
    return req < highest_priority(p->isr) ? req : NO_LINE;
}

static NativePic *chip_of(Machine *m, int irq)
{
    if (irq < 0 || irq >= IRQ_LINES)
        fatal("native: IRQ %d out of range", irq);
    return &native_state(m)->pic[irq >> 3];
}

void native_irq_raise(Machine *m, int irq)
{
    chip_of(m, irq)->irr |= (uint8_t)(1u << (irq & 7));
}

void native_irq_lower(Machine *m, int irq)
{
    chip_of(m, irq)->irr &= (uint8_t)~(1u << (irq & 7));
}

void native_pic_eoi(Machine *m, int chip, int irq)
{
    NativePic *p = &native_state(m)->pic[chip ? 1 : 0];
    int line = irq < 0 ? highest_priority(p->isr) : irq & 7;
    if (line < NO_LINE)
        p->isr &= (uint8_t)~(1u << line);
}

void native_pic_set_mask(Machine *m, int chip, uint8_t mask)
{
    native_state(m)->pic[chip ? 1 : 0].imr = mask;
}

uint8_t native_pic_mask(const Machine *m, int chip)
{
    return native_state(m)->pic[chip ? 1 : 0].imr;
}

bool native_deliver_irq(Machine *m)
{
    NativeState *n = native_state(m);
    NativePic *master = &n->pic[0], *slave = &n->pic[1];
    int slave_line = chip_pending(slave, 0);
    int line = chip_pending(master, slave_line < NO_LINE ? CASCADE_BIT : 0);
    if (line == NO_LINE)
        return false;
    uint8_t vector;
    if (line == CASCADE_IRQ && slave_line < NO_LINE) {
        master->isr |= CASCADE_BIT;
        slave->irr &= (uint8_t)~(1u << slave_line);
        slave->isr |= (uint8_t)(1u << slave_line);
        vector = (uint8_t)(SLAVE_VECTOR_BASE + slave_line);
    } else {
        master->irr &= (uint8_t)~(1u << line);
        master->isr |= (uint8_t)(1u << line);
        vector = (uint8_t)(MASTER_VECTOR_BASE + line);
    }
    native_int(m, vector);
    return true;
}

/* The vector is read before the frame is pushed, as the CPU does (the stack may overlap the IVT). */
void native_int(Machine *m, uint8_t vector)
{
    uint16_t off = mem_rd16(m, vector * 4u), seg = mem_rd16(m, vector * 4u + 2);
    uint16_t saved_cs = m->cpu.seg[S_CS];
    uint32_t saved_ip = m->cpu.eip;
    cpu_push16(m, (uint16_t)m->cpu.flags);
    m->cpu.flags &= ~(uint32_t)(F_IF | F_TF);
    lift_dispatch_far_at(m, seg, off);
    cpu_set_seg(&m->cpu, S_CS, saved_cs);
    m->cpu.eip = saved_ip;
}

/* ---- mixed mode (docs/native.md section 11, package 10b) ---- */

enum { KBD_LINE = 1, KBD_LINE_BIT = 1u << KBD_LINE };

/* The scancode bytes the native model holds but IRQ 1 has not delivered yet go to the 8042, so a key
 * pressed just before the game started is not lost. A byte whose ISR already read it is done. */
static void keyboard_to_8042(Machine *m)
{
    HostKbdQueue *q = kbd_host_queue(m);
    if (!q)
        return;
    if (q->has_latch && !q->latch_read)
        kbd_host_raw(m, q->latched);
    for (int i = 0; i < q->count; i++)
        kbd_host_raw(m, q->bytes[(q->head + i) % HOST_KBD_QUEUE]);
    *q = (HostKbdQueue){ 0 };
}

void native_irq_to_pic(Machine *m)
{
    NativeState *n = native_state(m);
    /* Line 1 belongs to the keyboard queue handled below, not to the request bit. */
    n->pic[0].irr &= (uint8_t)~KBD_LINE_BIT;
    for (int chip = 0; chip < 2; chip++) {
        NativePic *p = &n->pic[chip];
        pic_set_state(m, chip, p->irr, p->isr, p->imr);
        p->irr = p->isr = p->lines = 0; /* pending requests are delivered once, by the emulated chip */
    }
    keyboard_to_8042(m);
}

void native_irq_from_pic(Machine *m)
{
    NativeState *n = native_state(m);
    for (int chip = 0; chip < 2; chip++) {
        uint8_t irr, isr, imr;
        pic_get_state(m, chip, &irr, &isr, &imr);
        NativePic *p = &n->pic[chip];
        /* The game's requests and in-service bits die with it: a bit left in service would block its line
         * for good. Only the mask carries over. */
        p->irr = p->isr = p->lines = 0;
        p->imr = imr;
        pic_set_state(m, chip, 0, 0, imr);
    }
    kbd_clear(m);
    HostKbdQueue *q = kbd_host_queue(m);
    if (q)
        *q = (HostKbdQueue){ 0 };
}
