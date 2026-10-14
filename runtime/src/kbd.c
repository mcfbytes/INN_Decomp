/* 8042 keyboard controller (60h data, 64h status/command, IRQ1). Host keys are released one
 * byte per slice after the guest read the previous one, about a real keyboard link's speed. */
#include <stdlib.h>

#include "devices.h"

enum {
    PORT_KBD_DATA = 0x60, PORT_KBD_STATUS = 0x64,
    STATUS_OBF = 0x01, STATUS_SYSTEM = 0x04, STATUS_CMD = 0x08, STATUS_UNLOCKED = 0x10,
    KBD_QUEUE = 64,
    EXTENDED_PREFIX = 0xE0, RELEASE_BIT = 0x80,
    CTL_READ_CMD = 0x20, CTL_WRITE_CMD = 0x60, CTL_SELF_TEST = 0xAA, CTL_IFACE_TEST = 0xAB,
    CTL_DISABLE_KBD = 0xAD, CTL_ENABLE_KBD = 0xAE, CTL_READ_OUTPUT = 0xD0,
    CTL_WRITE_OUTPUT = 0xD1, CTL_A20_OFF = 0xDD, CTL_A20_ON = 0xDF, CTL_PULSE_FIRST = 0xF0,
    SELF_TEST_OK = 0x55, IFACE_OK = 0x00,
    OUTPUT_A20 = 0x02, OUTPUT_DEFAULT = 0x01,
    CMDBYTE_DEFAULT = 0x45,
    KBD_ACK = 0xFA, KBD_BAT_OK = 0xAA, KBD_ID1 = 0xAB, KBD_ID2 = 0x83,
    KBD_SET_LEDS = 0xED, KBD_ECHO = 0xEE, KBD_SCANSET = 0xF0, KBD_IDENTIFY = 0xF2,
    KBD_TYPEMATIC = 0xF3, KBD_RESET = 0xFF,
};

typedef enum PendingWrite { WRITE_NONE, WRITE_CMD_BYTE, WRITE_OUTPUT, WRITE_KBD_ARG } PendingWrite;

typedef struct Kbd {
    uint8_t queue[KBD_QUEUE];
    int head, count;
    uint8_t data;
    bool full, enabled, last_was_cmd;
    uint8_t cmd_byte, output_port;
    PendingWrite pending;
} Kbd;

static Kbd *kbd_state(Machine *m) { return m->kbd; }

static void enqueue(Kbd *k, uint8_t b)
{
    if (k->count == KBD_QUEUE)
        return; /* keyboard overrun: drop, as the real buffer does */
    k->queue[(k->head + k->count++) % KBD_QUEUE] = b;
}

/* Replies from the controller jump the queue so command/response pairs stay ordered. */
static void reply(Kbd *k, uint8_t b)
{
    if (k->count == KBD_QUEUE)
        k->count--;
    k->head = (k->head + KBD_QUEUE - 1) % KBD_QUEUE;
    k->queue[k->head] = b;
    k->count++;
}

static void kbd_deliver(Machine *m)
{
    Kbd *k = kbd_state(m);
    if (k->full || !k->count || !k->enabled)
        return;
    k->data = k->queue[k->head];
    k->head = (k->head + 1) % KBD_QUEUE;
    k->count--;
    k->full = true;
    pic_raise_irq(m, PIC_IRQ_KEYBOARD);
}

void kbd_host_key(Machine *m, uint16_t set1_scancode, bool down)
{
    Kbd *k = kbd_state(m);
    if ((set1_scancode >> 8) == EXTENDED_PREFIX)
        enqueue(k, EXTENDED_PREFIX);
    enqueue(k, (uint8_t)((set1_scancode & 0x7F) | (down ? 0 : RELEASE_BIT)));
    kbd_deliver(m);
}

static void keyboard_command(Kbd *k, uint8_t v)
{
    if (k->pending == WRITE_KBD_ARG) {
        k->pending = WRITE_NONE;
        enqueue(k, KBD_ACK);
        return;
    }
    switch (v) {
    case KBD_SET_LEDS: case KBD_TYPEMATIC: case KBD_SCANSET:
        k->pending = WRITE_KBD_ARG;
        enqueue(k, KBD_ACK);
        break;
    case KBD_ECHO: enqueue(k, KBD_ECHO); break;
    case KBD_IDENTIFY: enqueue(k, KBD_ACK); enqueue(k, KBD_ID1); enqueue(k, KBD_ID2); break;
    case KBD_RESET: enqueue(k, KBD_ACK); enqueue(k, KBD_BAT_OK); break;
    default: enqueue(k, KBD_ACK); break;
    }
}

static void data_write(Machine *m, uint8_t v)
{
    Kbd *k = kbd_state(m);
    k->last_was_cmd = false;
    switch (k->pending) {
    case WRITE_CMD_BYTE:
        k->cmd_byte = v;
        k->pending = WRITE_NONE;
        break;
    case WRITE_OUTPUT:
        k->output_port = v;
        m->a20 = v & OUTPUT_A20;
        k->pending = WRITE_NONE;
        break;
    default:
        keyboard_command(k, v);
    }
}

static void controller_command(Machine *m, uint8_t v)
{
    Kbd *k = kbd_state(m);
    k->last_was_cmd = true;
    switch (v) {
    case CTL_READ_CMD: reply(k, k->cmd_byte); break;
    case CTL_WRITE_CMD: k->pending = WRITE_CMD_BYTE; break;
    case CTL_SELF_TEST: reply(k, SELF_TEST_OK); break;
    case CTL_IFACE_TEST: reply(k, IFACE_OK); break;
    case CTL_DISABLE_KBD: k->enabled = false; break;
    case CTL_ENABLE_KBD: k->enabled = true; break;
    case CTL_READ_OUTPUT: reply(k, k->output_port); break;
    case CTL_WRITE_OUTPUT: k->pending = WRITE_OUTPUT; break;
    case CTL_A20_OFF: m->a20 = false; k->output_port &= (uint8_t)~OUTPUT_A20; break;
    case CTL_A20_ON: m->a20 = true; k->output_port |= OUTPUT_A20; break;
    default: break; /* reset pulses (F0-FF) and the rest are ignored */
    }
}

static uint32_t kbd_in(Machine *m, uint16_t port, int size)
{
    Kbd *k = kbd_state(m);
    if (port == PORT_KBD_STATUS) {
        uint8_t s = STATUS_SYSTEM | STATUS_UNLOCKED;
        if (k->full)
            s |= STATUS_OBF;
        if (k->last_was_cmd)
            s |= STATUS_CMD;
        return s;
    }
    k->full = false;
    pic_lower_irq(m, PIC_IRQ_KEYBOARD);
    return k->data;
}

static void kbd_out(Machine *m, uint16_t port, uint32_t value, int size)
{
    if (port == PORT_KBD_STATUS)
        controller_command(m, (uint8_t)value);
    else
        data_write(m, (uint8_t)value);
    kbd_deliver(m);
}

void kbd_init(Machine *m)
{
    Kbd *k = calloc(1, sizeof *k);
    if (!k)
        fatal("out of memory");
    k->enabled = true;
    k->cmd_byte = CMDBYTE_DEFAULT;
    k->output_port = OUTPUT_DEFAULT | (m->a20 ? OUTPUT_A20 : 0);
    m->kbd = k;
    port_register(m, PORT_KBD_DATA, PORT_KBD_DATA, kbd_in, kbd_out);
    port_register(m, PORT_KBD_STATUS, PORT_KBD_STATUS, kbd_in, kbd_out);
    machine_add_ticker(m, kbd_deliver);
}
