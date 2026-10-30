/* EMS (LIM 4.0) and XMS (3.0), opt-in (--ems-kb, --xms-kb). Everything they own lives in guest memory
 * above 1 MB + 64 KB, which real-mode code cannot address: the state block, then the XMS pool, then
 * the EMS pages. Shadow verification snapshots [0, Machine.snapshot_bytes), so it carries them.
 *
 * The EMS page frame (E000h) is plain memory: mapping a page copies the frame slot's old page back to
 * its pool page and the new page in. Moves (EMS 57h, XMS 0Bh) write the mapped slots back first, read
 * frame addresses through the mapping and reload the slots afterwards. A logical page mapped into two
 * slots at once shows the same bytes at each map call, not live aliases. */
#include <stdlib.h>
#include <string.h>

#include "emsxms.h"

enum {
    EMS_MAX_PAGES = 1024, EMS_MAX_HANDLES = 255, XMS_MAX_HANDLES = 32,
    EMS_VERSION = 0x40, XMS_VERSION = 0x0300, XMS_REVISION = 0x0300,
    UNMAPPED = 0xFFFF, NO_PAGE = 0xFFFF,
    EXT_LIMIT = 0x1000000, /* 16 MB */
    FRAME_LINEAR = MEM_EMS_FRAME, FRAME_BYTES = MEM_EMS_FRAME_BYTES,
    MOVE_MAX_BYTES = 0x100000, CONV_LIMIT = MEM_ONE_MEG + 0xFFF0,
    MUX_XMS_PRESENT = 0x4300, MUX_XMS_ENTRY = 0x4310, XMS_INSTALLED = 0x80,
};

/* LIM EMS status codes (AH). */
enum {
    EMS_OK = 0x00, EMS_BAD_HANDLE = 0x83, EMS_BAD_FUNCTION = 0x84, EMS_NO_HANDLES = 0x85,
    EMS_SAVED_CONTEXT = 0x86, EMS_TOO_MANY_PAGES = 0x87, EMS_NOT_ENOUGH_FREE = 0x88,
    EMS_ZERO_PAGES = 0x89, EMS_BAD_LOGICAL = 0x8A, EMS_BAD_PHYSICAL = 0x8B, EMS_ALREADY_SAVED = 0x8D,
    EMS_NOT_SAVED = 0x8E, EMS_BAD_SUBFUNCTION = 0x8F, EMS_REGION_OVERFLOW = 0x93,
    EMS_BAD_OFFSET = 0x95, EMS_MOVE_TOO_LONG = 0x96, EMS_BAD_MEMORY_TYPE = 0x98, EMS_CONV_OVERFLOW = 0xA2,
};

/* XMS error codes (BL). */
enum {
    XMS_NOT_IMPLEMENTED = 0x80, XMS_NO_HMA = 0x90, XMS_NO_MEMORY = 0xA0, XMS_NO_HANDLES = 0xA1,
    XMS_BAD_HANDLE = 0xA2, XMS_BAD_SRC_HANDLE = 0xA3, XMS_BAD_SRC_OFFSET = 0xA4,
    XMS_BAD_DST_HANDLE = 0xA5, XMS_BAD_DST_OFFSET = 0xA6, XMS_BAD_LENGTH = 0xA7,
    XMS_NOT_LOCKED = 0xAA, XMS_LOCKED = 0xAB,
};

typedef struct EmsHandle {
    uint8_t used, saved;
    uint16_t pages;
    uint16_t saved_map[EMS_PHYS_PAGES][2];
} EmsHandle;

typedef struct XmsHandle {
    uint8_t used, locks;
    uint16_t size_kb;
    uint32_t start_kb; /* in the XMS pool */
} XmsHandle;

typedef struct ExtState {
    uint32_t ems_pool, xms_pool; /* linear */
    uint16_t ems_pages, xms_kb;
    uint16_t xms_entry_off;      /* in CALLBACK_SEG */
    uint16_t map[EMS_PHYS_PAGES][2]; /* handle and logical page per frame slot, or UNMAPPED */
    uint16_t page_owner[EMS_MAX_PAGES]; /* 0 free, else handle + 1 */
    uint16_t page_logical[EMS_MAX_PAGES];
    EmsHandle ems[EMS_MAX_HANDLES];
    XmsHandle xms[XMS_MAX_HANDLES];
} ExtState;

/* ---- EMS pages and the frame ---- */

static uint32_t slot_linear(int slot) { return FRAME_LINEAR + (uint32_t)slot * EMS_PAGE_BYTES; }

static uint16_t find_page(const ExtState *s, uint16_t handle, uint16_t logical)
{
    for (uint16_t p = 0; p < s->ems_pages; p++)
        if (s->page_owner[p] == handle + 1 && s->page_logical[p] == logical)
            return p;
    return NO_PAGE;
}

static uint32_t page_linear(const ExtState *s, uint16_t page) { return s->ems_pool + (uint32_t)page * EMS_PAGE_BYTES; }

static uint16_t free_pages(const ExtState *s)
{
    uint16_t n = 0;
    for (uint16_t p = 0; p < s->ems_pages; p++)
        n += s->page_owner[p] == 0;
    return n;
}

static uint16_t slot_page(const ExtState *s, int slot)
{
    return s->map[slot][0] == UNMAPPED ? NO_PAGE : find_page(s, s->map[slot][0], s->map[slot][1]);
}

/* Frame slots back to their pages, before anything reads the pages directly. */
static void frame_write_back(Machine *m)
{
    ExtState *s = m->ems;
    if (!s)
        return;
    for (int slot = 0; slot < EMS_PHYS_PAGES; slot++) {
        uint16_t p = slot_page(s, slot);
        if (p != NO_PAGE)
            memcpy(m->mem + page_linear(s, p), m->mem + slot_linear(slot), EMS_PAGE_BYTES);
    }
}

static void frame_reload(Machine *m)
{
    ExtState *s = m->ems;
    if (!s)
        return;
    for (int slot = 0; slot < EMS_PHYS_PAGES; slot++) {
        uint16_t p = slot_page(s, slot);
        if (p != NO_PAGE)
            memcpy(m->mem + slot_linear(slot), m->mem + page_linear(s, p), EMS_PAGE_BYTES);
    }
}

/* A conventional address, with frame addresses of mapped slots turned into their pool pages. */
static uint32_t conv_through_frame(Machine *m, uint32_t linear)
{
    ExtState *s = m->ems;
    if (!s || linear - FRAME_LINEAR >= FRAME_BYTES)
        return linear;
    int slot = (int)((linear - FRAME_LINEAR) / EMS_PAGE_BYTES);
    uint16_t p = slot_page(s, slot);
    return p == NO_PAGE ? linear : page_linear(s, p) + (linear & (EMS_PAGE_BYTES - 1));
}

static uint8_t space_rd(Machine *m, uint32_t linear)
{
    return linear >= EXT_MEM_BASE ? m->mem[linear] : mem_rd8(m, linear);
}

static void space_wr(Machine *m, uint32_t linear, uint8_t v)
{
    if (linear >= EXT_MEM_BASE)
        m->mem[linear] = v;
    else
        mem_wr8(m, linear, v);
}

/* ---- block moves shared by EMS 57h and XMS 0Bh ---- */

typedef enum Space { SPACE_CONV, SPACE_EMS, SPACE_XMS } Space;

typedef struct Region {
    Space space;
    uint16_t handle;
    uint32_t base;          /* conventional or XMS: linear start; EMS: byte offset from logical page 0 */
    uint16_t cached_logical; /* EMS: the last page looked up */
    uint16_t cached_page;
} Region;

static uint32_t region_linear(Machine *m, Region *r, uint32_t i)
{
    switch (r->space) {
    case SPACE_CONV: return conv_through_frame(m, r->base + i);
    case SPACE_XMS: return r->base + i;
    case SPACE_EMS: {
        uint32_t at = r->base + i;
        uint16_t logical = (uint16_t)(at / EMS_PAGE_BYTES);
        if (logical != r->cached_logical || r->cached_page == NO_PAGE) {
            r->cached_logical = logical;
            r->cached_page = find_page(m->ems, r->handle, logical);
        }
        return page_linear(m->ems, r->cached_page) + (at & (EMS_PAGE_BYTES - 1));
    }
    }
    return 0;
}

/* Copies (or exchanges) len bytes as if through a buffer, so overlapping regions behave like memmove. */
static void region_move(Machine *m, Region *src, Region *dst, uint32_t len, bool exchange)
{
    if (len == 0)
        return;
    frame_write_back(m);
    uint8_t *a = malloc(len), *b = exchange ? malloc(len) : NULL;
    if (!a || (exchange && !b))
        fatal("out of memory");
    for (uint32_t i = 0; i < len; i++)
        a[i] = space_rd(m, region_linear(m, src, i));
    if (exchange)
        for (uint32_t i = 0; i < len; i++)
            b[i] = space_rd(m, region_linear(m, dst, i));
    for (uint32_t i = 0; i < len; i++)
        space_wr(m, region_linear(m, dst, i), a[i]);
    if (exchange)
        for (uint32_t i = 0; i < len; i++)
            space_wr(m, region_linear(m, src, i), b[i]);
    free(a);
    free(b);
    frame_reload(m);
}

/* ---- INT 67h ---- */

static bool ems_handle_ok(const ExtState *s, uint16_t h) { return h < EMS_MAX_HANDLES && s->ems[h].used; }

static uint8_t ems_allocate(ExtState *s, uint16_t pages, uint16_t *handle)
{
    if (pages == 0)
        return EMS_ZERO_PAGES;
    if (pages > s->ems_pages)
        return EMS_TOO_MANY_PAGES;
    if (pages > free_pages(s))
        return EMS_NOT_ENOUGH_FREE;
    uint16_t h = 1; /* handle 0 belongs to the operating system */
    while (h < EMS_MAX_HANDLES && s->ems[h].used)
        h++;
    if (h == EMS_MAX_HANDLES)
        return EMS_NO_HANDLES;
    s->ems[h] = (EmsHandle){ .used = 1, .pages = pages };
    for (uint16_t p = 0, n = 0; n < pages; p++)
        if (s->page_owner[p] == 0) {
            s->page_owner[p] = (uint16_t)(h + 1);
            s->page_logical[p] = n++;
        }
    *handle = h;
    return EMS_OK;
}

static uint8_t ems_map(Machine *m, ExtState *s, uint8_t slot, uint16_t logical, uint16_t h)
{
    if (!ems_handle_ok(s, h))
        return EMS_BAD_HANDLE;
    if (slot >= EMS_PHYS_PAGES)
        return EMS_BAD_PHYSICAL;
    if (logical != UNMAPPED && logical >= s->ems[h].pages)
        return EMS_BAD_LOGICAL;
    frame_write_back(m);
    s->map[slot][0] = logical == UNMAPPED ? UNMAPPED : h;
    s->map[slot][1] = logical;
    frame_reload(m);
    return EMS_OK;
}

static uint8_t ems_deallocate(Machine *m, ExtState *s, uint16_t h)
{
    if (!ems_handle_ok(s, h))
        return EMS_BAD_HANDLE;
    if (s->ems[h].saved)
        return EMS_SAVED_CONTEXT;
    frame_write_back(m);
    for (int slot = 0; slot < EMS_PHYS_PAGES; slot++)
        if (s->map[slot][0] == h)
            s->map[slot][0] = s->map[slot][1] = UNMAPPED;
    for (uint16_t p = 0; p < s->ems_pages; p++)
        if (s->page_owner[p] == h + 1)
            s->page_owner[p] = 0;
    s->ems[h].pages = 0;
    s->ems[h].used = h == 0; /* the system handle stays, with no pages */
    return EMS_OK;
}

static uint8_t ems_save_map(ExtState *s, uint16_t h)
{
    if (!ems_handle_ok(s, h))
        return EMS_BAD_HANDLE;
    if (s->ems[h].saved)
        return EMS_ALREADY_SAVED;
    memcpy(s->ems[h].saved_map, s->map, sizeof s->map);
    s->ems[h].saved = 1;
    return EMS_OK;
}

static uint8_t ems_restore_map(Machine *m, ExtState *s, uint16_t h)
{
    if (!ems_handle_ok(s, h))
        return EMS_BAD_HANDLE;
    if (!s->ems[h].saved)
        return EMS_NOT_SAVED;
    frame_write_back(m);
    memcpy(s->map, s->ems[h].saved_map, sizeof s->map);
    s->ems[h].saved = 0;
    frame_reload(m);
    return EMS_OK;
}

/* One end of a 57h move: type byte, handle, offset, segment or logical page. */
static uint8_t ems_move_end(Machine *m, uint32_t at, uint32_t len, Region *r, uint8_t *type_out)
{
    ExtState *s = m->ems;
    uint8_t type = mem_rd8(m, at);
    uint16_t h = mem_rd16(m, at + 1), off = mem_rd16(m, at + 3), seg_page = mem_rd16(m, at + 5);
    *type_out = type;
    if (type == 0) {
        uint32_t linear = real_addr(seg_page, off);
        if (linear + len > MEM_ONE_MEG)
            return EMS_CONV_OVERFLOW;
        *r = (Region){ .space = SPACE_CONV, .base = linear };
        return EMS_OK;
    }
    if (type != 1)
        return EMS_BAD_MEMORY_TYPE;
    if (!ems_handle_ok(s, h))
        return EMS_BAD_HANDLE;
    if (off >= EMS_PAGE_BYTES)
        return EMS_BAD_OFFSET;
    if (seg_page >= s->ems[h].pages)
        return EMS_BAD_LOGICAL;
    uint32_t start = (uint32_t)seg_page * EMS_PAGE_BYTES + off;
    if (start + len > (uint32_t)s->ems[h].pages * EMS_PAGE_BYTES)
        return EMS_REGION_OVERFLOW;
    *r = (Region){ .space = SPACE_EMS, .handle = h, .base = start, .cached_page = NO_PAGE };
    return EMS_OK;
}

static uint8_t ems_move(Machine *m, bool exchange)
{
    uint32_t at = real_addr(DS, SI);
    uint32_t len = mem_rd32(m, at);
    if (len > MOVE_MAX_BYTES)
        return EMS_MOVE_TOO_LONG;
    Region src, dst;
    uint8_t st, dt, rc;
    if ((rc = ems_move_end(m, at + 4, len, &src, &st)) != EMS_OK ||
        (rc = ems_move_end(m, at + 11, len, &dst, &dt)) != EMS_OK)
        return rc;
    region_move(m, &src, &dst, len, exchange);
    return EMS_OK;
}

CallbackResult ems_int67(Machine *m)
{
    ExtState *s = m->ems;
    uint8_t status = EMS_OK;
    uint16_t h = DX;
    switch (AH) {
    case 0x40: break;                                         /* status */
    case 0x41: BX = EMS_FRAME_SEG; break;                     /* page frame */
    case 0x42: BX = free_pages(s); DX = s->ems_pages; break;  /* page counts */
    case 0x43:                                                /* allocate */
        status = ems_allocate(s, BX, &h);
        if (status == EMS_OK)
            DX = h;
        break;
    case 0x44: status = ems_map(m, s, AL, BX, DX); break;
    case 0x45: status = ems_deallocate(m, s, DX); break;
    case 0x46: AL = EMS_VERSION; break;
    case 0x47: status = ems_save_map(s, DX); break;
    case 0x48: status = ems_restore_map(m, s, DX); break;
    case 0x4B: {                                              /* handle count */
        uint16_t n = 0;
        for (int i = 0; i < EMS_MAX_HANDLES; i++)
            n += s->ems[i].used;
        BX = n;
        break;
    }
    case 0x4C:                                                /* pages of one handle */
        if (!ems_handle_ok(s, DX))
            status = EMS_BAD_HANDLE;
        else
            BX = s->ems[DX].pages;
        break;
    case 0x4D: {                                              /* pages of every handle, at ES:DI */
        uint16_t n = 0;
        for (int i = 0; i < EMS_MAX_HANDLES; i++)
            if (s->ems[i].used) {
                mem_wr16(m, real_addr(ES, (uint16_t)(DI + 4 * n)), (uint16_t)i);
                mem_wr16(m, real_addr(ES, (uint16_t)(DI + 4 * n + 2)), s->ems[i].pages);
                n++;
            }
        BX = n;
        break;
    }
    case 0x57:                                                /* move or exchange a region */
        if (AL > 1)
            status = EMS_BAD_SUBFUNCTION;
        else
            status = ems_move(m, AL == 1);
        break;
    case 0x58:                                                /* mappable physical addresses */
        if (AL == 0)
            for (int slot = 0; slot < EMS_PHYS_PAGES; slot++) {
                mem_wr16(m, real_addr(ES, (uint16_t)(DI + 4 * slot)), (uint16_t)(EMS_FRAME_SEG + slot * (EMS_PAGE_BYTES >> 4)));
                mem_wr16(m, real_addr(ES, (uint16_t)(DI + 4 * slot + 2)), (uint16_t)slot);
            }
        if (AL > 1)
            status = EMS_BAD_SUBFUNCTION;
        else
            CX = EMS_PHYS_PAGES;
        break;
    default:
        status = EMS_BAD_FUNCTION;
        break;
    }
    if (m->trace)
        log_msg("ems: AH=%02X status %02X", AH, status);
    AH = status;
    return CB_DONE;
}

/* ---- XMS ---- */

static bool xms_handle_ok(const ExtState *s, uint16_t h) { return h >= 1 && h <= XMS_MAX_HANDLES && s->xms[h - 1].used; }

/* The free gaps of the pool in address order: the largest and the sum. */
static void xms_gaps(const ExtState *s, uint32_t *largest, uint32_t *total, uint32_t want, uint32_t *fit)
{
    uint32_t at = 0;
    *largest = *total = 0;
    if (fit)
        *fit = UINT32_MAX;
    for (;;) {
        uint32_t next = s->xms_kb; /* the start of the lowest block at or after at */
        int which = -1;
        for (int i = 0; i < XMS_MAX_HANDLES; i++)
            if (s->xms[i].used && s->xms[i].size_kb && s->xms[i].start_kb >= at && s->xms[i].start_kb < next)
                next = s->xms[i].start_kb, which = i;
        uint32_t gap = next - at;
        if (gap > *largest)
            *largest = gap;
        *total += gap;
        if (fit && *fit == UINT32_MAX && gap >= want)
            *fit = at;
        if (which < 0)
            return;
        at = s->xms[which].start_kb + s->xms[which].size_kb;
    }
}

static uint32_t xms_block_linear(const ExtState *s, uint16_t h) { return s->xms_pool + s->xms[h - 1].start_kb * 1024u; }

static void xms_fail(Machine *m, uint8_t code)
{
    AX = 0;
    BL = code;
}

/* One end of a 0Bh move: handle 0 is a real-mode seg:off. */
static bool xms_move_end(Machine *m, uint16_t h, uint32_t off, uint32_t len, Region *r)
{
    ExtState *s = m->xms;
    if (h == 0) {
        uint32_t linear = real_addr((uint16_t)(off >> 16), (uint16_t)off);
        if (linear + len > CONV_LIMIT)
            return false;
        *r = (Region){ .space = SPACE_CONV, .base = linear };
        return true;
    }
    if (off + len < off || off + len > s->xms[h - 1].size_kb * 1024u)
        return false;
    *r = (Region){ .space = SPACE_XMS, .base = xms_block_linear(s, h) + off };
    return true;
}

static void xms_move(Machine *m)
{
    ExtState *s = m->xms;
    uint32_t at = real_addr(DS, SI);
    uint32_t len = mem_rd32(m, at), src_off = mem_rd32(m, at + 6), dst_off = mem_rd32(m, at + 12);
    uint16_t src_h = mem_rd16(m, at + 4), dst_h = mem_rd16(m, at + 10);
    Region src, dst;
    if (src_h && !xms_handle_ok(s, src_h))
        return xms_fail(m, XMS_BAD_SRC_HANDLE);
    if (dst_h && !xms_handle_ok(s, dst_h))
        return xms_fail(m, XMS_BAD_DST_HANDLE);
    if (len > EXT_LIMIT)
        return xms_fail(m, XMS_BAD_LENGTH);
    if (!xms_move_end(m, src_h, src_off, len, &src))
        return xms_fail(m, XMS_BAD_SRC_OFFSET);
    if (!xms_move_end(m, dst_h, dst_off, len, &dst))
        return xms_fail(m, XMS_BAD_DST_OFFSET);
    region_move(m, &src, &dst, len, false);
    AX = 1;
    BL = 0;
}

static CallbackResult xms_driver(Machine *m)
{
    ExtState *s = m->xms;
    uint8_t fn = AH;
    if (m->trace)
        log_msg("xms: AH=%02X DX=%04X", fn, DX);
    switch (fn) {
    case 0x00: AX = XMS_VERSION; BX = XMS_REVISION; DX = 0; break; /* no HMA to hand out */
    case 0x01: xms_fail(m, XMS_NO_HMA); break;
    case 0x08: {
        uint32_t largest, total;
        xms_gaps(s, &largest, &total, 0, NULL);
        AX = (uint16_t)largest;
        DX = (uint16_t)total;
        BL = largest ? 0 : XMS_NO_MEMORY;
        break;
    }
    case 0x09: {
        uint32_t largest, total, fit;
        int free_slot = -1;
        for (int i = 0; i < XMS_MAX_HANDLES && free_slot < 0; i++)
            if (!s->xms[i].used)
                free_slot = i;
        xms_gaps(s, &largest, &total, DX, &fit);
        if (free_slot < 0) {
            xms_fail(m, XMS_NO_HANDLES);
        } else if (fit == UINT32_MAX) {
            xms_fail(m, XMS_NO_MEMORY);
        } else {
            s->xms[free_slot] = (XmsHandle){ .used = 1, .size_kb = DX, .start_kb = fit };
            AX = 1;
            DX = (uint16_t)(free_slot + 1);
            BL = 0;
        }
        break;
    }
    case 0x0A:
        if (!xms_handle_ok(s, DX))
            xms_fail(m, XMS_BAD_HANDLE);
        else if (s->xms[DX - 1].locks)
            xms_fail(m, XMS_LOCKED);
        else {
            s->xms[DX - 1] = (XmsHandle){ 0 };
            AX = 1;
            BL = 0;
        }
        break;
    case 0x0B: xms_move(m); break;
    case 0x0C:
        if (!xms_handle_ok(s, DX)) {
            xms_fail(m, XMS_BAD_HANDLE);
        } else {
            uint32_t linear = xms_block_linear(s, DX);
            s->xms[DX - 1].locks++;
            AX = 1;
            BX = (uint16_t)linear;
            DX = (uint16_t)(linear >> 16);
        }
        break;
    case 0x0D:
        if (!xms_handle_ok(s, DX))
            xms_fail(m, XMS_BAD_HANDLE);
        else if (!s->xms[DX - 1].locks)
            xms_fail(m, XMS_NOT_LOCKED);
        else {
            s->xms[DX - 1].locks--;
            AX = 1;
            BL = 0;
        }
        break;
    case 0x0E:
        if (!xms_handle_ok(s, DX)) {
            xms_fail(m, XMS_BAD_HANDLE);
        } else {
            uint8_t free_handles = 0;
            for (int i = 0; i < XMS_MAX_HANDLES; i++)
                free_handles += !s->xms[i].used;
            BH = s->xms[DX - 1].locks;
            BL = free_handles;
            DX = s->xms[DX - 1].size_kb;
            AX = 1;
        }
        break;
    default:
        xms_fail(m, XMS_NOT_IMPLEMENTED);
        break;
    }
    return CB_DONE;
}

CallbackResult xms_int2f(Machine *m)
{
    if (AX == MUX_XMS_PRESENT) {
        AL = XMS_INSTALLED;
    } else if (AX == MUX_XMS_ENTRY) {
        cpu_set_seg(&m->cpu, S_ES, CALLBACK_SEG);
        BX = ((ExtState *)m->xms)->xms_entry_off;
    }
    return CB_DONE;
}

uint32_t xms_entry(Machine *m)
{
    return m->xms ? (uint32_t)CALLBACK_SEG << 16 | ((ExtState *)m->xms)->xms_entry_off : 0;
}

/* ---- set-up ---- */

bool emsxms_init(Machine *m, unsigned ems_kb, unsigned xms_kb)
{
    uint32_t pages = ems_kb / (EMS_PAGE_BYTES / 1024);
    if (!pages && !xms_kb)
        return true;
    if (m->ems || m->xms || pages > EMS_MAX_PAGES || xms_kb > 0xFFFF)
        return false;
    uint32_t state_bytes = (sizeof(ExtState) + 0xFFF) & ~0xFFFu;
    uint32_t xms_pool = EXT_MEM_BASE + state_bytes;
    uint32_t ems_pool = xms_pool + xms_kb * 1024u;
    uint32_t end = ems_pool + pages * EMS_PAGE_BYTES;
    if (end > EXT_LIMIT)
        return false;
    ExtState *s = (ExtState *)(m->mem + EXT_MEM_BASE);
    memset(m->mem + EXT_MEM_BASE, 0, end - EXT_MEM_BASE);
    s->xms_pool = xms_pool;
    s->ems_pool = ems_pool;
    s->ems_pages = (uint16_t)pages;
    s->xms_kb = (uint16_t)xms_kb;
    for (int slot = 0; slot < EMS_PHYS_PAGES; slot++)
        s->map[slot][0] = s->map[slot][1] = UNMAPPED;
    if (pages) {
        s->ems[0].used = 1;
        memset(m->mem + FRAME_LINEAR, 0, FRAME_BYTES);
        m->ems = s;
    }
    if (xms_kb) {
        int id = callback_register(m, "xms driver", xms_driver, -1);
        m->mem[callback_stub_addr(id) + 3] = 0xCB; /* RETF: the driver is far-called */
        s->xms_entry_off = (uint16_t)(callback_stub_addr(id) - ((uint32_t)CALLBACK_SEG << 4));
        m->xms = s;
    }
    m->snapshot_bytes = end;
    log_msg("emsxms: EMS %u KB (%u pages at %04X:0000), XMS %u KB", (unsigned)pages * 16, (unsigned)pages,
            EMS_FRAME_SEG, xms_kb);
    return true;
}
