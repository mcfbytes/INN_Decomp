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
    /* Sound Blaster DSP at 220h and 8237 channel 1 (docs/lifting.md, "Semantic port comparison") */
    SEM_DMA_MASK,         /* a = channel, b = 1 masked, 0 unmasked */
    SEM_DMA_PROGRAM,      /* a = mode << 24 | page << 16 | address, b = count */
    SEM_SB_RESET,
    SEM_SB_DATA,          /* 22Ah read; value replayed */
    SEM_SB_IDENT,         /* a = E0h (b = its argument) or E1h (b = 0) */
    SEM_SB_TIME_CONSTANT, /* a = time constant */
    SEM_SB_BLOCK_SIZE,    /* 48h: a = length field (bytes - 1) */
    SEM_SB_PCM_START,     /* a = 1 auto-init (1Ch, 90h) or 0 (14h, 91h), b = length field (14h's, else the last 48h's) */
    SEM_SB_PCM_STOP,      /* a = 0 halt (D0h), 1 end auto-init after this block (DAh) */
    SEM_SB_PCM_CONTINUE,  /* D4h */
    SEM_SB_SPEAKER,       /* a = 1 on (D1h), 0 off (D3h) */
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
    /* Sound Blaster: channel 1's programming (held until mode, address, count and page are all in), the
     * DSP reset, and the DSP command being received with its argument bytes */
    SemRaw dma_ev[8];
    int dma_n;
    uint8_t dma_have, dma_mode, dma_page;
    uint16_t dma_addr, dma_count;
    bool dma_flipflop;
    SemHold sb_reset, sb_cmd;
    int sb_args_needed;
    uint32_t sb_block; /* the last 48h length field, SB_NO_BLOCK when none */
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
