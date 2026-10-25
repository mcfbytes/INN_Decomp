/* Canonicalisation of recorded port I/O into semantic events, and their comparison. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lift_internal.h"
#include "lift_semantic.h"

enum {
    OPL_PORTS = 4,
    PORT_PIT_CH0 = 0x40,
    PORT_PIT_CONTROL = 0x43,
    PORT_KBD_DATA = 0x60,
    PORT_KBD_CTRL = 0x61,
    PORT_PIC_MASTER = 0x20,
    PORT_PIC_SLAVE = 0xA0,
    PORT_DAC_READ_INDEX = 0x3C7,
    PORT_DAC_WRITE_INDEX = 0x3C8,
    PORT_DAC_DATA = 0x3C9,
    PORT_CRTC_INDEX = 0x3D4,
    PORT_CRTC_DATA = 0x3D5,
    CRTC_START_HIGH = 0x0C,
    CRTC_START_LOW = 0x0D,
    PIT_CONTROL_CH0_WORD = 0x30, /* channel 0 (bits 7-6 clear), access lobyte/hibyte */
    PIT_CONTROL_CHANNEL_MASK = 0xC0,
    PIT_CONTROL_ACCESS_MASK = 0x30,
    PIC_NON_SPECIFIC_EOI = 0x20,
    KBD_ACK_STROBE = 0x80,
    /* 8237 channel 1 and the Sound Blaster DSP (base 220h, DMA 1) */
    DMA_ADDRESS1 = 0x02, DMA_COUNT1 = 0x03, DMA_SINGLE_MASK = 0x0A, DMA_MODE = 0x0B, DMA_CLEAR_FLIPFLOP = 0x0C,
    DMA_MASTER_CLEAR = 0x0D, DMA_PAGE1 = 0x83, DMA_LAST_REG = 0x0F, DMA_CHANNEL1 = 1, DMA_MASK_BIT = 0x04,
    DMA_HAVE_MODE = 1, DMA_HAVE_ADDR_LO = 2, DMA_HAVE_ADDR_HI = 4, DMA_HAVE_COUNT_LO = 8, DMA_HAVE_COUNT_HI = 16,
    DMA_HAVE_PAGE = 32, DMA_HAVE_ALL = 63,
    SB_RESET_PORT = 0x226, SB_READ_PORT = 0x22A, SB_WRITE_PORT = 0x22C, SB_STATUS_PORT = 0x22E,
    DSP_DMA_SINGLE = 0x14, DSP_DMA_AUTO = 0x1C, DSP_TIME_CONSTANT = 0x40, DSP_BLOCK_SIZE = 0x48,
    DSP_HS_AUTO = 0x90, DSP_HS_SINGLE = 0x91, DSP_HALT = 0xD0, DSP_SPEAKER_ON = 0xD1, DSP_SPEAKER_OFF = 0xD3,
    DSP_CONTINUE = 0xD4, DSP_EXIT_AUTO = 0xDA, DSP_IDENTIFY = 0xE0, DSP_VERSION = 0xE1,
};

static const uint32_t SB_NO_BLOCK = 0xFFFFFFFFu;

static const uint16_t OPL_INDEX_PORTS[OPL_PORTS] = { 0x388, 0x228, 0x220, 0x222 };

/* ---- trace storage ---- */

static void *grow(void *p, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap)
        return p;
    size_t n = *cap ? *cap * 2 : 256;
    while (n < need)
        n *= 2;
    void *q = realloc(p, n * elem);
    if (!q)
        fatal("out of memory");
    *cap = n;
    return q;
}

static void push_raw(SemTrace *t, SemRaw r)
{
    t->raw = grow(t->raw, &t->capraw, t->nraw + 1, sizeof *t->raw);
    t->raw[t->nraw++] = r;
}

void sem_trace_event(SemTrace *t, SemKind kind, uint32_t a, uint32_t b)
{
    t->sem = grow(t->sem, &t->capsem, t->nsem + 1, sizeof *t->sem);
    t->sem[t->nsem++] = (SemEvent){ kind, a, b };
}

void sem_trace_init(SemTrace *t)
{
    SemEvent *sem = t->sem;
    SemRaw *raw = t->raw;
    size_t capsem = t->capsem, capraw = t->capraw;
    memset(t, 0, sizeof *t);
    t->sem = sem;
    t->raw = raw;
    t->capsem = capsem;
    t->capraw = capraw;
    t->crtc_selected = -1;
    t->sb_block = SB_NO_BLOCK;
}

void sem_trace_free(SemTrace *t)
{
    free(t->sem);
    free(t->raw);
    memset(t, 0, sizeof *t);
}

static void hold_add(SemHold *h, uint16_t port, int size, uint32_t value, bool is_out)
{
    if (h->n < (int)(sizeof h->ev / sizeof h->ev[0]))
        h->ev[h->n++] = (SemRaw){ port, (uint8_t)size, is_out, value };
}

static void hold_flush(SemTrace *t, SemHold *h)
{
    for (int i = 0; i < h->n; i++)
        push_raw(t, h->ev[i]);
    h->n = 0;
}

/* ---- per-device recognisers; each returns true when it consumed the access ---- */

static bool opl_port(SemTrace *t, uint16_t port, int size, uint32_t value, bool is_out)
{
    for (int i = 0; i < OPL_PORTS; i++) {
        if (port == OPL_INDEX_PORTS[i]) {
            if (!is_out)
                return true; /* status read: a delay */
            hold_flush(t, &t->opl[i]);
            if (size == 2) { /* index in AL, data in AH */
                sem_trace_event(t, SEM_OPL_WRITE, value & 0xFF, value >> 8 & 0xFF);
                return true;
            }
            hold_add(&t->opl[i], port, size, value, true);
            return true;
        }
        if (port == OPL_INDEX_PORTS[i] + 1 && is_out && t->opl[i].n) {
            sem_trace_event(t, SEM_OPL_WRITE, t->opl[i].ev[0].value & 0xFF, value & 0xFF);
            t->opl[i].n = 0;
            return true;
        }
    }
    return false;
}

static uint32_t pack_rgb(const SemHold *h)
{
    return (h->ev[0].value & 0xFF) << 16 | (h->ev[1].value & 0xFF) << 8 | (h->ev[2].value & 0xFF);
}

static void dac_data(SemTrace *t, SemHold *h, int *index, bool is_out, uint32_t value)
{
    hold_add(h, PORT_DAC_DATA, 1, value, is_out);
    if (h->n < 3)
        return;
    sem_trace_event(t, is_out ? SEM_DAC_SET : SEM_DAC_GET, (uint32_t)*index, pack_rgb(h));
    *index = (*index + 1) & 0xFF;
    h->n = 0;
}

static bool dac_port(SemTrace *t, uint16_t port, int size, uint32_t value, bool is_out)
{
    switch (port) {
    case PORT_DAC_READ_INDEX:
        if (!is_out)
            return false;
        hold_flush(t, &t->dac_r);
        t->dac_r_index = (int)(value & 0xFF);
        return true;
    case PORT_DAC_WRITE_INDEX:
        if (!is_out)
            return false;
        hold_flush(t, &t->dac_w);
        t->dac_w_index = (int)(value & 0xFF);
        if (size == 2)
            dac_data(t, &t->dac_w, &t->dac_w_index, true, value >> 8);
        return true;
    case PORT_DAC_DATA:
        if (size != 1)
            return false;
        if (is_out)
            dac_data(t, &t->dac_w, &t->dac_w_index, true, value);
        else
            dac_data(t, &t->dac_r, &t->dac_r_index, false, value);
        return true;
    default:
        return false;
    }
}

/* Index 0Ch/0Dh writes and their data are held; once both halves are in, one event replaces them. */
static void crtc_data(SemTrace *t, int reg, uint32_t value, int size)
{
    SemHold *h = &t->crtc_reg[reg == CRTC_START_HIGH ? 0 : 1];
    hold_flush(t, h);
    hold_add(h, PORT_CRTC_DATA, size, value, true);
    SemHold *hi = &t->crtc_reg[0], *lo = &t->crtc_reg[1];
    if (hi->n && lo->n) {
        sem_trace_event(t, SEM_CRTC_START, (hi->ev[0].value & 0xFF) << 8 | (lo->ev[0].value & 0xFF), 0);
        hi->n = lo->n = 0;
    }
}

static bool crtc_port(SemTrace *t, uint16_t port, int size, uint32_t value, bool is_out)
{
    if (!is_out)
        return false;
    if (port == PORT_CRTC_INDEX) {
        int reg = (int)(value & 0xFF);
        if (reg != CRTC_START_HIGH && reg != CRTC_START_LOW) {
            t->crtc_selected = -1;
            return false;
        }
        t->crtc_selected = reg;
        if (size == 2) /* OUT DX,AX: index in AL, data in AH */
            crtc_data(t, reg, value >> 8, 1);
        return true;
    }
    if (port == PORT_CRTC_DATA && size == 1 && t->crtc_selected >= 0) {
        crtc_data(t, t->crtc_selected, value, 1);
        return true;
    }
    return false;
}

static bool pit_port(SemTrace *t, uint16_t port, int size, uint32_t value, bool is_out)
{
    if (!is_out || size != 1)
        return false;
    if (port == PORT_PIT_CONTROL) {
        hold_flush(t, &t->pit);
        if ((value & PIT_CONTROL_CHANNEL_MASK) != 0 || (value & PIT_CONTROL_ACCESS_MASK) != PIT_CONTROL_CH0_WORD)
            return false;
        hold_add(&t->pit, port, size, value, true);
        return true;
    }
    if (port == PORT_PIT_CH0 && t->pit.n) {
        hold_add(&t->pit, port, size, value, true);
        if (t->pit.n == 3) {
            sem_trace_event(t, SEM_PIT_DIVISOR, 0, (t->pit.ev[1].value & 0xFF) | (t->pit.ev[2].value & 0xFF) << 8);
            t->pit.n = 0;
        }
        return true;
    }
    return false;
}

static bool pic_port(SemTrace *t, uint16_t port, int size, uint32_t value, bool is_out)
{
    uint32_t chip = port == PORT_PIC_MASTER || port == PORT_PIC_MASTER + 1 ? 0 : 1;
    bool command = port == PORT_PIC_MASTER || port == PORT_PIC_SLAVE;
    bool mask = port == PORT_PIC_MASTER + 1 || port == PORT_PIC_SLAVE + 1;
    if (size != 1 || (!command && !mask))
        return false;
    if (mask) {
        if (is_out)
            sem_trace_event(t, SEM_PIC_MASK, chip, value & 0xFF);
        return true; /* mask reads are dropped */
    }
    if (is_out && (value & 0xFF) == PIC_NON_SPECIFIC_EOI) {
        sem_trace_event(t, SEM_PIC_EOI, chip, 0);
        return true;
    }
    return false;
}

/* Port 61h: a read, a write with bit 7 set and a write with it clear form one acknowledge. */
static bool kbd_port(SemTrace *t, uint16_t port, int size, uint32_t value, bool is_out)
{
    if (size != 1)
        return false;
    if (port == PORT_KBD_DATA && !is_out) {
        sem_trace_event(t, SEM_KBD_DATA, 0, value & 0xFF);
        return true;
    }
    if (port != PORT_KBD_CTRL)
        return false;
    SemHold *h = &t->kbd61;
    bool set = is_out && (value & KBD_ACK_STROBE);
    bool held_set = h->n && h->ev[h->n - 1].is_out && (h->ev[h->n - 1].value & KBD_ACK_STROBE);
    if (is_out && !set && held_set) {
        sem_trace_event(t, SEM_KBD_ACK, 0, 0);
        h->n = 0;
        return true;
    }
    bool continues = set && (h->n == 0 || (h->n == 1 && !h->ev[0].is_out));
    if (!continues)
        hold_flush(t, h);
    hold_add(h, port, size, value, is_out);
    return true;
}

/* ---- 8237 channel 1: mask writes are events; the flip-flop clear, mode, both address and count bytes and
 * the page are held, in any order, until all are in and become one program event ---- */

static void dma_flush(SemTrace *t)
{
    for (int i = 0; i < t->dma_n; i++)
        push_raw(t, t->dma_ev[i]);
    t->dma_n = 0;
    t->dma_have = 0;
}

/* One access of the program group; have is the part it sets (0 for the flip-flop clear). */
static void dma_hold(SemTrace *t, uint16_t port, uint32_t value, uint8_t have)
{
    if ((t->dma_have & have) || t->dma_n == (int)(sizeof t->dma_ev / sizeof t->dma_ev[0]))
        dma_flush(t); /* a part written twice: what was held does not form a program */
    t->dma_ev[t->dma_n++] = (SemRaw){ port, 1, true, value };
    t->dma_have |= have;
    if (t->dma_have != DMA_HAVE_ALL)
        return;
    sem_trace_event(t, SEM_DMA_PROGRAM, (uint32_t)t->dma_mode << 24 | (uint32_t)t->dma_page << 16 | t->dma_addr,
                    t->dma_count);
    t->dma_n = 0;
    t->dma_have = 0;
}

static void dma_word_byte(SemTrace *t, uint16_t port, uint8_t v)
{
    bool hi = t->dma_flipflop;
    t->dma_flipflop = !t->dma_flipflop;
    uint16_t *word = port == DMA_ADDRESS1 ? &t->dma_addr : &t->dma_count;
    uint8_t lo_bit = port == DMA_ADDRESS1 ? DMA_HAVE_ADDR_LO : DMA_HAVE_COUNT_LO;
    *word = hi ? (uint16_t)((*word & 0x00FF) | v << 8) : (uint16_t)((*word & 0xFF00) | v);
    dma_hold(t, port, v, hi ? (uint8_t)(lo_bit << 1) : lo_bit);
}

static bool dma_port(SemTrace *t, uint16_t port, int size, uint32_t value, bool is_out)
{
    if (port > DMA_LAST_REG && port != DMA_PAGE1)
        return false;
    uint8_t v = (uint8_t)value;
    if (port < 0x08) { /* address and count registers: every access steps the flip-flop */
        bool ours = is_out && size == 1 && (port == DMA_ADDRESS1 || port == DMA_COUNT1);
        if (ours) {
            dma_word_byte(t, port, v);
            return true;
        }
        t->dma_flipflop = !t->dma_flipflop;
        return false;
    }
    if (!is_out || size != 1)
        return false;
    switch (port) {
    case DMA_SINGLE_MASK:
        if ((v & 3) != DMA_CHANNEL1)
            return false;
        sem_trace_event(t, SEM_DMA_MASK, DMA_CHANNEL1, (v & DMA_MASK_BIT) != 0);
        return true;
    case DMA_MODE:
        if ((v & 3) != DMA_CHANNEL1)
            return false;
        t->dma_mode = v;
        dma_hold(t, port, v, DMA_HAVE_MODE);
        return true;
    case DMA_CLEAR_FLIPFLOP:
        t->dma_flipflop = false;
        dma_hold(t, port, v, 0);
        return true;
    case DMA_PAGE1:
        t->dma_page = v;
        dma_hold(t, port, v, DMA_HAVE_PAGE);
        return true;
    case DMA_MASTER_CLEAR:
        t->dma_flipflop = false;
        return false;
    default:
        return false;
    }
}

/* ---- Sound Blaster DSP: 226h 1 then 0 is a reset; 22Ch writes form commands with their argument bytes;
 * 22Ah reads are data; 22Ch and 22Eh reads (write-ready and data-ready polls, the IRQ acknowledge) are
 * dropped ---- */

static int dsp_args(uint8_t cmd)
{
    switch (cmd) {
    case 0x10: case DSP_TIME_CONSTANT: case DSP_IDENTIFY: case 0xE4: case 0x38:
        return 1;
    case DSP_DMA_SINGLE: case DSP_BLOCK_SIZE:
    case 0x16: case 0x17: case 0x24: case 0x74: case 0x75: case 0x76: case 0x77: case 0x80:
        return 2;
    default:
        return 0;
    }
}

static uint32_t hold_word(const SemHold *h) { return (h->ev[1].value & 0xFF) | (h->ev[2].value & 0xFF) << 8; }

/* A complete command: its event, or its bytes as raw accesses when it has no meaning here. */
static void dsp_command(SemTrace *t)
{
    SemHold *h = &t->sb_cmd;
    uint8_t cmd = (uint8_t)h->ev[0].value;
    switch (cmd) {
    case DSP_TIME_CONSTANT: sem_trace_event(t, SEM_SB_TIME_CONSTANT, h->ev[1].value & 0xFF, 0); break;
    case DSP_BLOCK_SIZE:
        t->sb_block = hold_word(h);
        sem_trace_event(t, SEM_SB_BLOCK_SIZE, t->sb_block, 0);
        break;
    case DSP_DMA_SINGLE: sem_trace_event(t, SEM_SB_PCM_START, 0, hold_word(h)); break;
    case DSP_HS_SINGLE: sem_trace_event(t, SEM_SB_PCM_START, 0, t->sb_block); break;
    case DSP_DMA_AUTO: case DSP_HS_AUTO: sem_trace_event(t, SEM_SB_PCM_START, 1, t->sb_block); break;
    case DSP_HALT: sem_trace_event(t, SEM_SB_PCM_STOP, 0, 0); break;
    case DSP_EXIT_AUTO: sem_trace_event(t, SEM_SB_PCM_STOP, 1, 0); break;
    case DSP_CONTINUE: sem_trace_event(t, SEM_SB_PCM_CONTINUE, 0, 0); break;
    case DSP_SPEAKER_ON: sem_trace_event(t, SEM_SB_SPEAKER, 1, 0); break;
    case DSP_SPEAKER_OFF: sem_trace_event(t, SEM_SB_SPEAKER, 0, 0); break;
    case DSP_IDENTIFY: sem_trace_event(t, SEM_SB_IDENT, DSP_IDENTIFY, h->ev[1].value & 0xFF); break;
    case DSP_VERSION: sem_trace_event(t, SEM_SB_IDENT, DSP_VERSION, 0); break;
    default: hold_flush(t, h); return;
    }
    h->n = 0;
}

static bool sb_port(SemTrace *t, uint16_t port, int size, uint32_t value, bool is_out)
{
    if (size != 1)
        return false;
    switch (port) {
    case SB_RESET_PORT:
        if (!is_out)
            return false;
        if (value & 1) {
            hold_flush(t, &t->sb_reset);
            hold_add(&t->sb_reset, port, size, value, true);
            return true;
        }
        if (!t->sb_reset.n)
            return false;
        t->sb_reset.n = 0;
        hold_flush(t, &t->sb_cmd); /* an unfinished command is abandoned */
        t->sb_args_needed = 0;
        sem_trace_event(t, SEM_SB_RESET, 0, 0);
        return true;
    case SB_READ_PORT:
        if (is_out)
            return false;
        sem_trace_event(t, SEM_SB_DATA, 0, value & 0xFF);
        return true;
    case SB_STATUS_PORT:
        return !is_out; /* data-ready poll and IRQ acknowledge */
    case SB_WRITE_PORT:
        if (!is_out)
            return true; /* write-ready poll */
        if (!t->sb_cmd.n)
            t->sb_args_needed = dsp_args((uint8_t)value);
        hold_add(&t->sb_cmd, port, size, value, true);
        if (t->sb_cmd.n > t->sb_args_needed)
            dsp_command(t);
        return true;
    default:
        return false;
    }
}

void sem_trace_port(SemTrace *t, uint16_t port, int size, uint32_t value, bool is_out)
{
    if (opl_port(t, port, size, value, is_out) || dac_port(t, port, size, value, is_out) ||
        crtc_port(t, port, size, value, is_out) || pit_port(t, port, size, value, is_out) ||
        pic_port(t, port, size, value, is_out) || kbd_port(t, port, size, value, is_out) ||
        dma_port(t, port, size, value, is_out) || sb_port(t, port, size, value, is_out))
        return;
    if (port == 0x3DA && !is_out)
        return; /* input status: retrace polling */
    push_raw(t, (SemRaw){ port, (uint8_t)size, is_out, value });
}

void sem_trace_finish(SemTrace *t)
{
    for (int i = 0; i < OPL_PORTS; i++)
        hold_flush(t, &t->opl[i]);
    hold_flush(t, &t->dac_w);
    hold_flush(t, &t->dac_r);
    hold_flush(t, &t->crtc_reg[0]);
    hold_flush(t, &t->crtc_reg[1]);
    hold_flush(t, &t->pit);
    hold_flush(t, &t->kbd61);
    dma_flush(t);
    hold_flush(t, &t->sb_reset);
    hold_flush(t, &t->sb_cmd);
    t->sb_args_needed = 0;
}

bool sem_trace_next_read(SemTrace *t, SemKind kind, uint32_t *b)
{
    size_t *c = &t->read_cursor[kind];
    while (*c < t->nsem && t->sem[*c].kind != kind)
        (*c)++;
    if (*c >= t->nsem)
        return false;
    *b = t->sem[(*c)++].b;
    return true;
}

/* ---- comparison ---- */

static const char *const KIND_NAME[SEM_KIND_COUNT] = {
    "opl_write", "dac_set", "dac_get", "crtc_start", "pit_divisor", "pic_eoi", "pic_mask", "kbd_data", "kbd_ack",
    "dma_mask", "dma_program", "sb_reset", "sb_data", "sb_ident", "sb_time_constant", "sb_block_size",
    "sb_pcm_start", "sb_pcm_stop", "sb_pcm_continue", "sb_speaker",
};

static bool is_read_kind(SemKind k) { return k == SEM_DAC_GET || k == SEM_KBD_DATA || k == SEM_SB_DATA; }

static void describe_sem(const SemEvent *e, char *out, size_t cap)
{
    if (!e)
        snprintf(out, cap, "none");
    else
        snprintf(out, cap, "%s(%X, %X)", KIND_NAME[e->kind], e->a, e->b);
}

static void describe_raw(const SemRaw *e, char *out, size_t cap)
{
    if (!e)
        snprintf(out, cap, "none");
    else
        snprintf(out, cap, "%s %X size %d value %X", e->is_out ? "out" : "in", e->port, e->size, e->value);
}

static bool sem_equal(const SemEvent *x, const SemEvent *y)
{
    if (x->kind != y->kind)
        return false;
    if (x->kind == SEM_KBD_DATA || x->kind == SEM_KBD_ACK)
        return true; /* no address; a data byte is replayed */
    return x->a == y->a && (is_read_kind(x->kind) || x->b == y->b);
}

/* A read's value is replayed from the original, so only the access itself is compared. */
static bool raw_equal(const SemRaw *x, const SemRaw *y)
{
    return x->port == y->port && x->size == y->size && x->is_out == y->is_out && (!x->is_out || x->value == y->value);
}

bool sem_trace_compare(const SemTrace *lifted, const SemTrace *original, char *why, size_t cap)
{
    char l[96], o[96];
    size_t n = lifted->nsem > original->nsem ? lifted->nsem : original->nsem;
    for (size_t k = 0; k < n; k++) {
        const SemEvent *x = k < lifted->nsem ? &lifted->sem[k] : NULL;
        const SemEvent *y = k < original->nsem ? &original->sem[k] : NULL;
        if (x && y && sem_equal(x, y))
            continue;
        describe_sem(x, l, sizeof l);
        describe_sem(y, o, sizeof o);
        snprintf(why, cap, "semantic event %zu: lifted %s, original %s", k, l, o);
        return false;
    }
    n = lifted->nraw > original->nraw ? lifted->nraw : original->nraw;
    for (size_t k = 0; k < n; k++) {
        const SemRaw *x = k < lifted->nraw ? &lifted->raw[k] : NULL;
        const SemRaw *y = k < original->nraw ? &original->raw[k] : NULL;
        if (x && y && raw_equal(x, y))
            continue;
        describe_raw(x, l, sizeof l);
        describe_raw(y, o, sizeof o);
        snprintf(why, cap, "port event %zu: lifted %s, original %s", k, l, o);
        return false;
    }
    return true;
}

/* ---- device backend entry points ---- */

void lift_semantic_event(Machine *m, SemKind kind, uint32_t a, uint32_t b)
{
    SemTrace *t = lift_verify_semantic_lifted(m);
    if (t)
        sem_trace_event(t, kind, a, b);
}

uint32_t lift_semantic_read(Machine *m, SemKind kind, uint32_t a, uint32_t live_value)
{
    SemTrace *t = lift_verify_semantic_lifted(m);
    if (!t)
        return live_value;
    uint32_t value;
    if (!lift_verify_semantic_original_read(m, kind, &value)) {
        lift_verify_semantic_missing(m, kind);
        value = live_value;
    }
    sem_trace_event(t, kind, a, value);
    return value;
}
