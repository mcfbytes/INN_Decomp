/* Semantic comparison of device access during shadow verification (docs/lifting.md, "Semantic port
 * comparison"). A converted driver function calls host APIs instead of touching ports; the original's
 * recorded port I/O and the converted function's calls are both reduced to the same events and compared. */
#ifndef LIFT_SEMANTIC_H
#define LIFT_SEMANTIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "machine.h"

typedef enum SemKind {
    SEM_OPL_WRITE,     /* a = register, b = value */
    SEM_DAC_SET,       /* a = index, b = r << 16 | g << 8 | b (6-bit each) */
    SEM_DAC_GET,       /* a = index */
    SEM_CRTC_START,    /* a = start address */
    SEM_PIT_DIVISOR,   /* a = channel, b = divisor */
    SEM_PIC_EOI,       /* a = chip, b = 0 (non-specific) */
    SEM_PIC_MASK,      /* a = chip, b = mask */
    SEM_KBD_DATA,      /* read; value replayed */
    SEM_KBD_ACK,
    SEM_KIND_COUNT
} SemKind;

/* Device backends call these in hybrid-hw builds. While the lifted run replays (IO_REPLAY) the event is
 * recorded for comparison and reads return the original's value; otherwise they do nothing (the
 * backend drives the device itself). */
void lift_semantic_event(Machine *m, SemKind kind, uint32_t a, uint32_t b);
uint32_t lift_semantic_read(Machine *m, SemKind kind, uint32_t a, uint32_t live_value);

/* ---- canonical traces (used by lift_verify.c; exposed for tests) ---- */

typedef struct SemEvent { SemKind kind; uint32_t a, b; } SemEvent;
typedef struct SemRaw { uint16_t port; uint8_t size; bool is_out; uint32_t value; } SemRaw;

typedef struct SemHold { SemRaw ev[4]; int n; } SemHold;

/* The events of one run: semantic ones, and the port accesses that have no semantic meaning, each in order. */
typedef struct SemTrace {
    SemEvent *sem;
    size_t nsem, capsem;
    SemRaw *raw;
    size_t nraw, capraw;
    size_t read_cursor[SEM_KIND_COUNT]; /* sem_trace_next_read */
    /* canonicaliser state: accesses held until their group is complete */
    SemHold opl[4], dac_w, dac_r, crtc_reg[2], pit, kbd61;
    int dac_w_index, dac_r_index, crtc_selected;
} SemTrace;

void sem_trace_init(SemTrace *t);   /* empties the trace; keeps its buffers */
void sem_trace_free(SemTrace *t);
/* Feeds one recorded port access through the canonicaliser. */
void sem_trace_port(SemTrace *t, uint16_t port, int size, uint32_t value, bool is_out);
/* Appends an event that the converted code reported directly. */
void sem_trace_event(SemTrace *t, SemKind kind, uint32_t a, uint32_t b);
/* Ends the run: groups that never completed become raw accesses. */
void sem_trace_finish(SemTrace *t);
/* The next not yet replayed event of this kind (b is its value); false when there is none. */
bool sem_trace_next_read(SemTrace *t, SemKind kind, uint32_t *b);
/* Compares the lifted run's trace with the original's; on a difference fills why with
 * "semantic event K: lifted <...>, original <...>" (or "port event K: ...") and returns false. */
bool sem_trace_compare(const SemTrace *lifted, const SemTrace *original, char *why, size_t cap);

#endif
