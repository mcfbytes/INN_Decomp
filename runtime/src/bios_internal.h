/* Shared by the BIOS translation units: ROM layout, BDA offsets and the ROM code emitter. */
#ifndef DOSRT_BIOS_INTERNAL_H
#define DOSRT_BIOS_INTERNAL_H

#include <stddef.h>

#include "devices.h"

enum {
    BIOS_SEG = 0xF000,
    ROM_FONT16_OFF = 0x0000,  /* 4 KB 8x16 font, below the callback stubs */
    ROM_FONT8_OFF = 0xE000,   /* 2 KB 8x8 font */
    ROM_FONT8_HIGH_OFF = 0xE400,
    ROM_CODE_OFF = 0xE800,    /* trampolines and tables, bump-allocated */
    ROM_CODE_END = 0xFF00,
    ROM_DUMMY_IRET_OFF = 0xFF53,
    ROM_DATE_OFF = 0xFFF5,
    ROM_MODEL_OFF = 0xFFFE,
    ROM_MODEL_AT = 0xFC,

    BDA_SEG = 0x0040, BDA_BASE = 0x400,
    BDA_COM_PORTS = 0x00, BDA_EQUIPMENT = 0x10, BDA_MEMORY_KB = 0x13,
    BDA_KBD_FLAGS0 = 0x17, BDA_KBD_FLAGS1 = 0x18, BDA_KBD_HEAD = 0x1A, BDA_KBD_TAIL = 0x1C,
    BDA_KBD_BUFFER = 0x1E, BDA_KBD_BUFFER_END = 0x3E,
    BDA_VIDEO_MODE = 0x49, BDA_COLUMNS = 0x4A, BDA_PAGE_SIZE = 0x4C, BDA_PAGE_START = 0x4E,
    BDA_CURSOR_POS = 0x50, BDA_CURSOR_SHAPE = 0x60, BDA_ACTIVE_PAGE = 0x62, BDA_CRTC_PORT = 0x63,
    BDA_MODE_CONTROL = 0x65, BDA_PALETTE = 0x66,
    BDA_TICKS = 0x6C, BDA_MIDNIGHT = 0x70, BDA_BREAK = 0x71,
    BDA_KBD_START_PTR = 0x80, BDA_KBD_END_PTR = 0x82,
    BDA_ROWS_MINUS_1 = 0x84, BDA_CHAR_HEIGHT = 0x85, BDA_VIDEO_CTL = 0x87, BDA_VIDEO_SWITCHES = 0x88,
    BDA_VGA_FLAGS = 0x89, BDA_KBD_MODE = 0x96, BDA_KBD_LEDS = 0x97,

    PIC_EOI_CMD = 0x20,
};

static inline uint8_t bda_rd8(Machine *m, int off) { return m->mem[BDA_BASE + off]; }
static inline uint16_t bda_rd16(Machine *m, int off)
{
    return (uint16_t)(m->mem[BDA_BASE + off] | m->mem[BDA_BASE + off + 1] << 8);
}
static inline void bda_wr8(Machine *m, int off, uint8_t v) { m->mem[BDA_BASE + off] = v; }
static inline void bda_wr16(Machine *m, int off, uint16_t v)
{
    m->mem[BDA_BASE + off] = (uint8_t)v;
    m->mem[BDA_BASE + off + 1] = (uint8_t)(v >> 8);
}

/* Copies code into the ROM code area; returns its offset in BIOS_SEG. */
uint16_t bios_rom_emit(Machine *m, const uint8_t *code, size_t len);
/* Emits FE 38 id for fn, so ROM code can call into C mid-routine. */
uint16_t bios_rom_callback(Machine *m, const char *name, CallbackFn fn);
void bios_set_vector(Machine *m, int vector, uint16_t seg, uint16_t off);
/* Points vector at: callback fn, then EOI to the PIC(s), then IRET. */
void bios_irq_handler(Machine *m, int vector, const char *name, CallbackFn fn, bool slave);
/* Leaves the IRET-on-return frame alone but lets interrupts in while a call blocks. */
CallbackResult bios_block(Machine *m);

void bios_video_init(Machine *m);
void bios_kbd_init(Machine *m);

#endif
