/* 8250/16550A UARTs at COM1 (3F8h, IRQ4) and COM2 (2F8h, IRQ3), paced at the programmed baud
 * rate in virtual time, with a SerialLink standing in for the modem. Also a minimal INT 14h. */
#include <stdlib.h>
#include <string.h>

#include "bios_internal.h"

enum {
    COM_PORTS = 2, FIFO_SIZE = 16, UART_CLOCK_DIV = 115200,
    REG_DATA = 0, REG_IER = 1, REG_IIR_FCR = 2, REG_LCR = 3, REG_MCR = 4, REG_LSR = 5,
    REG_MSR = 6, REG_SCR = 7,
    IER_RX = 0x01, IER_THRE = 0x02, IER_LSR = 0x04, IER_MSR = 0x08,
    IIR_NONE = 0x01, IIR_MSR = 0x00, IIR_THRE = 0x02, IIR_RX = 0x04, IIR_LSR = 0x06,
    IIR_TIMEOUT = 0x0C, IIR_FIFO_ON = 0xC0,
    FCR_ENABLE = 0x01, FCR_CLEAR_RX = 0x02, FCR_CLEAR_TX = 0x04, FCR_TRIGGER_SHIFT = 6,
    LCR_WORD_MASK = 0x03, LCR_STOP2 = 0x04, LCR_PARITY = 0x08, LCR_BREAK = 0x40, LCR_DLAB = 0x80,
    MCR_DTR = 0x01, MCR_RTS = 0x02, MCR_OUT1 = 0x04, MCR_OUT2 = 0x08, MCR_LOOP = 0x10,
    LSR_DR = 0x01, LSR_OE = 0x02, LSR_BI = 0x10, LSR_THRE = 0x20, LSR_TEMT = 0x40,
    LSR_ERRORS = 0x1E,
    MSR_CTS = 0x10, MSR_DSR = 0x20, MSR_RI = 0x40, MSR_DCD = 0x80, MSR_TERI = 0x04,
    MSR_DELTAS = 0x0F,
    TIMEOUT_CHARS = 4,
    VEC_SERIAL = 0x14, INT14_TIMEOUT = 0x80,
};

static const uint16_t COM_BASE[COM_PORTS] = { 0x3F8, 0x2F8 };
static const int COM_IRQ[COM_PORTS] = { PIC_IRQ_COM1, PIC_IRQ_COM2 };
static const uint8_t RX_TRIGGER[4] = { 1, 4, 8, 14 };
static const uint16_t INT14_DIVISORS[8] = { 1047, 768, 384, 192, 96, 48, 24, 12 };

typedef struct Fifo { uint8_t data[FIFO_SIZE]; int head, count; } Fifo;

typedef struct Uart {
    uint8_t ier, lcr, mcr, lsr, msr, scr, fcr, dll, dlm;
    Fifo rx, tx;
    bool thre_pending, irq_high;
    bool shifting; /* the transmit shift register holds shift_byte until shift_done_us */
    uint8_t shift_byte;
    uint64_t shift_done_us;
    uint64_t next_rx_us, last_rx_us;
    SerialLink link;
} Uart;

typedef struct Uarts { Uart com[COM_PORTS]; } Uarts;

static Uart *uart_at(Machine *m, int index) { return &((Uarts *)m->uart)->com[index]; }

static int port_index(uint16_t port) { return (port & 0xFF00) == (COM_BASE[0] & 0xFF00) ? 0 : 1; }

static bool fifo_push(Fifo *f, uint8_t b)
{
    if (f->count == FIFO_SIZE)
        return false;
    f->data[(f->head + f->count++) % FIFO_SIZE] = b;
    return true;
}

static uint8_t fifo_pop(Fifo *f)
{
    uint8_t b = f->data[f->head];
    f->head = (f->head + 1) % FIFO_SIZE;
    f->count--;
    return b;
}

static bool fifo_on(const Uart *u) { return u->fcr & FCR_ENABLE; }
static int capacity(const Uart *u) { return fifo_on(u) ? FIFO_SIZE : 1; }

/* Start bit, data bits, parity, stop bits, at 115200/divisor baud. */
static uint64_t byte_us(const Uart *u)
{
    unsigned divisor = u->dll | u->dlm << 8;
    if (!divisor)
        divisor = 1;
    int bits = 1 + 5 + (u->lcr & LCR_WORD_MASK) + ((u->lcr & LCR_PARITY) ? 1 : 0) + ((u->lcr & LCR_STOP2) ? 2 : 1);
    return (uint64_t)bits * 1000000u * divisor / UART_CLOCK_DIV;
}

/* ---- modem status ---- */

static uint8_t modem_lines(const Uart *u)
{
    if (u->mcr & MCR_LOOP) {
        return (uint8_t)(((u->mcr & MCR_RTS) ? MSR_CTS : 0) | ((u->mcr & MCR_DTR) ? MSR_DSR : 0) |
                         ((u->mcr & MCR_OUT1) ? MSR_RI : 0) | ((u->mcr & MCR_OUT2) ? MSR_DCD : 0));
    }
    bool up = u->link.connected && u->link.connected(u->link.ctx);
    return up ? (uint8_t)(MSR_CTS | MSR_DSR | MSR_DCD) : 0;
}

static void refresh_msr(Uart *u)
{
    uint8_t now = modem_lines(u), old = u->msr;
    uint8_t delta = (uint8_t)((now ^ old) >> 4) & (uint8_t)~MSR_TERI;
    if ((old & MSR_RI) && !(now & MSR_RI))
        delta |= MSR_TERI;
    u->msr = (uint8_t)(now | (old & MSR_DELTAS) | delta);
}

/* ---- interrupts ---- */

static bool rx_timeout(Machine *m, const Uart *u)
{
    return fifo_on(u) && u->rx.count && m->virtual_us - u->last_rx_us >= TIMEOUT_CHARS * byte_us(u);
}

static uint8_t pending_iir(Machine *m, const Uart *u)
{
    if ((u->ier & IER_LSR) && (u->lsr & LSR_ERRORS))
        return IIR_LSR;
    if (u->ier & IER_RX) {
        int trigger = fifo_on(u) ? RX_TRIGGER[u->fcr >> FCR_TRIGGER_SHIFT] : 1;
        if (u->rx.count >= trigger)
            return IIR_RX;
        if (rx_timeout(m, u))
            return IIR_TIMEOUT;
    }
    if ((u->ier & IER_THRE) && u->thre_pending)
        return IIR_THRE;
    if ((u->ier & IER_MSR) && (u->msr & MSR_DELTAS))
        return IIR_MSR;
    return IIR_NONE;
}

static void update_irq(Machine *m, int index)
{
    Uart *u = uart_at(m, index);
    bool high = (u->mcr & MCR_OUT2) && pending_iir(m, u) != IIR_NONE;
    if (high && !u->irq_high)
        pic_raise_irq(m, COM_IRQ[index]);
    else if (!high && u->irq_high)
        pic_lower_irq(m, COM_IRQ[index]);
    u->irq_high = high;
}

/* ---- data path ---- */

static void receive_byte(Machine *m, Uart *u, uint8_t b)
{
    if (u->rx.count >= capacity(u)) {
        u->lsr |= LSR_OE;
        return;
    }
    fifo_push(&u->rx, b);
    u->last_rx_us = m->virtual_us;
}

static uint8_t data_read(Machine *m, Uart *u)
{
    if (!u->rx.count)
        return 0;
    uint8_t b = fifo_pop(&u->rx);
    u->last_rx_us = m->virtual_us;
    return b;
}

/* THR (or the FIFO) empties into the shift register at once, so THRE rises a byte early. */
static void start_shift(Machine *m, Uart *u)
{
    if (u->shifting || !u->tx.count)
        return;
    u->shift_byte = fifo_pop(&u->tx);
    u->shifting = true;
    u->shift_done_us = m->virtual_us + byte_us(u);
    if (!u->tx.count)
        u->thre_pending = true;
}

static void data_write(Machine *m, Uart *u, uint8_t b)
{
    u->thre_pending = false;
    if (u->tx.count < capacity(u))
        fifo_push(&u->tx, b);
    start_shift(m, u);
}

static void set_break(Uart *u, uint8_t new_lcr)
{
    bool rising = (new_lcr & LCR_BREAK) && !(u->lcr & LCR_BREAK);
    /* innkeeperd treats a CR between frames as the PAD escape a real BREAK would cause. */
    static const uint8_t BREAK_STANDIN = '\r';
    if (rising && !(u->mcr & MCR_LOOP) && u->link.send)
        u->link.send(u->link.ctx, &BREAK_STANDIN, 1);
}

static uint8_t lsr_value(const Uart *u)
{
    uint8_t v = u->lsr;
    if (u->rx.count)
        v |= LSR_DR;
    if (!u->tx.count)
        v |= LSR_THRE;
    if (!u->tx.count && !u->shifting)
        v |= LSR_TEMT;
    return v;
}

/* ---- ports ---- */

static uint32_t uart_in(Machine *m, uint16_t port, int size)
{
    int index = port_index(port);
    Uart *u = uart_at(m, index);
    uint8_t v = 0;
    switch (port & 7) {
    case REG_DATA: v = (u->lcr & LCR_DLAB) ? u->dll : data_read(m, u); break;
    case REG_IER: v = (u->lcr & LCR_DLAB) ? u->dlm : u->ier; break;
    case REG_IIR_FCR:
        v = pending_iir(m, u);
        if (v == IIR_THRE)
            u->thre_pending = false;
        if (fifo_on(u))
            v |= IIR_FIFO_ON;
        break;
    case REG_LCR: v = u->lcr; break;
    case REG_MCR: v = u->mcr; break;
    case REG_LSR: v = lsr_value(u); u->lsr &= (uint8_t)~LSR_ERRORS; break;
    case REG_MSR: refresh_msr(u); v = u->msr; u->msr &= (uint8_t)~MSR_DELTAS; break;
    case REG_SCR: v = u->scr; break;
    }
    update_irq(m, index);
    return v;
}

static void fcr_write(Uart *u, uint8_t v)
{
    if ((v & FCR_ENABLE) != (u->fcr & FCR_ENABLE) || (v & FCR_CLEAR_RX))
        u->rx.count = 0;
    if ((v & FCR_ENABLE) != (u->fcr & FCR_ENABLE) || (v & FCR_CLEAR_TX))
        u->tx.count = 0;
    u->fcr = v & (uint8_t)(FCR_ENABLE | 0xC0);
}

static void uart_out(Machine *m, uint16_t port, uint32_t value, int size)
{
    int index = port_index(port);
    Uart *u = uart_at(m, index);
    uint8_t v = (uint8_t)value;
    switch (port & 7) {
    case REG_DATA:
        if (u->lcr & LCR_DLAB) u->dll = v; else data_write(m, u, v);
        break;
    case REG_IER:
        if (u->lcr & LCR_DLAB) {
            u->dlm = v;
        } else {
            if ((v & IER_THRE) && !(u->ier & IER_THRE) && !u->tx.count)
                u->thre_pending = true; /* 16550: enabling THRE with an empty THR interrupts */
            u->ier = v & 0x0F;
        }
        break;
    case REG_IIR_FCR: fcr_write(u, v); break;
    case REG_LCR: set_break(u, v); u->lcr = v; break;
    case REG_MCR: u->mcr = v & 0x1F; refresh_msr(u); break;
    case REG_SCR: u->scr = v; break;
    default: break;
    }
    update_irq(m, index);
}

/* ---- scheduler ---- */

static void pump_tx(Machine *m, Uart *u)
{
    if (!u->shifting || m->virtual_us < u->shift_done_us)
        return;
    if (u->mcr & MCR_LOOP) {
        receive_byte(m, u, u->shift_byte);
    } else if (u->link.send && u->link.send(u->link.ctx, &u->shift_byte, 1) == 0) {
        return; /* link busy: retry next slice */
    }
    u->shifting = false;
    start_shift(m, u);
}

static void pump_rx(Machine *m, Uart *u)
{
    if ((u->mcr & MCR_LOOP) || !u->link.recv || m->virtual_us < u->next_rx_us)
        return;
    if (u->rx.count >= capacity(u))
        return; /* hold bytes in the link rather than overrun */
    uint8_t b;
    if (u->link.recv(u->link.ctx, &b, 1) == 1) {
        receive_byte(m, u, b);
        u->next_rx_us = m->virtual_us + byte_us(u);
    }
}

static void uart_tick(Machine *m)
{
    for (int i = 0; i < COM_PORTS; i++) {
        Uart *u = uart_at(m, i);
        pump_tx(m, u);
        pump_rx(m, u);
        refresh_msr(u);
        update_irq(m, i);
    }
}

/* ---- INT 14h ---- */

static uint16_t int14_status(Uart *u)
{
    refresh_msr(u);
    return (uint16_t)(lsr_value(u) << 8 | u->msr);
}

static CallbackResult int14_serial(Machine *m)
{
    if (DX >= COM_PORTS) {
        AH = INT14_TIMEOUT;
        return CB_DONE;
    }
    Uart *u = uart_at(m, DX);
    switch (AH) {
    case 0x00: {
        uint16_t div = INT14_DIVISORS[AL >> 5];
        u->dll = (uint8_t)div, u->dlm = (uint8_t)(div >> 8);
        u->lcr = AL & 0x1F;
        AX = int14_status(u);
        break;
    }
    case 0x01: data_write(m, u, AL); AH = (uint8_t)(int14_status(u) >> 8); break;
    case 0x02:
        if (u->rx.count) {
            AL = data_read(m, u);
            AH = (uint8_t)(int14_status(u) >> 8) & LSR_ERRORS;
        } else {
            AH = INT14_TIMEOUT;
        }
        break;
    case 0x03: AX = int14_status(u); break;
    default: break;
    }
    return CB_DONE;
}

/* ---- setup ---- */

int uart_attach(Machine *m, int com, SerialLink link)
{
    if (com < UART_COM1 || com > UART_COM2 || !m->uart)
        return -1;
    Uart *u = uart_at(m, com - 1);
    if (u->link.close)
        u->link.close(u->link.ctx);
    u->link = link;
    return 0;
}

SerialLink *uart_link(Machine *m, int com)
{
    if (com < UART_COM1 || com > UART_COM2 || !m->uart)
        return NULL;
    return &uart_at(m, com - 1)->link;
}

void uart_init(Machine *m)
{
    Uarts *s = calloc(1, sizeof *s);
    if (!s)
        fatal("out of memory");
    m->uart = s;
    for (int i = 0; i < COM_PORTS; i++) {
        s->com[i].dll = INT14_DIVISORS[5] & 0xFF; /* 2400 baud until programmed */
        s->com[i].lcr = 0x03;
        port_register(m, COM_BASE[i], (uint16_t)(COM_BASE[i] + 7), uart_in, uart_out);
    }
    machine_add_ticker(m, uart_tick);
    callback_register(m, "int14", int14_serial, VEC_SERIAL);
}
