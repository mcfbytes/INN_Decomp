/* Borland overlay (VROOMM) layout of an MZ file, as far as the runtime needs it to find overlay code
 * in the overlay manager's file reads. Format notes: docs/lifting.md, "Overlays". */
#ifndef DOSRT_BORLAND_OVERLAY_H
#define DOSRT_BORLAND_OVERLAY_H

#include <stddef.h>
#include <stdint.h>

/* One overlay: its stub segment in the resident image names it; its code and fixups sit at
 * file_off in the EXE and are read into the overlay buffer together. */
typedef struct BorlandOverlay {
    uint16_t stub_seg;    /* image-relative segment of the INT 3Fh stub */
    uint32_t file_off;    /* absolute file offset of the code */
    uint16_t code_bytes;
    uint16_t fixup_bytes; /* words after the code: offsets of words that get the load segment added */
} BorlandOverlay;

/* Lists the overlays of an MZ file with an "FBOV" trailer into out (at most max).
 * Returns the number found (0 for a program without overlays) or -1 for a malformed trailer. */
int borland_overlays_parse(const uint8_t *exe, size_t len, BorlandOverlay *out, int max);

#endif
