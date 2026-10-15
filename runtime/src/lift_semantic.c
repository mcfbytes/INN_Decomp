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
};

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

void sem_trace_port(SemTrace *t, uint16_t port, int size, uint32_t value, bool is_out)
{
    if (opl_port(t, port, size, value, is_out) || dac_port(t, port, size, value, is_out) ||
        crtc_port(t, port, size, value, is_out) || pit_port(t, port, size, value, is_out) ||
        pic_port(t, port, size, value, is_out) || kbd_port(t, port, size, value, is_out))
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
};

static bool is_read_kind(SemKind k) { return k == SEM_DAC_GET || k == SEM_KBD_DATA; }

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
