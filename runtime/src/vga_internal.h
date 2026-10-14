/* VGA state shared by vga.c (registers, memory), vga_render.c and bios_video.c (INT 10h). */
#ifndef DOSRT_VGA_INTERNAL_H
#define DOSRT_VGA_INTERNAL_H

#include "devices.h"

enum {
    VGA_PLANE_SIZE = 0x10000,
    VGA_TEXT_BASE = 0xB8000, VGA_TEXT_SIZE = 0x8000,
    VGA_SEQ_REGS = 5, VGA_CRTC_REGS = 25, VGA_GC_REGS = 9, VGA_ATTR_REGS = 21,
    VGA_DAC_SIZE = 256,

    SEQ_MAP_MASK = 2, SEQ_MEMORY_MODE = 4, SEQ_CHAIN4 = 0x08,
    CRTC_H_DISPLAY_END = 0x01, CRTC_OVERFLOW = 0x07, CRTC_MAX_SCAN = 0x09,
    CRTC_CURSOR_START = 0x0A, CRTC_CURSOR_END = 0x0B,
    CRTC_START_HI = 0x0C, CRTC_START_LO = 0x0D, CRTC_CURSOR_HI = 0x0E, CRTC_CURSOR_LO = 0x0F,
    CRTC_V_DISPLAY_END = 0x12, CRTC_OFFSET = 0x13,
    GC_SET_RESET = 0, GC_ENABLE_SET_RESET = 1, GC_COLOR_COMPARE = 2, GC_ROTATE = 3,
    GC_READ_MAP = 4, GC_MODE = 5, GC_MISC = 6, GC_COLOR_DONT_CARE = 7, GC_BIT_MASK = 8,
    GC_MISC_GRAPHICS = 0x01, GC_MODE_READ1 = 0x08,
    ATTR_MODE = 0x10, ATTR_PLANE_ENABLE = 0x12, ATTR_COLOR_SELECT = 0x14,
    ATTR_MODE_GRAPHICS = 0x01, ATTR_MODE_BLINK = 0x08, ATTR_MODE_8BIT = 0x40,
    ATTR_PALETTE_REGS = 16,
};

typedef struct VgaModeRegs {
    uint8_t mode, misc;
    uint8_t seq[VGA_SEQ_REGS];
    uint8_t crtc[VGA_CRTC_REGS];
    uint8_t gc[VGA_GC_REGS];
    uint8_t attr[VGA_ATTR_REGS];
} VgaModeRegs;

typedef struct Vga {
    uint8_t mode;
    uint8_t planes[4][VGA_PLANE_SIZE];
    uint8_t text[VGA_TEXT_SIZE];
    uint8_t latch[4];
    uint8_t misc;
    uint8_t seq[VGA_SEQ_REGS], seq_index;
    uint8_t crtc[VGA_CRTC_REGS], crtc_index;
    uint8_t gc[VGA_GC_REGS], gc_index;
    uint8_t attr[VGA_ATTR_REGS], attr_index;
    bool attr_data_next;
    uint8_t dac[VGA_DAC_SIZE][3];
    uint8_t dac_write, dac_read, dac_write_sub, dac_read_sub, dac_mask;
    bool dac_last_read;
    uint32_t status_reads;
} Vga;

static inline Vga *vga_state(Machine *m) { return m->vga; }

/* Mode-aware pixel access used by INT 10h (AH=0Ch/0Dh and glyph drawing). */
void vga_put_pixel(Machine *m, int x, int y, uint8_t color, bool xor_mode);
uint8_t vga_get_pixel(Machine *m, int x, int y);
/* Fills the DAC with the power-on palette for a mode (16-colour or 256-colour). */
void vga_default_palette(Vga *v, bool vga256);
uint16_t vga_crtc_start(const Vga *v);
/* Display geometry decoded from the CRTC: pixel or character cells. */
int vga_display_width(const Vga *v);
int vga_display_lines(const Vga *v);

#endif
