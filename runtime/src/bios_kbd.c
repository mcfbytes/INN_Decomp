/* BIOS keyboard: INT 09h turns set-1 scancodes from port 60h into BIOS key words in the BDA
 * ring buffer and tracks shift states; INT 16h reads them back. */
#include "bios_internal.h"

enum {
    PORT_KBD_DATA = 0x60,
    VEC_KBD_IRQ = 0x09, VEC_KBD_SERVICE = 0x16,
    SC_RELEASE = 0x80, SC_EXTENDED = 0xE0, SC_PAUSE_PREFIX = 0xE1,
    SC_CTRL = 0x1D, SC_LSHIFT = 0x2A, SC_RSHIFT = 0x36, SC_ALT = 0x38, SC_CAPS = 0x3A,
    SC_NUM = 0x45, SC_SCROLL = 0x46, SC_INSERT = 0x52, SC_KP_ENTER = 0x1C, SC_KP_SLASH = 0x35,
    SC_F1 = 0x3B, SC_F10 = 0x44, SC_KP_FIRST = 0x47, SC_KP_LAST = 0x53, SC_KP_MINUS = 0x4A,
    SC_KP_PLUS = 0x4E, SC_TABLE_SIZE = 0x59,
    F0_RSHIFT = 0x01, F0_LSHIFT = 0x02, F0_CTRL = 0x04, F0_ALT = 0x08, F0_SCROLL = 0x10,
    F0_NUM = 0x20, F0_CAPS = 0x40, F0_INSERT = 0x80,
    F1_LCTRL = 0x01, F1_LALT = 0x02, F1_SCROLL_DOWN = 0x10, F1_NUM_DOWN = 0x20,
    F1_CAPS_DOWN = 0x40, F1_INSERT_DOWN = 0x80,
    KM_LAST_E0 = 0x02, KM_RCTRL = 0x04, KM_RALT = 0x08, KM_ENHANCED = 0x10,
    EXT_MARK = 0xE0, ALT_EXT_OFFSET = 0x50,
};

typedef struct KeyCodes { uint16_t normal, shift, ctrl, alt; } KeyCodes;

/* Indexed by set-1 make code; zero rows are modifiers or keys with no BIOS code. */
static const KeyCodes KEYS[SC_TABLE_SIZE] = {
    [0x01] = { 0x011B, 0x011B, 0x011B, 0x0100 }, [0x02] = { 0x0231, 0x0221, 0, 0x7800 },
    [0x03] = { 0x0332, 0x0340, 0x0300, 0x7900 }, [0x04] = { 0x0433, 0x0423, 0, 0x7A00 },
    [0x05] = { 0x0534, 0x0524, 0, 0x7B00 }, [0x06] = { 0x0635, 0x0625, 0, 0x7C00 },
    [0x07] = { 0x0736, 0x075E, 0x071E, 0x7D00 }, [0x08] = { 0x0837, 0x0826, 0, 0x7E00 },
    [0x09] = { 0x0938, 0x092A, 0, 0x7F00 }, [0x0A] = { 0x0A39, 0x0A28, 0, 0x8000 },
    [0x0B] = { 0x0B30, 0x0B29, 0, 0x8100 }, [0x0C] = { 0x0C2D, 0x0C5F, 0x0C1F, 0x8200 },
    [0x0D] = { 0x0D3D, 0x0D2B, 0, 0x8300 }, [0x0E] = { 0x0E08, 0x0E08, 0x0E7F, 0x0E00 },
    [0x0F] = { 0x0F09, 0x0F00, 0x9400, 0xA500 }, [0x10] = { 0x1071, 0x1051, 0x1011, 0x1000 },
    [0x11] = { 0x1177, 0x1157, 0x1117, 0x1100 }, [0x12] = { 0x1265, 0x1245, 0x1205, 0x1200 },
    [0x13] = { 0x1372, 0x1352, 0x1312, 0x1300 }, [0x14] = { 0x1474, 0x1454, 0x1414, 0x1400 },
    [0x15] = { 0x1579, 0x1559, 0x1519, 0x1500 }, [0x16] = { 0x1675, 0x1655, 0x1615, 0x1600 },
    [0x17] = { 0x1769, 0x1749, 0x1709, 0x1700 }, [0x18] = { 0x186F, 0x184F, 0x180F, 0x1800 },
    [0x19] = { 0x1970, 0x1950, 0x1910, 0x1900 }, [0x1A] = { 0x1A5B, 0x1A7B, 0x1A1B, 0x1A00 },
    [0x1B] = { 0x1B5D, 0x1B7D, 0x1B1D, 0x1B00 }, [0x1C] = { 0x1C0D, 0x1C0D, 0x1C0A, 0x1C00 },
    [0x1E] = { 0x1E61, 0x1E41, 0x1E01, 0x1E00 }, [0x1F] = { 0x1F73, 0x1F53, 0x1F13, 0x1F00 },
    [0x20] = { 0x2064, 0x2044, 0x2004, 0x2000 }, [0x21] = { 0x2166, 0x2146, 0x2106, 0x2100 },
    [0x22] = { 0x2267, 0x2247, 0x2207, 0x2200 }, [0x23] = { 0x2368, 0x2348, 0x2308, 0x2300 },
    [0x24] = { 0x246A, 0x244A, 0x240A, 0x2400 }, [0x25] = { 0x256B, 0x254B, 0x250B, 0x2500 },
    [0x26] = { 0x266C, 0x264C, 0x260C, 0x2600 }, [0x27] = { 0x273B, 0x273A, 0, 0x2700 },
    [0x28] = { 0x2827, 0x2822, 0, 0x2800 }, [0x29] = { 0x2960, 0x297E, 0, 0x2900 },
    [0x2B] = { 0x2B5C, 0x2B7C, 0x2B1C, 0x2B00 }, [0x2C] = { 0x2C7A, 0x2C5A, 0x2C1A, 0x2C00 },
    [0x2D] = { 0x2D78, 0x2D58, 0x2D18, 0x2D00 }, [0x2E] = { 0x2E63, 0x2E43, 0x2E03, 0x2E00 },
    [0x2F] = { 0x2F76, 0x2F56, 0x2F16, 0x2F00 }, [0x30] = { 0x3062, 0x3042, 0x3002, 0x3000 },
    [0x31] = { 0x316E, 0x314E, 0x310E, 0x3100 }, [0x32] = { 0x326D, 0x324D, 0x320D, 0x3200 },
    [0x33] = { 0x332C, 0x333C, 0, 0x3300 }, [0x34] = { 0x342E, 0x343E, 0, 0x3400 },
    [0x35] = { 0x352F, 0x353F, 0, 0x3500 }, [0x37] = { 0x372A, 0x372A, 0x9600, 0x3700 },
    [0x39] = { 0x3920, 0x3920, 0x3920, 0x3920 },
    [0x47] = { 0x4700, 0x4737, 0x7700, 0 }, [0x48] = { 0x4800, 0x4838, 0x8D00, 0 },
    [0x49] = { 0x4900, 0x4939, 0x8400, 0 }, [0x4A] = { 0x4A2D, 0x4A2D, 0x8E00, 0x4A00 },
    [0x4B] = { 0x4B00, 0x4B34, 0x7300, 0 }, [0x4C] = { 0x4C00, 0x4C35, 0x8F00, 0 },
    [0x4D] = { 0x4D00, 0x4D36, 0x7400, 0 }, [0x4E] = { 0x4E2B, 0x4E2B, 0x9000, 0x4E00 },
    [0x4F] = { 0x4F00, 0x4F31, 0x7500, 0 }, [0x50] = { 0x5000, 0x5032, 0x9100, 0 },
    [0x51] = { 0x5100, 0x5133, 0x7600, 0 }, [0x52] = { 0x5200, 0x5230, 0x9200, 0 },
    [0x53] = { 0x5300, 0x532E, 0x9300, 0 },
    [0x57] = { 0x8500, 0x8700, 0x8900, 0x8B00 }, [0x58] = { 0x8600, 0x8800, 0x8A00, 0x8C00 },
};

/* F1-F10 occupy four regular runs of codes. */
enum { FKEY_NORMAL = 0x3B, FKEY_SHIFT = 0x54, FKEY_CTRL = 0x5E, FKEY_ALT = 0x68 };

/* ---- the BDA ring buffer ---- */

static uint16_t ring_next(Machine *m, uint16_t ptr)
{
    ptr = (uint16_t)(ptr + 2);
    return ptr >= bda_rd16(m, BDA_KBD_END_PTR) ? bda_rd16(m, BDA_KBD_START_PTR) : ptr;
}

bool bios_kbd_peek(Machine *m, uint16_t *key)
{
    uint16_t head = bda_rd16(m, BDA_KBD_HEAD);
    if (head == bda_rd16(m, BDA_KBD_TAIL))
        return false;
    *key = mem_rd16(m, real_addr(BDA_SEG, head));
    return true;
}

bool bios_kbd_pop(Machine *m, uint16_t *key)
{
    if (!bios_kbd_peek(m, key))
        return false;
    bda_wr16(m, BDA_KBD_HEAD, ring_next(m, bda_rd16(m, BDA_KBD_HEAD)));
    return true;
}

bool bios_kbd_push(Machine *m, uint16_t key)
{
    uint16_t tail = bda_rd16(m, BDA_KBD_TAIL), next = ring_next(m, tail);
    if (next == bda_rd16(m, BDA_KBD_HEAD))
        return false;
    mem_wr16(m, real_addr(BDA_SEG, tail), key);
    bda_wr16(m, BDA_KBD_TAIL, next);
    return true;
}

/* ---- INT 09h ---- */

static uint16_t fkey_code(uint8_t sc, uint8_t f0)
{
    int base = (f0 & F0_ALT) ? FKEY_ALT : (f0 & F0_CTRL) ? FKEY_CTRL
             : (f0 & (F0_LSHIFT | F0_RSHIFT)) ? FKEY_SHIFT : FKEY_NORMAL;
    return (uint16_t)((base + sc - SC_F1) << 8);
}

static uint16_t regular_code(uint8_t sc, uint8_t f0)
{
    if (sc >= SC_F1 && sc <= SC_F10)
        return fkey_code(sc, f0);
    const KeyCodes *k = &KEYS[sc];
    if (f0 & F0_ALT)
        return k->alt;
    if (f0 & F0_CTRL)
        return k->ctrl;
    bool shift = f0 & (F0_LSHIFT | F0_RSHIFT);
    bool letter = (k->normal & 0xFF) >= 'a' && (k->normal & 0xFF) <= 'z';
    bool keypad = sc >= SC_KP_FIRST && sc <= SC_KP_LAST && sc != SC_KP_MINUS && sc != SC_KP_PLUS;
    if ((letter && (f0 & F0_CAPS)) || (keypad && (f0 & F0_NUM)))
        shift = !shift;
    return shift ? k->shift : k->normal;
}

/* Grey editing keys and the keypad Enter and slash carry E0 so INT 16h AH=10h can tell them apart. */
static uint16_t extended_code(uint8_t sc, uint8_t f0)
{
    if (sc == SC_KP_ENTER)
        return (f0 & F0_ALT) ? 0xA600 : (f0 & F0_CTRL) ? 0xE00A : 0xE00D;
    if (sc == SC_KP_SLASH)
        return (f0 & F0_ALT) ? 0xA400 : (f0 & F0_CTRL) ? 0x9500 : 0xE02F;
    if (sc < SC_KP_FIRST || sc > SC_KP_LAST)
        return 0;
    if (f0 & F0_ALT)
        return (uint16_t)((sc + ALT_EXT_OFFSET) << 8);
    if (f0 & F0_CTRL)
        return (uint16_t)(KEYS[sc].ctrl | EXT_MARK);
    return (uint16_t)(sc << 8 | EXT_MARK);
}

static void toggle_key(Machine *m, uint8_t f0_toggle, uint8_t f1_down, bool down)
{
    uint8_t f1 = bda_rd8(m, BDA_KBD_FLAGS1);
    if (down && !(f1 & f1_down))
        bda_wr8(m, BDA_KBD_FLAGS0, bda_rd8(m, BDA_KBD_FLAGS0) ^ f0_toggle);
    bda_wr8(m, BDA_KBD_FLAGS1, down ? (uint8_t)(f1 | f1_down) : (uint8_t)(f1 & ~f1_down));
}

static void set_bits(Machine *m, int off, uint8_t bits, bool on)
{
    uint8_t v = bda_rd8(m, off);
    bda_wr8(m, off, on ? (uint8_t)(v | bits) : (uint8_t)(v & ~bits));
}

/* Returns true when sc was a modifier (and has been accounted for). */
static bool modifier(Machine *m, uint8_t sc, bool down, bool ext)
{
    switch (sc) {
    case SC_LSHIFT: if (!ext) set_bits(m, BDA_KBD_FLAGS0, F0_LSHIFT, down); return true;
    case SC_RSHIFT: if (!ext) set_bits(m, BDA_KBD_FLAGS0, F0_RSHIFT, down); return true;
    case SC_CTRL:
        set_bits(m, ext ? BDA_KBD_MODE : BDA_KBD_FLAGS1, ext ? KM_RCTRL : F1_LCTRL, down);
        set_bits(m, BDA_KBD_FLAGS0, F0_CTRL, (bda_rd8(m, BDA_KBD_MODE) & KM_RCTRL) ||
                                                 (bda_rd8(m, BDA_KBD_FLAGS1) & F1_LCTRL));
        return true;
    case SC_ALT:
        set_bits(m, ext ? BDA_KBD_MODE : BDA_KBD_FLAGS1, ext ? KM_RALT : F1_LALT, down);
        set_bits(m, BDA_KBD_FLAGS0, F0_ALT, (bda_rd8(m, BDA_KBD_MODE) & KM_RALT) ||
                                                (bda_rd8(m, BDA_KBD_FLAGS1) & F1_LALT));
        return true;
    case SC_CAPS: toggle_key(m, F0_CAPS, F1_CAPS_DOWN, down); return true;
    case SC_NUM: toggle_key(m, F0_NUM, F1_NUM_DOWN, down); return true;
    case SC_SCROLL: toggle_key(m, F0_SCROLL, F1_SCROLL_DOWN, down); return true;
    default: return false;
    }
}

static CallbackResult int09_keyboard(Machine *m)
{
    uint8_t raw = (uint8_t)port_in(m, PORT_KBD_DATA, 1);
    uint8_t mode = bda_rd8(m, BDA_KBD_MODE);
    if (raw == SC_EXTENDED || raw == SC_PAUSE_PREFIX) {
        bda_wr8(m, BDA_KBD_MODE, (uint8_t)(mode | KM_LAST_E0));
        return CB_DONE;
    }
    bool ext = mode & KM_LAST_E0;
    bda_wr8(m, BDA_KBD_MODE, (uint8_t)(mode & ~KM_LAST_E0));
    uint8_t sc = raw & (uint8_t)~SC_RELEASE;
    bool down = !(raw & SC_RELEASE);
    if (modifier(m, sc, down, ext) || !down || sc >= SC_TABLE_SIZE)
        return CB_DONE;
    uint8_t f0 = bda_rd8(m, BDA_KBD_FLAGS0);
    if (sc == SC_INSERT && !(f0 & (F0_CTRL | F0_ALT)))
        bda_wr8(m, BDA_KBD_FLAGS0, f0 ^ F0_INSERT);
    uint16_t key = ext ? extended_code(sc, f0) : regular_code(sc, f0);
    if (key)
        bios_kbd_push(m, key);
    return CB_DONE;
}

/* ---- INT 16h ---- */

/* The pre-101-key functions (AH=00h/01h) see grey keys as their keypad twins. */
static uint16_t legacy_key(uint16_t key)
{
    if ((key >> 8) == EXT_MARK)
        return (uint16_t)((key & 0xFF) == 0x2F ? 0x352F : (0x1C00 | (key & 0xFF)));
    if ((key & 0xFF) == EXT_MARK)
        return (uint16_t)(key & 0xFF00);
    return key;
}

static uint8_t extended_shift_flags(Machine *m)
{
    uint8_t f1 = bda_rd8(m, BDA_KBD_FLAGS1), km = bda_rd8(m, BDA_KBD_MODE);
    uint8_t v = (uint8_t)(f1 & (F1_LCTRL | F1_LALT));
    v |= (km & KM_RCTRL) ? 0x04 : 0;
    v |= (km & KM_RALT) ? 0x08 : 0;
    return (uint8_t)(v | (f1 & (F1_SCROLL_DOWN | F1_NUM_DOWN | F1_CAPS_DOWN | F1_INSERT_DOWN)));
}

static CallbackResult int16_service(Machine *m)
{
    uint16_t key;
    switch (AH) {
    case 0x00: case 0x10:
        if (!bios_kbd_pop(m, &key))
            return bios_block(m);
        AX = AH == 0x00 ? legacy_key(key) : key;
        return CB_DONE;
    case 0x01: case 0x11: {
        bool have = bios_kbd_peek(m, &key);
        if (have)
            AX = AH == 0x01 ? legacy_key(key) : key;
        cb_set_zero(m, !have);
        return CB_DONE;
    }
    case 0x02: AL = bda_rd8(m, BDA_KBD_FLAGS0); return CB_DONE;
    case 0x12:
        AL = bda_rd8(m, BDA_KBD_FLAGS0);
        AH = extended_shift_flags(m);
        return CB_DONE;
    case 0x05: AL = bios_kbd_push(m, CX) ? 0 : 1; return CB_DONE;
    default: return CB_DONE; /* typematic rate and the rest: accepted, no effect */
    }
}

void bios_kbd_init(Machine *m)
{
    bda_wr16(m, BDA_KBD_START_PTR, BDA_KBD_BUFFER);
    bda_wr16(m, BDA_KBD_END_PTR, BDA_KBD_BUFFER_END);
    bda_wr16(m, BDA_KBD_HEAD, BDA_KBD_BUFFER);
    bda_wr16(m, BDA_KBD_TAIL, BDA_KBD_BUFFER);
    bda_wr8(m, BDA_KBD_MODE, KM_ENHANCED);
    bios_irq_handler(m, VEC_KBD_IRQ, "int09", int09_keyboard, false);
    callback_register(m, "int16", int16_service, VEC_KBD_SERVICE);
}
