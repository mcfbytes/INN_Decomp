/* Expanded (LIM EMS 4.0, INT 67h) and extended (XMS 3.0, INT 2Fh AX=43xxh) memory, both opt-in.
 * Off by default: INT 67h then answers 84h, INT 2Fh AX=4300h answers AL=0 and EMMXXXX0 does not
 * exist. Design notes: docs/runtime.md, "Conventional memory". */
#ifndef DOSRT_EMSXMS_H
#define DOSRT_EMSXMS_H

#include <stdbool.h>
#include <stdint.h>

#include "machine.h"

enum {
    EMS_FRAME_SEG = MEM_EMS_FRAME >> 4, /* four 16 KB physical pages at E000:0000-E000:FFFF */
    EMS_PAGE_BYTES = 0x4000,
    EMS_PHYS_PAGES = 4,
    EXT_MEM_BASE = MEM_ONE_MEG + 0x10000, /* above the HMA: real-mode code cannot address it */
};

/* Turns on EMS with ems_kb KB (rounded down to 16 KB pages) and XMS with xms_kb KB; 0 leaves one
 * off. Call once, after dos_init and before the program starts. Their memory and the handle tables
 * live in Machine.mem above 1 MB + 64 KB, and Machine.snapshot_bytes grows to cover them, so
 * shadow-verification snapshots carry the state. Returns false (and changes nothing) when the sizes
 * do not fit in the machine's 16 MB. */
bool emsxms_init(Machine *m, unsigned ems_kb, unsigned xms_kb);

/* INT 67h when EMS is on (dos.c routes it here). */
CallbackResult ems_int67(Machine *m);
/* INT 2Fh AX=4300h/4310h when XMS is on: AL=80h, ES:BX = the driver entry. */
CallbackResult xms_int2f(Machine *m);

/* For tests and tools: the guest-visible XMS driver entry (seg:off), or 0 when XMS is off. */
uint32_t xms_entry(Machine *m);

#endif
