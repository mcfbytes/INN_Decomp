/* VGA scan-out: turns planes, text buffer and DAC into an RGBA frame; power-on palettes. */
#include <stdio.h>

#include "font.h"
#include "vga_internal.h"

enum {
    TEXT_CELL_W = 8, TEXT_CELL_H = 16, TEXT_COLS = 80,
    EGA_PRIMARY = 42, EGA_SECONDARY = 21, DAC_MAX = 63,
    GRAY_LEVELS = 16, HUE_STEPS = 24, HUE_SEGMENTS = 6, HUE_RAMP = 4,
    FIRST_GRAY = 16, FIRST_HUE = 32,
    CURSOR_BLINK_US = 266000,
    OVERFLOW_VDE8 = 0x02, OVERFLOW_VDE9 = 0x40, MAX_SCAN_DOUBLE = 0x80, MAX_SCAN_LINES = 0x1F,
};

/* ---- palettes ---- */

/* EGA 6-bit colour rgbRGB: primary bits count 42, secondary 21. */
static void ega_color(uint8_t c, uint8_t out[3])
{
    out[0] = (uint8_t)((c >> 2 & 1) * EGA_PRIMARY + (c >> 5 & 1) * EGA_SECONDARY);
    out[1] = (uint8_t)((c >> 1 & 1) * EGA_PRIMARY + (c >> 4 & 1) * EGA_SECONDARY);
    out[2] = (uint8_t)((c & 1) * EGA_PRIMARY + (c >> 3 & 1) * EGA_SECONDARY);
}

static const uint8_t GRAYS[GRAY_LEVELS] = { 0, 5, 8, 11, 14, 17, 20, 24, 28, 32, 36, 40, 45, 50, 56, 63 };
/* The 256-colour hue rings: three brightnesses, each at three saturations (low component). */
static const uint8_t RING_HIGH[3] = { 63, 28, 16 };
static const uint8_t RING_LOW_PERCENT[3] = { 0, 49, 71 };

/* Walks the colour wheel from blue: R up, B down, G up, R down, B up, G down. */
static void hue_ring(uint8_t (*out)[3], uint8_t lo, uint8_t hi)
{
    static const int8_t STEP[HUE_SEGMENTS][2] = { { 0, 1 }, { 2, -1 }, { 1, 1 }, { 0, -1 }, { 2, 1 }, { 1, -1 } };
    int rgb[3] = { lo, lo, hi };
    for (int seg = 0; seg < HUE_SEGMENTS; seg++) {
        for (int k = 0; k < HUE_RAMP; k++) {
            uint8_t *dst = out[seg * HUE_RAMP + k];
            for (int c = 0; c < 3; c++)
                dst[c] = (uint8_t)rgb[c];
            int comp = STEP[seg][0];
            rgb[comp] += STEP[seg][1] * (hi - lo) / HUE_RAMP;
        }
        rgb[STEP[seg][0]] = STEP[seg][1] > 0 ? hi : lo;
    }
}

void vga_default_palette(Vga *v, bool vga256)
{
    for (int i = 0; i < VGA_DAC_SIZE; i++)
        v->dac[i][0] = v->dac[i][1] = v->dac[i][2] = 0;
    if (!vga256) {
        for (int i = 0; i < 64; i++)
            ega_color((uint8_t)i, v->dac[i]);
        return;
    }
    static const uint8_t EGA16[ATTR_PALETTE_REGS] = { 0, 1, 2, 3, 4, 5, 20, 7, 56, 57, 58, 59, 60, 61, 62, 63 };
    for (int i = 0; i < ATTR_PALETTE_REGS; i++)
        ega_color(EGA16[i], v->dac[i]);
    for (int i = 0; i < GRAY_LEVELS; i++)
        v->dac[FIRST_GRAY + i][0] = v->dac[FIRST_GRAY + i][1] = v->dac[FIRST_GRAY + i][2] = GRAYS[i];
    int idx = FIRST_HUE;
    for (int b = 0; b < 3; b++)
        for (int s = 0; s < 3; s++, idx += HUE_STEPS)
            hue_ring(&v->dac[idx], (uint8_t)(RING_HIGH[b] * RING_LOW_PERCENT[s] / 100), RING_HIGH[b]);
}

static uint32_t dac_rgba(const Vga *v, uint8_t index)
{
    const uint8_t *c = v->dac[index & v->dac_mask];
    uint32_t r = c[0] * 255u / DAC_MAX, g = c[1] * 255u / DAC_MAX, b = c[2] * 255u / DAC_MAX;
    return 0xFF000000u | r << 16 | g << 8 | b;
}

/* Attribute-controller lookup for 4-bit pixels and text colours. */
static uint32_t attr_rgba(const Vga *v, uint8_t color)
{
    uint8_t pal = v->attr[color & v->attr[ATTR_PLANE_ENABLE] & 0x0F] & 0x3F;
    return dac_rgba(v, (uint8_t)((v->attr[ATTR_COLOR_SELECT] & 0x0C) << 4 | pal));
}

/* ---- geometry ---- */

int vga_display_lines(const Vga *v)
{
    int vde = v->crtc[CRTC_V_DISPLAY_END] | (v->crtc[CRTC_OVERFLOW] & OVERFLOW_VDE8) << 7 |
              (v->crtc[CRTC_OVERFLOW] & OVERFLOW_VDE9) << 3;
    int lines = vde + 1;
    uint8_t ms = v->crtc[CRTC_MAX_SCAN];
    if (ms & MAX_SCAN_DOUBLE)
        lines /= 2;
    if (v->gc[GC_MISC] & GC_MISC_GRAPHICS)
        lines /= (ms & MAX_SCAN_LINES) + 1;
    return lines;
}

int vga_display_width(const Vga *v)
{
    int chars = v->crtc[CRTC_H_DISPLAY_END] + 1;
    return (v->attr[ATTR_MODE] & ATTR_MODE_8BIT) ? chars * 4 : chars * 8;
}

static int clamp(int x, int hi) { return x < 1 ? 1 : x > hi ? hi : x; }

/* ---- scan-out ---- */

static void render_256(const Vga *v, uint32_t *out, int w, int h)
{
    uint32_t start = vga_crtc_start(v), stride = v->crtc[CRTC_OFFSET] * 2u;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t off = (start + y * stride + (uint32_t)(x >> 2)) & 0xFFFF;
            out[y * w + x] = dac_rgba(v, v->planes[x & 3][off]);
        }
}

static void render_planar(const Vga *v, uint32_t *out, int w, int h)
{
    uint32_t start = vga_crtc_start(v), stride = v->crtc[CRTC_OFFSET] * 2u;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t off = (start + y * stride + (uint32_t)(x >> 3)) & 0xFFFF;
            int shift = 7 - (x & 7);
            uint8_t c = 0;
            for (int p = 0; p < 4; p++)
                c |= (uint8_t)((v->planes[p][off] >> shift & 1) << p);
            out[y * w + x] = attr_rgba(v, c);
        }
}

static bool cursor_on_row(const Vga *v, int row_in_cell, uint64_t now_us)
{
    if ((now_us / CURSOR_BLINK_US) & 1)
        return false;
    int start = v->crtc[CRTC_CURSOR_START], end = v->crtc[CRTC_CURSOR_END] & MAX_SCAN_LINES;
    if (start & 0x20)
        return false; /* cursor disabled */
    return row_in_cell >= (start & MAX_SCAN_LINES) && row_in_cell <= end;
}

static void render_cell(const Vga *v, uint32_t *out, int w, int col, int row, bool cursor, uint64_t now)
{
    uint32_t at = ((vga_crtc_start(v) + (uint32_t)(row * TEXT_COLS + col)) * 2) & (VGA_TEXT_SIZE - 1);
    uint8_t ch = v->text[at], attr = v->text[at + 1];
    uint8_t bg = attr >> 4;
    bool blink_hidden = false;
    if (v->attr[ATTR_MODE] & ATTR_MODE_BLINK) {
        blink_hidden = (bg & 8) && ((now / CURSOR_BLINK_US) & 2);
        bg &= 7;
    }
    uint32_t fg_rgba = attr_rgba(v, attr & 0x0F), bg_rgba = attr_rgba(v, bg);
    for (int y = 0; y < TEXT_CELL_H; y++) {
        uint8_t bits = blink_hidden ? 0 : font_8x16[ch][y];
        if (cursor && cursor_on_row(v, y, now))
            bits = 0xFF;
        uint32_t *px = &out[(row * TEXT_CELL_H + y) * w + col * TEXT_CELL_W];
        for (int x = 0; x < TEXT_CELL_W; x++)
            px[x] = (bits & (0x80 >> x)) ? fg_rgba : bg_rgba;
    }
}

static void render_text(Machine *m, const Vga *v, uint32_t *out, int w, int h)
{
    int rows = h / TEXT_CELL_H;
    uint16_t cursor = (uint16_t)(v->crtc[CRTC_CURSOR_HI] << 8 | v->crtc[CRTC_CURSOR_LO]);
    uint16_t cursor_cell = (uint16_t)(cursor - vga_crtc_start(v));
    for (int row = 0; row < rows; row++)
        for (int col = 0; col < TEXT_COLS; col++)
            render_cell(v, out, w, col, row, cursor_cell == row * TEXT_COLS + col, m->virtual_us);
}

void vga_render(Machine *m, uint32_t *rgba_out, int *w, int *h)
{
    const Vga *v = vga_state(m);
    if (!(v->gc[GC_MISC] & GC_MISC_GRAPHICS)) {
        *w = TEXT_COLS * TEXT_CELL_W;
        *h = clamp(vga_display_lines(v) / TEXT_CELL_H * TEXT_CELL_H, VGA_MAX_H);
        render_text(m, v, rgba_out, *w, *h);
        return;
    }
    *w = clamp(vga_display_width(v), VGA_MAX_W);
    *h = clamp(vga_display_lines(v), VGA_MAX_H);
    if (v->attr[ATTR_MODE] & ATTR_MODE_8BIT)
        render_256(v, rgba_out, *w, *h);
    else
        render_planar(v, rgba_out, *w, *h);
}

int vga_write_ppm(Machine *m, const char *path)
{
    static uint32_t frame[VGA_MAX_W * VGA_MAX_H];
    int w, h;
    vga_render(m, frame, &w, &h);
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
