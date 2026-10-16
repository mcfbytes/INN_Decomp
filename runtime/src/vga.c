/* VGA registers, DAC and the A0000h window: chained (13h), unchained (mode X) and planar modes.
 * Chained byte n lives in plane n&3 at n>>2, so mode 13h and mode X share one layout. */
#include <stdlib.h>
#include <string.h>

#include "vga_internal.h"

enum {
    VGA_GRAPHICS_BASE = 0xA0000, VGA_GRAPHICS_SIZE = 0x10000,
    PORT_ATTR = 0x3C0, PORT_ATTR_READ = 0x3C1, PORT_MISC_WRITE = 0x3C2, PORT_SEQ_INDEX = 0x3C4,
    PORT_SEQ_DATA = 0x3C5, PORT_DAC_MASK = 0x3C6, PORT_DAC_READ_INDEX = 0x3C7,
    PORT_DAC_WRITE_INDEX = 0x3C8, PORT_DAC_DATA = 0x3C9, PORT_FEATURE_READ = 0x3CA,
    PORT_MISC_READ = 0x3CC, PORT_GC_INDEX = 0x3CE, PORT_GC_DATA = 0x3CF,
    PORT_CRTC_INDEX = 0x3D4, PORT_CRTC_DATA = 0x3D5, PORT_STATUS1 = 0x3DA,
    PORT_MONO_CRTC_INDEX = 0x3B4, PORT_MONO_CRTC_DATA = 0x3B5, PORT_MONO_STATUS1 = 0x3BA,
    STATUS_DISPLAY_OFF = 0x01, STATUS_VRETRACE = 0x08,
    FRAME_US = 14286,       /* 70 Hz */
    VRETRACE_US = 1000,     /* long enough to be seen across a scheduler slice */
    ATTR_INDEX_MASK = 0x1F, DAC_COMPONENT_MASK = 0x3F,
};

/* Standard register sets for the BIOS modes we support. */
static const VgaModeRegs MODES[] = {
    { 0x03, 0x67, { 0x03, 0x00, 0x03, 0x00, 0x02 },
      { 0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00,
        0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF },
      { 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF },
      { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D,
        0x3E, 0x3F, 0x0C, 0x00, 0x0F, 0x08, 0x00 } },
    { 0x0D, 0x63, { 0x03, 0x09, 0x0F, 0x00, 0x06 },
      { 0x2D, 0x27, 0x28, 0x90, 0x2B, 0x80, 0xBF, 0x1F, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x14, 0x00, 0x96, 0xB9, 0xE3, 0xFF },
      { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF },
      { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
        0x16, 0x17, 0x01, 0x00, 0x0F, 0x00, 0x00 } },
    { 0x12, 0xE3, { 0x03, 0x01, 0x0F, 0x00, 0x06 },
      { 0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xEA, 0x8C, 0xDF, 0x28, 0x00, 0xE7, 0x04, 0xE3, 0xFF },
      { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF },
      { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D,
        0x3E, 0x3F, 0x01, 0x00, 0x0F, 0x00, 0x00 } },
    { 0x13, 0x63, { 0x03, 0x01, 0x0F, 0x00, 0x0E },
      { 0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF },
      { 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF },
      { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
        0x0E, 0x0F, 0x41, 0x00, 0x0F, 0x00, 0x00 } },
};

static bool chained(const Vga *v) { return v->seq[SEQ_MEMORY_MODE] & SEQ_CHAIN4; }

/* ---- memory ---- */

static uint8_t logic_op(const Vga *v, uint8_t data, int plane)
{
    uint8_t latch = v->latch[plane];
    switch (v->gc[GC_ROTATE] >> 3 & 3) {
    case 1: return data & latch;
    case 2: return data | latch;
    case 3: return data ^ latch;
    default: return data;
    }
}

static uint8_t rotate_right(uint8_t x, int n) { return (uint8_t)(x >> n | x << (8 - n)); }

/* The byte that write modes 0-3 put into one plane, before the map mask. */
static uint8_t planar_data(const Vga *v, uint8_t value, int plane)
{
    uint8_t rotated = rotate_right(value, v->gc[GC_ROTATE] & 7);
    uint8_t sr = (v->gc[GC_SET_RESET] >> plane & 1) ? 0xFF : 0x00;
    uint8_t mask = v->gc[GC_BIT_MASK];
    uint8_t data;
    switch (v->gc[GC_MODE] & 3) {
    case 1:
        return v->latch[plane];
    case 2:
        data = (value >> plane & 1) ? 0xFF : 0x00;
        break;
    case 3:
        mask &= rotated;
        data = sr;
        break;
    default:
        data = (v->gc[GC_ENABLE_SET_RESET] >> plane & 1) ? sr : rotated;
    }
    data = logic_op(v, data, plane);
    return (uint8_t)((data & mask) | (v->latch[plane] & ~mask));
}

static void planar_write(Vga *v, uint32_t off, uint8_t value)
{
    for (int p = 0; p < 4; p++)
        if (v->seq[SEQ_MAP_MASK] >> p & 1)
            v->planes[p][off] = planar_data(v, value, p);
}

static uint8_t planar_read(Vga *v, uint32_t off)
{
    for (int p = 0; p < 4; p++)
        v->latch[p] = v->planes[p][off];
    if (!(v->gc[GC_MODE] & GC_MODE_READ1))
        return v->latch[v->gc[GC_READ_MAP] & 3];
    uint8_t match = 0xFF;
    for (int p = 0; p < 4; p++) {
        if (!(v->gc[GC_COLOR_DONT_CARE] >> p & 1))
            continue;
        uint8_t want = (v->gc[GC_COLOR_COMPARE] >> p & 1) ? 0xFF : 0x00;
        match &= (uint8_t)~(v->latch[p] ^ want);
    }
    return match;
}

uint8_t vga_mem_read(Machine *m, uint32_t linear)
{
    Vga *v = vga_state(m);
    if (linear >= VGA_TEXT_BASE)
        return v->text[linear - VGA_TEXT_BASE];
    if (linear >= VGA_GRAPHICS_BASE + VGA_GRAPHICS_SIZE)
        return 0xFF;
    uint32_t off = linear - VGA_GRAPHICS_BASE;
    if (chained(v)) {
        for (int p = 0; p < 4; p++)
            v->latch[p] = v->planes[p][off >> 2];
        return v->planes[off & 3][off >> 2];
    }
    return planar_read(v, off);
}

void vga_mem_write(Machine *m, uint32_t linear, uint8_t value)
{
    Vga *v = vga_state(m);
    if (linear >= VGA_TEXT_BASE) {
        v->text[linear - VGA_TEXT_BASE] = value;
        return;
    }
    if (linear >= VGA_GRAPHICS_BASE + VGA_GRAPHICS_SIZE)
        return;
    uint32_t off = linear - VGA_GRAPHICS_BASE;
    if (chained(v)) {
        if (v->seq[SEQ_MAP_MASK] >> (off & 3) & 1)
            v->planes[off & 3][off >> 2] = value;
        return;
    }
    planar_write(v, off, value);
}

/* ---- pixels for the BIOS ---- */

static uint32_t row_bytes(const Vga *v) { return v->crtc[CRTC_OFFSET] * 2u; }

void vga_put_pixel(Machine *m, int x, int y, uint8_t color, bool xor_mode)
{
    Vga *v = vga_state(m);
    if (x < 0 || y < 0 || !(v->gc[GC_MISC] & GC_MISC_GRAPHICS))
        return;
    if (v->attr[ATTR_MODE] & ATTR_MODE_8BIT) {
        uint8_t *px = &v->planes[x & 3][(y * row_bytes(v) + (uint32_t)(x >> 2)) & 0xFFFF];
        *px = xor_mode ? (uint8_t)(*px ^ color) : color;
        return;
    }
    uint32_t off = (y * row_bytes(v) + (uint32_t)(x >> 3)) & 0xFFFF;
    uint8_t bit = (uint8_t)(0x80 >> (x & 7));
    for (int p = 0; p < 4; p++) {
        bool on = color >> p & 1;
        if (xor_mode)
            v->planes[p][off] ^= on ? bit : 0;
        else
            v->planes[p][off] = on ? (v->planes[p][off] | bit) : (v->planes[p][off] & ~bit);
    }
}

uint8_t vga_get_pixel(Machine *m, int x, int y)
{
    Vga *v = vga_state(m);
    if (x < 0 || y < 0)
        return 0;
    if (v->attr[ATTR_MODE] & ATTR_MODE_8BIT)
        return v->planes[x & 3][(y * row_bytes(v) + (uint32_t)(x >> 2)) & 0xFFFF];
    uint32_t off = (y * row_bytes(v) + (uint32_t)(x >> 3)) & 0xFFFF;
    uint8_t color = 0;
    for (int p = 0; p < 4; p++)
        color |= (uint8_t)((v->planes[p][off] >> (7 - (x & 7)) & 1) << p);
    return color;
}

/* ---- registers ---- */

static uint8_t status1_read(Machine *m)
{
    Vga *v = vga_state(m);
    v->attr_data_next = false;
    bool retrace = m->virtual_us % FRAME_US >= FRAME_US - VRETRACE_US;
    uint8_t s = (++v->status_reads & 1) ? STATUS_DISPLAY_OFF : 0;
    if (m->irq_hold) /* emulated time stands still in a shadow run; retrace waits must still end */
        retrace = (v->status_reads >> 3) & 1;
    return retrace ? (uint8_t)(s | STATUS_DISPLAY_OFF | STATUS_VRETRACE) : s;
}

static uint8_t dac_data_read(Vga *v)
{
    uint8_t value = v->dac[v->dac_read][v->dac_read_sub];
    if (++v->dac_read_sub == 3) {
        v->dac_read_sub = 0;
        v->dac_read++;
    }
    return value;
}

static void dac_data_write(Vga *v, uint8_t value)
{
    v->dac[v->dac_write][v->dac_write_sub] = value & DAC_COMPONENT_MASK;
    if (++v->dac_write_sub == 3) {
        v->dac_write_sub = 0;
        v->dac_write++;
    }
}

static uint8_t reg_read(uint8_t *regs, int count, uint8_t index)
{
    return index < count ? regs[index] : 0xFF;
}

static uint32_t vga_in(Machine *m, uint16_t port, int size)
{
    Vga *v = vga_state(m);
    switch (port) {
    case PORT_ATTR: return v->attr_index;
    case PORT_ATTR_READ: return reg_read(v->attr, VGA_ATTR_REGS, v->attr_index & ATTR_INDEX_MASK);
    case PORT_SEQ_INDEX: return v->seq_index;
    case PORT_SEQ_DATA: return reg_read(v->seq, VGA_SEQ_REGS, v->seq_index);
    case PORT_DAC_MASK: return v->dac_mask;
    case PORT_DAC_READ_INDEX: return v->dac_last_read ? 3 : 0;
    case PORT_DAC_WRITE_INDEX: return v->dac_write;
    case PORT_DAC_DATA: return dac_data_read(v);
    case PORT_FEATURE_READ: return 0;
    case PORT_MISC_READ: return v->misc;
    case PORT_GC_INDEX: return v->gc_index;
    case PORT_GC_DATA: return reg_read(v->gc, VGA_GC_REGS, v->gc_index);
    case PORT_CRTC_INDEX: case PORT_MONO_CRTC_INDEX: return v->crtc_index;
    case PORT_CRTC_DATA: case PORT_MONO_CRTC_DATA:
        return reg_read(v->crtc, VGA_CRTC_REGS, v->crtc_index);
    case PORT_STATUS1: case PORT_MONO_STATUS1: return status1_read(m);
    default: return 0xFF;
    }
}

static void reg_write(uint8_t *regs, int count, uint8_t index, uint8_t value)
{
    if (index < count)
        regs[index] = value;
}

static void attr_write(Vga *v, uint8_t value)
{
    if (!v->attr_data_next)
        v->attr_index = value;
    else
        reg_write(v->attr, VGA_ATTR_REGS, v->attr_index & ATTR_INDEX_MASK, value);
    v->attr_data_next = !v->attr_data_next;
}

static void vga_out(Machine *m, uint16_t port, uint32_t value, int size)
{
    Vga *v = vga_state(m);
    uint8_t b = (uint8_t)value;
    if (size > 1) {
        vga_out(m, port, b, 1); /* out dx,ax on an index port: index then data */
        vga_out(m, (uint16_t)(port + 1), value >> 8 & 0xFF, 1);
        return;
    }
    switch (port) {
    case PORT_ATTR: attr_write(v, b); break;
    case PORT_MISC_WRITE: v->misc = b; break;
    case PORT_SEQ_INDEX: v->seq_index = b; break;
    case PORT_SEQ_DATA: reg_write(v->seq, VGA_SEQ_REGS, v->seq_index, b); break;
    case PORT_DAC_MASK: v->dac_mask = b; break;
    case PORT_DAC_READ_INDEX: v->dac_read = b; v->dac_read_sub = 0; v->dac_last_read = true; break;
    case PORT_DAC_WRITE_INDEX: v->dac_write = b; v->dac_write_sub = 0; v->dac_last_read = false; break;
    case PORT_DAC_DATA: dac_data_write(v, b); break;
    case PORT_GC_INDEX: v->gc_index = b; break;
    case PORT_GC_DATA: reg_write(v->gc, VGA_GC_REGS, v->gc_index, b); break;
    case PORT_CRTC_INDEX: case PORT_MONO_CRTC_INDEX: v->crtc_index = b; break;
    case PORT_CRTC_DATA: case PORT_MONO_CRTC_DATA:
        reg_write(v->crtc, VGA_CRTC_REGS, v->crtc_index, b);
        break;
    default: break;
    }
}

/* ---- modes ---- */

static const VgaModeRegs *find_mode(uint8_t mode)
{
    for (size_t i = 0; i < sizeof MODES / sizeof MODES[0]; i++)
        if (MODES[i].mode == mode)
            return &MODES[i];
    return NULL;
}

static void clear_text(Vga *v)
{
    for (int i = 0; i < VGA_TEXT_SIZE; i += 2) {
        v->text[i] = ' ';
        v->text[i + 1] = 0x07;
    }
}

bool vga_set_mode(Machine *m, uint8_t mode, bool clear)
{
    Vga *v = vga_state(m);
    const VgaModeRegs *r = find_mode(mode);
    if (!r)
        return false;
    v->mode = mode;
    v->misc = r->misc;
    memcpy(v->seq, r->seq, sizeof v->seq);
    memcpy(v->crtc, r->crtc, sizeof v->crtc);
    memcpy(v->gc, r->gc, sizeof v->gc);
    memcpy(v->attr, r->attr, sizeof v->attr);
    v->attr_data_next = false;
    v->dac_mask = 0xFF;
    vga_default_palette(v, r->attr[ATTR_MODE] & ATTR_MODE_8BIT);
    if (clear) {
        memset(v->planes, 0, sizeof v->planes);
        clear_text(v);
    }
    return true;
}

uint8_t vga_mode(Machine *m) { return vga_state(m)->mode; }

void vga_dac_set(Machine *m, uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    Vga *v = vga_state(m);
    v->dac[index][0] = r & DAC_COMPONENT_MASK;
    v->dac[index][1] = g & DAC_COMPONENT_MASK;
    v->dac[index][2] = b & DAC_COMPONENT_MASK;
}

void vga_dac_get(Machine *m, uint8_t index, uint8_t rgb[3])
{
    Vga *v = vga_state(m);
    rgb[0] = v->dac[index][0];
    rgb[1] = v->dac[index][1];
    rgb[2] = v->dac[index][2];
}

enum { VGA_STATE_PIXELS = 64000, VGA_STATE_TEXT_AT = 0x18000, MODE_TEXT80 = 0x03, MODE_320X200X8 = 0x13 };

bool vga_load_state(Machine *m, uint8_t mode, const uint8_t dac[256][3], const uint8_t *mem)
{
    Vga *v = vga_state(m);
    if (mode != MODE_TEXT80 && mode != MODE_320X200X8)
        return false;
    if (v->mode != mode)
        vga_set_mode(m, mode, false);
    memmove(v->dac, dac, sizeof v->dac);
    if (mode == MODE_320X200X8) {
        for (uint32_t n = 0; n < VGA_STATE_PIXELS; n++)
            v->planes[n & 3][n >> 2] = mem[n];
    } else {
        memcpy(v->text, mem + VGA_STATE_TEXT_AT, VGA_TEXT_SIZE);
    }
    return true;
}

bool vga_read_state(Machine *m, uint8_t *mode, uint8_t dac[256][3], uint8_t *mem)
{
    Vga *v = vga_state(m);
    if (v->mode != MODE_TEXT80 && v->mode != MODE_320X200X8)
        return false;
    *mode = v->mode;
    memmove(dac, v->dac, sizeof v->dac);
    if (v->mode == MODE_320X200X8) {
        for (uint32_t n = 0; n < VGA_STATE_PIXELS; n++)
            mem[n] = v->planes[n & 3][n >> 2];
    } else {
        memcpy(mem + VGA_STATE_TEXT_AT, v->text, VGA_TEXT_SIZE);
    }
    return true;
}

uint16_t vga_crtc_start(const Vga *v)
{
    return (uint16_t)(v->crtc[CRTC_START_HI] << 8 | v->crtc[CRTC_START_LO]);
}

void vga_init(Machine *m)
{
    Vga *v = calloc(1, sizeof *v);
    if (!v)
        fatal("out of memory");
    m->vga = v;
    port_register(m, PORT_ATTR, PORT_GC_DATA, vga_in, vga_out);
    port_register(m, PORT_CRTC_INDEX, PORT_CRTC_DATA, vga_in, vga_out);
    port_register(m, PORT_STATUS1, PORT_STATUS1, vga_in, vga_out);
    port_register(m, PORT_MONO_CRTC_INDEX, PORT_MONO_CRTC_DATA, vga_in, vga_out);
    port_register(m, PORT_MONO_STATUS1, PORT_MONO_STATUS1, vga_in, vga_out);
    vga_set_mode(m, 0x03, true);
}

size_t vga_snapshot_size(void) { return sizeof(Vga); }

/* A machine built without devices (unit tests) has no VGA; its snapshot is all zero. */
void vga_snapshot_save(Machine *m, void *dst)
{
    if (m->vga)
        memcpy(dst, vga_state(m), sizeof(Vga));
    else
        memset(dst, 0, sizeof(Vga));
}

void vga_snapshot_load(Machine *m, const void *src)
{
    if (m->vga)
        memcpy(vga_state(m), src, sizeof(Vga));
}
