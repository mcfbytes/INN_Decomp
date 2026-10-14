/* INT 10h video BIOS: mode set, cursor, text output (text and graphics modes), pixels,
 * palette and DAC services, font pointers and VGA identification. */
#include "bios_internal.h"
#include "font.h"
#include "vga_internal.h"

enum {
    VEC_VIDEO = 0x10,
    CRTC_PORT_COLOR = 0x3D4,
    MODE_TEXT = 0x03, MODE_NO_CLEAR = 0x80, MODE_MASK = 0x7F,
    TEXT_PAGE_SIZE = 0x1000, MAX_PAGES = 8,
    CURSOR_SHAPE_DEFAULT = 0x0D0E,
    CH_BELL = 0x07, CH_BACKSPACE = 0x08, CH_LF = 0x0A, CH_CR = 0x0D,
    DEFAULT_ATTR = 0x07, XOR_COLOR = 0x80,
    OVERSCAN_REG = 0x11,
    VGA_COLOR_DCC = 0x08, VGA_MEMORY_256K = 0x03,
    VESA_FAILED = 0x0100,
};

/* What the BIOS tells programs about each mode. */
typedef struct ModeInfo {
    uint8_t mode, columns, rows, char_height;
    uint16_t page_size;
} ModeInfo;

static const ModeInfo MODE_INFO[] = {
    { 0x03, 80, 25, 16, TEXT_PAGE_SIZE },
    { 0x0D, 40, 25, 8, 0x2000 },
    { 0x12, 80, 30, 16, 0xA000 },
    { 0x13, 40, 25, 8, 0xFA00 },
};

static const ModeInfo *mode_info(uint8_t mode)
{
    for (size_t i = 0; i < sizeof MODE_INFO / sizeof MODE_INFO[0]; i++)
        if (MODE_INFO[i].mode == mode)
            return &MODE_INFO[i];
    return NULL;
}

static bool is_text(Machine *m) { return vga_mode(m) == MODE_TEXT; }

/* ---- cursor ---- */

static void sync_hw_cursor(Machine *m)
{
    Vga *v = vga_state(m);
    uint8_t page = bda_rd8(m, BDA_ACTIVE_PAGE);
    uint16_t pos = bda_rd16(m, BDA_CURSOR_POS + page * 2);
    uint16_t cell = (uint16_t)(bda_rd16(m, BDA_PAGE_START) / 2 + (pos >> 8) * bda_rd16(m, BDA_COLUMNS) + (pos & 0xFF));
    v->crtc[CRTC_CURSOR_HI] = (uint8_t)(cell >> 8);
    v->crtc[CRTC_CURSOR_LO] = (uint8_t)cell;
}

static void set_cursor(Machine *m, uint8_t page, uint8_t col, uint8_t row)
{
    bda_wr16(m, BDA_CURSOR_POS + (page & (MAX_PAGES - 1)) * 2, (uint16_t)(row << 8 | col));
    if (page == bda_rd8(m, BDA_ACTIVE_PAGE))
        sync_hw_cursor(m);
}

static uint16_t get_cursor(Machine *m, uint8_t page)
{
    return bda_rd16(m, BDA_CURSOR_POS + (page & (MAX_PAGES - 1)) * 2);
}

/* ---- cells ---- */

static uint32_t text_addr(Machine *m, uint8_t page, int col, int row)
{
    return VGA_TEXT_BASE + page * (uint32_t)bda_rd16(m, BDA_PAGE_SIZE) +
           (uint32_t)(row * bda_rd16(m, BDA_COLUMNS) + col) * 2;
}

static void draw_glyph(Machine *m, int col, int row, uint8_t ch, uint8_t color)
{
    int height = bda_rd8(m, BDA_CHAR_HEIGHT);
    bool xor_mode = color & XOR_COLOR;
    for (int y = 0; y < height; y++) {
        uint8_t bits = height == 8 ? font_8x8[ch][y] : font_8x16[ch][y];
        for (int x = 0; x < 8; x++) {
            bool on = bits & (0x80 >> x);
            if (xor_mode && !on)
                continue;
            vga_put_pixel(m, col * 8 + x, row * height + y, on ? (uint8_t)(color & 0x7F) : 0, xor_mode);
        }
    }
}

static void put_cell(Machine *m, uint8_t page, int col, int row, uint8_t ch, uint8_t attr, bool keep_attr)
{
    if (!is_text(m)) {
        draw_glyph(m, col, row, ch, attr);
        return;
    }
    uint32_t a = text_addr(m, page, col, row);
    mem_wr8(m, a, ch);
    if (!keep_attr)
        mem_wr8(m, a + 1, attr);
}

/* Copies one character cell (all its pixels in graphics modes) from src to dst row/col. */
static void copy_cell(Machine *m, uint8_t page, int col, int src_row, int dst_row)
{
    if (is_text(m)) {
        mem_wr16(m, text_addr(m, page, col, dst_row), mem_rd16(m, text_addr(m, page, col, src_row)));
        return;
    }
    int h = bda_rd8(m, BDA_CHAR_HEIGHT);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < 8; x++)
            vga_put_pixel(m, col * 8 + x, dst_row * h + y, vga_get_pixel(m, col * 8 + x, src_row * h + y), false);
}

static void blank_cell(Machine *m, uint8_t page, int col, int row, uint8_t attr)
{
    if (is_text(m)) {
        mem_wr16(m, text_addr(m, page, col, row), (uint16_t)(attr << 8 | ' '));
        return;
    }
    int h = bda_rd8(m, BDA_CHAR_HEIGHT);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < 8; x++)
            vga_put_pixel(m, col * 8 + x, row * h + y, attr, false);
}

typedef struct Window { int top, left, bottom, right; } Window;

/* AH=06h/07h: lines == 0 clears the window. */
static void scroll(Machine *m, uint8_t page, Window w, int lines, bool up, uint8_t attr)
{
    int height = w.bottom - w.top + 1;
    if (lines <= 0 || lines > height)
        lines = height;
    for (int i = 0; i < height; i++) {
        int row = up ? w.top + i : w.bottom - i;
        int src = up ? row + lines : row - lines;
        bool from_inside = up ? src <= w.bottom : src >= w.top;
        for (int col = w.left; col <= w.right; col++) {
            if (from_inside)
                copy_cell(m, page, col, src, row);
            else
                blank_cell(m, page, col, row, attr);
        }
    }
}

static Window full_screen(Machine *m)
{
    return (Window){ 0, 0, bda_rd8(m, BDA_ROWS_MINUS_1), bda_rd16(m, BDA_COLUMNS) - 1 };
}

/* AH=0Eh/13h: control characters act, others print and advance, scrolling at the bottom. */
static void teletype(Machine *m, uint8_t ch, uint8_t color, bool set_attr)
{
    uint8_t page = bda_rd8(m, BDA_ACTIVE_PAGE);
    uint16_t pos = get_cursor(m, page);
    int col = pos & 0xFF, row = pos >> 8;
    int cols = bda_rd16(m, BDA_COLUMNS), last_row = bda_rd8(m, BDA_ROWS_MINUS_1);
    switch (ch) {
    case CH_BELL: return;
    case CH_BACKSPACE: if (col > 0) col--; break;
    case CH_CR: col = 0; break;
    case CH_LF: row++; break;
    default:
        put_cell(m, page, col, row, ch, color, is_text(m) && !set_attr);
        if (++col >= cols) {
            col = 0;
            row++;
        }
    }
    if (row > last_row) {
        uint8_t attr = is_text(m) ? mem_rd8(m, text_addr(m, page, col, last_row) + 1) : 0;
        scroll(m, page, full_screen(m), 1, true, attr);
        row = last_row;
    }
    set_cursor(m, page, (uint8_t)col, (uint8_t)row);
}

/* ---- mode set ---- */

static void set_mode(Machine *m, uint8_t al)
{
    uint8_t mode = al & MODE_MASK;
    if (mode <= 0x02 || mode == 0x07)
        mode = MODE_TEXT; /* other text modes fall back to 80x25 colour */
    if (!vga_set_mode(m, mode, !(al & MODE_NO_CLEAR))) {
        log_msg("int10: unsupported video mode %02X", mode);
        return;
    }
    const ModeInfo *info = mode_info(mode);
    bda_wr8(m, BDA_VIDEO_MODE, mode);
    bda_wr16(m, BDA_COLUMNS, info->columns);
    bda_wr16(m, BDA_PAGE_SIZE, info->page_size);
    bda_wr16(m, BDA_PAGE_START, 0);
    for (int p = 0; p < MAX_PAGES; p++)
        bda_wr16(m, BDA_CURSOR_POS + p * 2, 0);
    bda_wr16(m, BDA_CURSOR_SHAPE, CURSOR_SHAPE_DEFAULT);
    bda_wr8(m, BDA_ACTIVE_PAGE, 0);
    bda_wr16(m, BDA_CRTC_PORT, CRTC_PORT_COLOR);
    bda_wr8(m, BDA_ROWS_MINUS_1, (uint8_t)(info->rows - 1));
    bda_wr8(m, BDA_CHAR_HEIGHT, info->char_height);
    bda_wr8(m, BDA_VIDEO_CTL, 0x60);
    bda_wr8(m, BDA_VIDEO_SWITCHES, 0x09);
    bda_wr8(m, BDA_VGA_FLAGS, 0x11);
    sync_hw_cursor(m);
}

static void set_page(Machine *m, uint8_t page)
{
    page &= MAX_PAGES - 1;
    Vga *v = vga_state(m);
    uint16_t start = (uint16_t)(page * bda_rd16(m, BDA_PAGE_SIZE));
    bda_wr8(m, BDA_ACTIVE_PAGE, page);
    bda_wr16(m, BDA_PAGE_START, start);
    uint16_t crtc_start = is_text(m) ? start / 2 : start;
    v->crtc[CRTC_START_HI] = (uint8_t)(crtc_start >> 8);
    v->crtc[CRTC_START_LO] = (uint8_t)crtc_start;
    sync_hw_cursor(m);
}

/* ---- palette (AH=10h) ---- */

static void dac_block(Machine *m, bool write)
{
    Vga *v = vga_state(m);
    uint32_t addr = real_addr(ES, DX);
    for (int i = 0; i < CX && BX + i < VGA_DAC_SIZE; i++)
        for (int c = 0; c < 3; c++, addr++) {
            if (write)
                v->dac[BX + i][c] = mem_rd8(m, addr) & 0x3F;
            else
                mem_wr8(m, addr, v->dac[BX + i][c]);
        }
}

static void palette_service(Machine *m)
{
    Vga *v = vga_state(m);
    switch (AL) {
    case 0x00: if (BL < ATTR_PALETTE_REGS) v->attr[BL] = BH; break;
    case 0x01: v->attr[OVERSCAN_REG] = BH; break;
    case 0x02:
        for (int i = 0; i < ATTR_PALETTE_REGS; i++)
            v->attr[i] = mem_rd8(m, real_addr(ES, (uint16_t)(DX + i)));
        v->attr[OVERSCAN_REG] = mem_rd8(m, real_addr(ES, (uint16_t)(DX + ATTR_PALETTE_REGS)));
        break;
    case 0x03:
        v->attr[ATTR_MODE] = BL ? (uint8_t)(v->attr[ATTR_MODE] | ATTR_MODE_BLINK)
                                : (uint8_t)(v->attr[ATTR_MODE] & ~ATTR_MODE_BLINK);
        break;
    case 0x07: if (BL < ATTR_PALETTE_REGS) BH = v->attr[BL]; break;
    case 0x08: BH = v->attr[OVERSCAN_REG]; break;
    case 0x09:
        for (int i = 0; i <= ATTR_PALETTE_REGS; i++)
            mem_wr8(m, real_addr(ES, (uint16_t)(DX + i)), v->attr[i < ATTR_PALETTE_REGS ? i : OVERSCAN_REG]);
        break;
    case 0x10:
        v->dac[BL][0] = DH & 0x3F, v->dac[BL][1] = CH & 0x3F, v->dac[BL][2] = CL & 0x3F;
        break;
    case 0x12: dac_block(m, true); break;
    case 0x15: DH = v->dac[BL][0], CH = v->dac[BL][1], CL = v->dac[BL][2]; break;
    case 0x17: dac_block(m, false); break;
    case 0x1A: BX = 0; break;
    default: break;
    }
}

/* ---- AH=11h/30h font pointers ---- */

static void font_info(Machine *m)
{
    static const uint16_t FONT_OFFSETS[8] = {
        0, 0, ROM_FONT16_OFF, ROM_FONT8_OFF, ROM_FONT8_HIGH_OFF, ROM_FONT16_OFF, ROM_FONT16_OFF, ROM_FONT16_OFF,
    };
    uint16_t seg = BIOS_SEG, off = FONT_OFFSETS[BH & 7];
    if (BH <= 1) {
        int vec = BH == 0 ? 0x1F : 0x43;
        off = mem_rd16(m, (uint32_t)vec * 4);
        seg = mem_rd16(m, (uint32_t)vec * 4 + 2);
    }
    cpu_set_seg(&m->cpu, S_ES, seg);
    BP = off;
    CX = bda_rd8(m, BDA_CHAR_HEIGHT);
    DL = bda_rd8(m, BDA_ROWS_MINUS_1);
}

/* ---- dispatcher ---- */

static void write_chars(Machine *m, bool with_attr)
{
    uint16_t pos = get_cursor(m, BH);
    int col = pos & 0xFF, row = pos >> 8, cols = bda_rd16(m, BDA_COLUMNS);
    for (int i = 0; i < CX; i++, col++) {
        if (col >= cols) {
            col = 0;
            row++;
        }
        put_cell(m, BH, col, row, AL, BL, !with_attr && is_text(m));
    }
}

static void write_string(Machine *m)
{
    uint8_t page = BH, attr = BL;
    uint16_t saved = get_cursor(m, page);
    set_cursor(m, page, DL, DH);
    uint32_t src = real_addr(ES, BP);
    for (int i = 0; i < CX; i++) {
        uint8_t ch = mem_rd8(m, src++);
        if (AL & 2)
            attr = mem_rd8(m, src++);
        teletype(m, ch, attr, true);
    }
    if (!(AL & 1))
        set_cursor(m, page, (uint8_t)saved, (uint8_t)(saved >> 8));
}

static void read_cell(Machine *m)
{
    uint16_t pos = get_cursor(m, BH);
    AX = is_text(m) ? mem_rd16(m, text_addr(m, BH, pos & 0xFF, pos >> 8)) : 0;
}

static CallbackResult int10_video(Machine *m)
{
    switch (AH) {
    case 0x00: set_mode(m, AL); break;
    case 0x01:
        bda_wr16(m, BDA_CURSOR_SHAPE, CX);
        vga_state(m)->crtc[CRTC_CURSOR_START] = CH, vga_state(m)->crtc[CRTC_CURSOR_END] = CL;
        break;
    case 0x02: set_cursor(m, BH, DL, DH); break;
    case 0x03: DX = get_cursor(m, BH); CX = bda_rd16(m, BDA_CURSOR_SHAPE); break;
    case 0x05: set_page(m, AL); break;
    case 0x06: case 0x07:
        scroll(m, bda_rd8(m, BDA_ACTIVE_PAGE), (Window){ CH, CL, DH, DL }, AL, AH == 0x06, BH);
        break;
    case 0x08: read_cell(m); break;
    case 0x09: write_chars(m, true); break;
    case 0x0A: write_chars(m, false); break;
    case 0x0C: vga_put_pixel(m, CX, DX, AL & 0x7F, (AL & XOR_COLOR) && vga_mode(m) != 0x13); break;
    case 0x0D: AL = vga_get_pixel(m, CX, DX); break;
    case 0x0E: teletype(m, AL, BL, false); break;
    case 0x0F:
        AL = bda_rd8(m, BDA_VIDEO_MODE), AH = (uint8_t)bda_rd16(m, BDA_COLUMNS);
        BH = bda_rd8(m, BDA_ACTIVE_PAGE);
        break;
    case 0x10: palette_service(m); break;
    case 0x11: if (AL == 0x30) font_info(m); break;
    case 0x12:
        if (BL == 0x10)
            BH = 0, BL = VGA_MEMORY_256K, CX = 0;
        else
            AL = 0x12;
        break;
    case 0x13: write_string(m); break;
    case 0x1A: if (AL == 0) AL = 0x1A, BX = VGA_COLOR_DCC; break;
    case 0x4F: AX = VESA_FAILED; break;
    default: break;
    }
    return CB_DONE;
}

void bios_video_init(Machine *m)
{
    callback_register(m, "int10", int10_video, VEC_VIDEO);
    set_mode(m, MODE_TEXT);
}
