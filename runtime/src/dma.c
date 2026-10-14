/* 8237A DMA controller for the 8-bit channels 0-3 (ports 00h-0Fh, page registers 87h/83h/81h/82h).
 * Devices pull bytes with dma_read when their own timing says a transfer happens. */
#include <stdlib.h>

#include "devices.h"

enum {
    DMA_PORT_LAST = 0x0F,
    REG_STATUS = 0x08, REG_REQUEST = 0x09, REG_SINGLE_MASK = 0x0A, REG_MODE = 0x0B,
    REG_CLEAR_FLIPFLOP = 0x0C, REG_MASTER_CLEAR = 0x0D, REG_CLEAR_MASKS = 0x0E, REG_ALL_MASKS = 0x0F,
    CMD_DISABLE = 0x04,
    MODE_AUTO_INIT = 0x10, MODE_DECREMENT = 0x20,
    MASK_SET = 0x04,
};

static const uint16_t PAGE_PORTS[DMA_CHANNELS] = { 0x87, 0x83, 0x81, 0x82 };

typedef struct DmaChannel {
    uint16_t base_addr, base_count, addr, count;
    uint8_t page, mode;
    bool masked;
    DmaSyncFn sync;
} DmaChannel;

typedef struct Dma {
    DmaChannel ch[DMA_CHANNELS];
    uint8_t command, tc_status;
    bool flipflop; /* false: the next address/count access is the low byte */
} Dma;

static Dma *dma_state(Machine *m) { return m->dma; }

/* Lets the device catch up on transfers that are due before the guest looks at or changes them. */
static void sync_channel(Machine *m, int ch)
{
    DmaSyncFn fn = dma_state(m)->ch[ch].sync;
    if (fn)
        fn(m);
}

static void sync_all(Machine *m)
{
    for (int ch = 0; ch < DMA_CHANNELS; ch++)
        sync_channel(m, ch);
}

/* Bytes in the VGA window are not read: a VGA read latches planes, which DMA would not do. */
static uint8_t bus_read(Machine *m, uint32_t linear)
{
    return mem_is_vga(linear) ? 0xFF : m->mem[linear];
}

int dma_read(Machine *m, int ch, uint8_t *dst, int len, bool *tc)
{
    Dma *d = dma_state(m);
    DmaChannel *c = &d->ch[ch];
    if (tc)
        *tc = false;
    int n = 0;
    while (n < len && !c->masked && !(d->command & CMD_DISABLE)) {
        dst[n++] = bus_read(m, (uint32_t)c->page << 16 | c->addr);
        c->addr = (uint16_t)(c->mode & MODE_DECREMENT ? c->addr - 1 : c->addr + 1);
        if (c->count-- != 0)
            continue;
        d->tc_status |= (uint8_t)(1u << ch);
        if (c->mode & MODE_AUTO_INIT) {
            c->addr = c->base_addr;
            c->count = c->base_count;
        } else {
            c->masked = true;
        }
        if (tc)
            *tc = true;
        break;
    }
    return n;
}

void dma_set_sync(Machine *m, int ch, DmaSyncFn fn) { dma_state(m)->ch[ch].sync = fn; }

static uint8_t flip(Dma *d, uint16_t v)
{
    bool hi = d->flipflop;
    d->flipflop = !d->flipflop;
    return (uint8_t)(hi ? v >> 8 : v);
}

static void write_word(Dma *d, uint16_t *base, uint16_t *cur, uint8_t v)
{
    *base = d->flipflop ? (uint16_t)((*base & 0x00FF) | v << 8) : (uint16_t)((*base & 0xFF00) | v);
    *cur = *base;
    d->flipflop = !d->flipflop;
}

static int page_channel(uint16_t port)
{
    for (int ch = 0; ch < DMA_CHANNELS; ch++)
        if (PAGE_PORTS[ch] == port)
            return ch;
    return -1;
}

static uint32_t dma_in(Machine *m, uint16_t port, int size)
{
    Dma *d = dma_state(m);
    int pch = page_channel(port);
    if (pch >= 0)
        return d->ch[pch].page;
    if (port < REG_STATUS) {
        DmaChannel *c = &d->ch[port >> 1];
        sync_channel(m, port >> 1);
        return flip(d, port & 1 ? c->count : c->addr);
    }
    if (port == REG_STATUS) {
        sync_all(m);
        uint8_t v = d->tc_status;
        d->tc_status = 0;
        return v;
    }
    if (port == REG_ALL_MASKS) {
        uint8_t v = 0xF0;
        for (int ch = 0; ch < DMA_CHANNELS; ch++)
            v |= (uint8_t)(d->ch[ch].masked << ch);
        return v;
    }
    return 0xFF;
}

static void set_masks(Machine *m, uint8_t bits)
{
    sync_all(m);
    for (int ch = 0; ch < DMA_CHANNELS; ch++)
        dma_state(m)->ch[ch].masked = bits >> ch & 1;
}

static void dma_out(Machine *m, uint16_t port, uint32_t value, int size)
{
    Dma *d = dma_state(m);
    uint8_t v = (uint8_t)value;
    int pch = page_channel(port);
    if (pch >= 0) {
        sync_channel(m, pch);
        d->ch[pch].page = v;
        return;
    }
    if (port < REG_STATUS) {
        DmaChannel *c = &d->ch[port >> 1];
        sync_channel(m, port >> 1);
        if (port & 1)
            write_word(d, &c->base_count, &c->count, v);
        else
            write_word(d, &c->base_addr, &c->addr, v);
        return;
    }
    switch (port) {
    case REG_STATUS:
        sync_all(m);
        d->command = v;
        break;
    case REG_SINGLE_MASK:
        sync_channel(m, v & 3);
        d->ch[v & 3].masked = v & MASK_SET;
        break;
    case REG_MODE:
        sync_channel(m, v & 3);
        d->ch[v & 3].mode = v;
        break;
    case REG_CLEAR_FLIPFLOP: d->flipflop = false; break;
    case REG_MASTER_CLEAR:
        set_masks(m, 0x0F);
        d->command = d->tc_status = 0;
        d->flipflop = false;
        break;
    case REG_CLEAR_MASKS: set_masks(m, 0); break;
    case REG_ALL_MASKS: set_masks(m, v & 0x0F); break;
    default: break; /* REG_REQUEST: software requests only matter for memory-to-memory */
    }
}

void dma_init(Machine *m)
{
    Dma *d = calloc(1, sizeof *d);
    if (!d)
        fatal("out of memory");
    m->dma = d;
    for (int ch = 0; ch < DMA_CHANNELS; ch++) {
        d->ch[ch].masked = true;
        port_register(m, PAGE_PORTS[ch], PAGE_PORTS[ch], dma_in, dma_out);
    }
    port_register(m, 0x00, DMA_PORT_LAST, dma_in, dma_out);
}

void dma_shutdown(Machine *m)
{
    free(m->dma);
    m->dma = NULL;
}
