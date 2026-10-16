/* Host video: mode, DAC, CRTC start and cursor stay in the VGA device's state (one copy for the BIOS,
 * the mouse and the hand-off); pixels and text cells are plain guest memory while m->native_video. */
#include <stdio.h>
#include <string.h>

#include "host_video.h"

#include "devices.h"
#include "font.h"
#include "lift_semantic.h"
#include "native.h"
#include "vga_internal.h"

enum {
    PIXELS_BASE = 0xA0000, TEXT_BASE = 0xB8000, WINDOW_BASE = 0xA0000, WINDOW_SIZE = 0x20000,
    PIXELS_W = 320, PIXELS_H = 200, PIXEL_WRAP = 0xFFFF,
    TEXT_COLS = 80, TEXT_ROWS = 25, CELL_W = 8, CELL_H = 16,
    FRAME_US = 14286,          /* 70 Hz, as vga.c */
    RETRACE_DIV = 10,          /* retrace is the first tenth of each frame */
    DAC_MAX = 63, CURSOR_BLINK_US = 266000, MAX_SCAN_LINES = 0x1F, CURSOR_DISABLED = 0x20,
};

static void safepoint(Machine *m)
{
    if (m->native)
        native_safepoint(m);
}

static uint8_t *window(Machine *m) { return m->mem + WINDOW_BASE; }

/* Takes the VGA device's picture into guest memory and routes A0000-BFFFF to it. */
void host_video_from_vga(Machine *m)
{
    uint8_t mode, dac[VGA_DAC_SIZE][3];
    if (m->native_video)
        return;
    if (!vga_read_state(m, &mode, dac, window(m)))
        fatal("host video: VGA mode %02X cannot be taken over", vga_mode(m));
    m->native_video = true;
}

void host_video_init(Machine *m)
{
    if (!m->vga)
        vga_init(m);
    host_video_from_vga(m);
}

/* Gives the picture back to the VGA device and routes A0000-BFFFF to it again. */
void host_video_to_vga(Machine *m)
{
    if (!m->native_video)
        return;
    Vga *v = vga_state(m);
    if (!vga_load_state(m, v->mode, v->dac, window(m)))
        fatal("host video: mode %02X cannot be handed over", v->mode);
    m->native_video = false;
}

void host_video_set_mode(Machine *m, uint8_t mode, bool clear)
{
    safepoint(m);
    if (!m->native_video) {
        vga_set_mode(m, mode, clear);
        return;
    }
    if (mode != HOST_VIDEO_TEXT80 && mode != HOST_VIDEO_320X200X8)
        fatal("host video: mode %02X", mode);
    vga_set_mode(m, mode, false);
    if (clear) {
        memset(m->mem + PIXELS_BASE, 0, 0x10000);
        for (uint32_t i = 0; i < VGA_TEXT_SIZE; i += 2) {
            m->mem[TEXT_BASE + i] = ' ';
            m->mem[TEXT_BASE + i + 1] = 0x07;
        }
    }
}

uint8_t host_video_mode(const Machine *m)
{
    return m->vga ? ((const Vga *)m->vga)->mode : 0;
}

/* After a whole run of colours the write/read index sits where 3C8h/3C7h plus the data bytes leave it,
 * so a shadow snapshot matches the original port sequence. */
static void dac_index_after(Machine *m, uint8_t first, int count, bool reading)
{
    Vga *v = vga_state(m);
    uint8_t next = (uint8_t)(first + count);
    if (reading) {
        v->dac_read = next;
        v->dac_read_sub = 0;
        v->dac_last_read = true;
    } else {
        v->dac_write = next;
        v->dac_write_sub = 0;
        v->dac_last_read = false;
    }
}

void host_video_set_dac(Machine *m, uint8_t first, int count, const uint8_t *rgb6)
{
    safepoint(m);
    if (count <= 0)
        return;
    for (int i = 0; i < count; i++) {
        const uint8_t *p = rgb6 + 3 * i;
        uint8_t index = (uint8_t)(first + i);
        vga_dac_set(m, index, p[0], p[1], p[2]);
        lift_semantic_event(m, SEM_DAC_SET, index, (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]);
    }
    dac_index_after(m, first, count, false);
}

void host_video_get_dac(Machine *m, uint8_t first, int count, uint8_t *rgb6)
{
    safepoint(m);
    if (count <= 0)
        return;
    for (int i = 0; i < count; i++) {
        uint8_t live[3];
        uint8_t index = (uint8_t)(first + i);
        vga_dac_get(m, index, live);
        uint32_t packed = lift_semantic_read(m, SEM_DAC_GET, index,
                                             (uint32_t)live[0] << 16 | (uint32_t)live[1] << 8 | live[2]);
        rgb6[3 * i] = (uint8_t)(packed >> 16);
        rgb6[3 * i + 1] = (uint8_t)(packed >> 8);
        rgb6[3 * i + 2] = (uint8_t)packed;
    }
    dac_index_after(m, first, count, true);
}

void host_video_set_start(Machine *m, uint16_t start)
{
    Vga *v = vga_state(m);
    safepoint(m);
    v->crtc[CRTC_START_HI] = (uint8_t)(start >> 8);
    v->crtc[CRTC_START_LO] = (uint8_t)start;
}

/* The yield presents a frame when one is due; the wait ends at the start of the next 70 Hz period. */
void host_video_wait_retrace(Machine *m)
{
    safepoint(m);
    native_yield(m);
    native_wait_until(m, (m->virtual_us / FRAME_US + 1) * FRAME_US);
}

bool host_video_in_retrace(Machine *m)
{
    safepoint(m);
    return m->virtual_us % FRAME_US < FRAME_US / RETRACE_DIV;
}

/* ---- scan-out (the same pixels as vga_render for modes 03h and 13h) ---- */

static uint32_t dac_argb(const Vga *v, uint8_t index)
{
    const uint8_t *c = v->dac[index & v->dac_mask];
    return 0xFF000000u | (c[0] * 255u / DAC_MAX) << 16 | (c[1] * 255u / DAC_MAX) << 8 | (c[2] * 255u / DAC_MAX);
}

static uint32_t attr_argb(const Vga *v, uint8_t color)
{
    uint8_t pal = v->attr[color & v->attr[ATTR_PLANE_ENABLE] & 0x0F] & 0x3F;
    return dac_argb(v, (uint8_t)((v->attr[ATTR_COLOR_SELECT] & 0x0C) << 4 | pal));
}

static void render_pixels(Machine *m, const Vga *v, uint32_t *out)
{
    const uint8_t *px = m->mem + PIXELS_BASE;
    uint32_t start = vga_crtc_start(v) * 4u; /* chain-4: the start address counts dwords */
    uint32_t palette[VGA_DAC_SIZE];
    for (int i = 0; i < VGA_DAC_SIZE; i++)
        palette[i] = dac_argb(v, (uint8_t)i);
    for (int y = 0; y < PIXELS_H; y++)
        for (int x = 0; x < PIXELS_W; x++)
            out[y * PIXELS_W + x] = palette[px[(start + (uint32_t)(y * PIXELS_W + x)) & PIXEL_WRAP]];
}

static bool cursor_row(const Vga *v, int row_in_cell, uint64_t now_us)
{
    if ((now_us / CURSOR_BLINK_US) & 1)
        return false;
    int start = v->crtc[CRTC_CURSOR_START], end = v->crtc[CRTC_CURSOR_END] & MAX_SCAN_LINES;
    if (start & CURSOR_DISABLED)
        return false;
    return row_in_cell >= (start & MAX_SCAN_LINES) && row_in_cell <= end;
}

static void render_cell(Machine *m, const Vga *v, uint32_t *out, int col, int row, bool cursor)
{
    int width = TEXT_COLS * CELL_W;
    uint32_t at = ((vga_crtc_start(v) + (uint32_t)(row * TEXT_COLS + col)) * 2) & (VGA_TEXT_SIZE - 1);
    uint8_t ch = m->mem[TEXT_BASE + at], attr = m->mem[TEXT_BASE + at + 1];
    uint8_t bg = attr >> 4;
    bool blink_hidden = false;
    if (v->attr[ATTR_MODE] & ATTR_MODE_BLINK) {
        blink_hidden = (bg & 8) && ((m->virtual_us / CURSOR_BLINK_US) & 2);
        bg &= 7;
    }
    uint32_t fg_argb = attr_argb(v, attr & 0x0F), bg_argb = attr_argb(v, bg);
    for (int y = 0; y < CELL_H; y++) {
        uint8_t bits = blink_hidden ? 0 : font_8x16[ch][y];
        if (cursor && cursor_row(v, y, m->virtual_us))
            bits = 0xFF;
        uint32_t *px = &out[(row * CELL_H + y) * width + col * CELL_W];
        for (int x = 0; x < CELL_W; x++)
            px[x] = (bits & (0x80 >> x)) ? fg_argb : bg_argb;
    }
}

void host_video_render(Machine *m, uint32_t *argb, int *w, int *h)
{
    const Vga *v = vga_state(m);
    if (v->mode == HOST_VIDEO_320X200X8) {
        *w = PIXELS_W, *h = PIXELS_H;
        render_pixels(m, v, argb);
        return;
    }
    *w = TEXT_COLS * CELL_W, *h = TEXT_ROWS * CELL_H;
    uint16_t cursor = (uint16_t)(v->crtc[CRTC_CURSOR_HI] << 8 | v->crtc[CRTC_CURSOR_LO]);
    uint16_t cursor_cell = (uint16_t)(cursor - vga_crtc_start(v));
    for (int row = 0; row < TEXT_ROWS; row++)
        for (int col = 0; col < TEXT_COLS; col++)
            render_cell(m, v, argb, col, row, cursor_cell == row * TEXT_COLS + col);
}

int host_video_write_ppm(Machine *m, const char *path)
{
    static uint32_t frame[VGA_MAX_W * VGA_MAX_H];
    int w, h;
    host_video_render(m, frame, &w, &h);
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t rgb[3] = { (uint8_t)(frame[i] >> 16), (uint8_t)(frame[i] >> 8), (uint8_t)frame[i] };
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f) == 0 ? 0 : -1;
}
