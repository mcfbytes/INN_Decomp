/* 8259A programmable interrupt controller pair: master at 20h/21h, slave at A0h/A1h cascaded
 * on master IRQ2. Fixed priority (IRQ0 highest), edge-triggered lines. */
#include <stdlib.h>

#include "devices.h"

enum {
    PIC_MASTER_CMD = 0x20, PIC_MASTER_DATA = 0x21,
    PIC_SLAVE_CMD = 0xA0, PIC_SLAVE_DATA = 0xA1,
    ICW1_FLAG = 0x10, ICW1_NEED_ICW4 = 0x01, ICW1_SINGLE = 0x02,
    OCW3_FLAG = 0x08, OCW3_READ_MASK = 0x03, OCW3_READ_IRR = 0x02, OCW3_READ_ISR = 0x03,
    OCW3_POLL = 0x04,
    ICW4_AUTO_EOI = 0x02,
    OCW2_EOI = 0x20, OCW2_SPECIFIC = 0x40, OCW2_LEVEL_MASK = 0x07,
    CASCADE_BIT = 1u << PIC_IRQ_CASCADE,
};

typedef enum InitStep { INIT_DONE, INIT_ICW2, INIT_ICW3, INIT_ICW4 } InitStep;

typedef struct Pic8259 {
    uint8_t irr, isr, imr, lines;
    uint8_t vector_base;
    InitStep step;
    bool need_icw4, single, auto_eoi, read_isr, poll;
} Pic8259;

typedef struct PicPair {
    Pic8259 chip[2]; /* 0 master, 1 slave */
} PicPair;

static PicPair *pic_state(Machine *m) { return m->pic; }

/* Lowest set bit, i.e. highest priority under fixed priority; 8 when none. */
static int highest_bit(uint8_t v)
{
    for (int i = 0; i < 8; i++)
        if (v & (1u << i))
            return i;
    return 8;
}

/* The request this chip would signal now: unmasked, and above everything in service. */
static int chip_pending(const Pic8259 *p, uint8_t extra_irr)
{
    int req = highest_bit((uint8_t)((p->irr | extra_irr) & ~p->imr));
    return req < highest_bit(p->isr) ? req : 8;
}

static uint8_t slave_request(const PicPair *s)
{
    return chip_pending(&s->chip[1], 0) < 8 ? CASCADE_BIT : 0;
}

static void pic_update(Machine *m)
{
    PicPair *s = pic_state(m);
    m->irq_pending = chip_pending(&s->chip[0], slave_request(s)) < 8 ? 1 : 0;
}

void pic_raise_irq(Machine *m, int irq)
{
    PicPair *s = pic_state(m);
    Pic8259 *p = &s->chip[irq >> 3];
    uint8_t bit = (uint8_t)(1u << (irq & 7));
    if (!(p->lines & bit))
        p->irr |= bit;
    p->lines |= bit;
    pic_update(m);
}

/* True while irq is requested and not yet accepted by the CPU. */
bool pic_irq_pending(Machine *m, int irq)
{
    return pic_state(m)->chip[irq >> 3].irr & (1u << (irq & 7));
}

/* True when a CPU with interrupts enabled would take irq at once: unmasked and not in service. */
bool pic_irq_deliverable(Machine *m, int irq)
{
    const Pic8259 *p = &pic_state(m)->chip[irq >> 3];
    uint8_t bit = (uint8_t)(1u << (irq & 7));
    return !(p->imr & bit) && !(p->isr & bit);
}

void pic_lower_irq(Machine *m, int irq)
{
    Pic8259 *p = &pic_state(m)->chip[irq >> 3];
    p->lines &= (uint8_t)~(1u << (irq & 7));
}

/* Moves irq from requested to in-service and returns its vector. */
static int chip_accept(Pic8259 *p, int irq)
{
    uint8_t bit = (uint8_t)(1u << irq);
    p->irr &= (uint8_t)~bit;
    if (!p->auto_eoi)
        p->isr |= bit;
    return p->vector_base + irq;
}

int pic_acknowledge(Machine *m)
{
    PicPair *s = pic_state(m);
    Pic8259 *master = &s->chip[0], *slave = &s->chip[1];
    int irq = chip_pending(master, slave_request(s));
    int vector = -1;
    if (irq == PIC_IRQ_CASCADE && slave_request(s)) {
        if (!master->auto_eoi)
            master->isr |= CASCADE_BIT;
        vector = chip_accept(slave, chip_pending(slave, 0));
    } else if (irq < 8) {
        vector = chip_accept(master, irq);
    }
    pic_update(m);
    return vector;
}

static void pic_eoi(Pic8259 *p, uint8_t ocw2)
{
    if (ocw2 & OCW2_SPECIFIC)
        p->isr &= (uint8_t)~(1u << (ocw2 & OCW2_LEVEL_MASK));
    else
        p->isr &= (uint8_t)~(1u << highest_bit(p->isr));
}

static void pic_command(Pic8259 *p, uint8_t v)
{
    if (v & ICW1_FLAG) {
        p->step = INIT_ICW2;
        p->need_icw4 = v & ICW1_NEED_ICW4;
        p->single = v & ICW1_SINGLE;
        p->imr = p->isr = p->irr = 0;
        p->auto_eoi = p->read_isr = false;
    } else if (v & OCW3_FLAG) {
        p->poll = v & OCW3_POLL;
        if ((v & OCW3_READ_MASK) == OCW3_READ_IRR)
            p->read_isr = false;
        else if ((v & OCW3_READ_MASK) == OCW3_READ_ISR)
            p->read_isr = true;
    } else if (v & OCW2_EOI) {
        pic_eoi(p, v); /* rotate variants are treated as plain EOIs */
    }
}

static void pic_data(Pic8259 *p, uint8_t v)
{
    switch (p->step) {
    case INIT_ICW2:
        p->vector_base = v & 0xF8;
        p->step = p->single ? (p->need_icw4 ? INIT_ICW4 : INIT_DONE) : INIT_ICW3;
        break;
    case INIT_ICW3:
        p->step = p->need_icw4 ? INIT_ICW4 : INIT_DONE;
        break;
    case INIT_ICW4:
        p->auto_eoi = v & ICW4_AUTO_EOI;
        p->step = INIT_DONE;
        break;
    case INIT_DONE:
        p->imr = v;
        break;
    }
}

/* OCW3 poll mode: the read acknowledges the highest request, as INTA would. */
static uint8_t pic_poll(Pic8259 *p)
{
    p->poll = false;
    int irq = chip_pending(p, 0);
    if (irq >= 8)
        return 0;
    chip_accept(p, irq);
    return (uint8_t)(0x80 | irq);
}

static uint32_t pic_in(Machine *m, uint16_t port, int size)
{
    Pic8259 *p = &pic_state(m)->chip[port >= PIC_SLAVE_CMD];
    uint8_t v;
    if (port & 1)
        v = p->imr;
    else if (p->poll)
        v = pic_poll(p);
    else
        v = p->read_isr ? p->isr : p->irr;
    pic_update(m);
    return v;
}

static void pic_out(Machine *m, uint16_t port, uint32_t value, int size)
{
    Pic8259 *p = &pic_state(m)->chip[port >= PIC_SLAVE_CMD];
    if (port & 1)
        pic_data(p, (uint8_t)value);
    else
        pic_command(p, (uint8_t)value);
    pic_update(m);
}

/* Matches what a PC BIOS leaves behind: vectors 08h and 70h, timer/keyboard/cascade and the
 * PS/2 mouse unmasked. */
static const Pic8259 PIC_BOOT[2] = {
    { .vector_base = 0x08, .imr = 0xB8 },
    { .vector_base = 0x70, .imr = 0xEF },
};

void pic_init(Machine *m)
{
    PicPair *s = calloc(1, sizeof *s);
    if (!s)
        fatal("out of memory");
    s->chip[0] = PIC_BOOT[0];
    s->chip[1] = PIC_BOOT[1];
    m->pic = s;
    port_register(m, PIC_MASTER_CMD, PIC_MASTER_DATA, pic_in, pic_out);
    port_register(m, PIC_SLAVE_CMD, PIC_SLAVE_DATA, pic_in, pic_out);
    pic_update(m);
}

/* Mixed-mode hand-off: the native interrupt model takes over the chip's masks and requests (docs/native.md
 * section 11). Lines count as low afterwards, so a device raises its next edge normally. */
void pic_get_state(Machine *m, int chip, uint8_t *irr, uint8_t *isr, uint8_t *imr)
{
    const Pic8259 *p = &pic_state(m)->chip[chip ? 1 : 0];
    *irr = p->irr;
    *isr = p->isr;
    *imr = p->imr;
}

void pic_set_state(Machine *m, int chip, uint8_t irr, uint8_t isr, uint8_t imr)
{
    Pic8259 *p = &pic_state(m)->chip[chip ? 1 : 0];
    p->irr = irr;
    p->isr = isr;
    p->imr = imr;
    p->lines = 0;
    pic_update(m);
}
