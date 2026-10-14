/* Parses the overlay trailer Borland's linker appends to an overlaid MZ program (Borland C++ 3.x as
 * observed in DARKSTRT and FATES); the layout is in docs/lifting.md, "Overlays". */
#include "borland_overlay.h"

#include <string.h>

enum {
    MZ_LAST_PAGE = 2, MZ_PAGES = 4, MZ_HEADER_PARAS = 8, PAGE = 512, PARA = 16,
    TRAILER_BYTES = 16, SEG_ROW_BYTES = 8, SEG_FLAGS_OVERLAY_STUB = 3,
    STUB_CODE_OFF = 4, STUB_CODE_BYTES = 8, STUB_FIXUP_BYTES = 10, STUB_HEADER_BYTES = 0x20,
    INT_OPCODE = 0xCD, OVERLAY_VECTOR = 0x3F,
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (uint32_t)rd16(p + 2) << 16; }

int borland_overlays_parse(const uint8_t *exe, size_t len, BorlandOverlay *out, int max)
{
    if (len < 0x20 || !((exe[0] == 'M' && exe[1] == 'Z') || (exe[0] == 'Z' && exe[1] == 'M')))
        return 0;
    uint16_t last = rd16(exe + MZ_LAST_PAGE), pages = rd16(exe + MZ_PAGES);
    size_t module_end = (size_t)pages * PAGE - (last ? PAGE - last : 0);
    size_t header = (size_t)rd16(exe + MZ_HEADER_PARAS) * PARA;
    if (module_end + TRAILER_BYTES > len || memcmp(exe + module_end, "FBOV", 4) != 0)
        return 0;
    size_t data_start = module_end + TRAILER_BYTES;
    uint32_t data_bytes = rd32(exe + module_end + 4), table = rd32(exe + module_end + 8);
    uint32_t rows = rd32(exe + module_end + 12);
    if (data_start + data_bytes > len || table + (uint64_t)rows * SEG_ROW_BYTES > len)
        return -1;
    int count = 0;
    for (uint32_t i = 0; i < rows; i++) {
        const uint8_t *row = exe + table + i * SEG_ROW_BYTES;
        if (rd16(row + 4) != SEG_FLAGS_OVERLAY_STUB)
            continue;
        size_t stub = header + (size_t)rd16(row) * PARA;
        if (stub + STUB_HEADER_BYTES > len || exe[stub] != INT_OPCODE || exe[stub + 1] != OVERLAY_VECTOR)
            return -1;
        BorlandOverlay o = {
            .stub_seg = rd16(row),
            .file_off = (uint32_t)data_start + rd32(exe + stub + STUB_CODE_OFF),
            .code_bytes = rd16(exe + stub + STUB_CODE_BYTES),
            .fixup_bytes = rd16(exe + stub + STUB_FIXUP_BYTES),
        };
        if ((uint64_t)o.file_off + o.code_bytes + o.fixup_bytes > data_start + data_bytes)
            return -1;
        if (count < max)
            out[count] = o;
        count++;
    }
    return count < max ? count : max;
}
