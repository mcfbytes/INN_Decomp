/* MPU-401 at 330h/331h: commands answered with FEh; UART mode ignores all but reset and passes data to
 * MIDI OUT. Intelligent mode (the card's sequencer) is not emulated: its commands are only acknowledged. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpu401.h"

enum {
    QUEUE_BYTES = 16,
    STATUS_UNUSED_BITS = 0x3F,
    SMF_TICKS_PER_QUARTER = 500,
    SMF_TEMPO_US = 500000, /* with 500 ticks per quarter: one tick per millisecond */
    NS_PER_TICK = 1000000,
};

typedef struct Mpu {
    bool uart;
    uint8_t queue[QUEUE_BYTES];
    int head, count;
    MidiSinkFn sink;
    void *sink_ctx;
    MidiFile *file; /* --midi-out, owned */
} Mpu;

static Mpu *mpu(Machine *m) { return m->mpu; }

static void queue_byte(Mpu *p, uint8_t v)
{
    if (p->count < QUEUE_BYTES)
        p->queue[(p->head + p->count++) % QUEUE_BYTES] = v;
}

static uint32_t mpu_in(Machine *m, uint16_t port, int size)
{
    Mpu *p = mpu(m);
    if (port == MPU_PORT_STATUS)
        return STATUS_UNUSED_BITS | (p->count ? 0 : MPU_STATUS_DSR); /* always ready to take a byte */
    if (!p->count)
        return MPU_ACK; /* an empty queue reads FEh, as DOSBox's MPU-401 does */
    uint8_t v = p->queue[p->head];
    p->head = (p->head + 1) % QUEUE_BYTES;
    p->count--;
    return v;
}

static void command(Mpu *p, uint8_t cmd)
{
    if (p->uart && cmd != MPU_CMD_RESET)
        return;
    if (cmd == MPU_CMD_RESET) {
        p->uart = false;
        p->count = 0;
    } else if (cmd == MPU_CMD_UART) {
        p->uart = true;
    }
    queue_byte(p, MPU_ACK);
}

static void mpu_out(Machine *m, uint16_t port, uint32_t value, int size)
{
    Mpu *p = mpu(m);
    if (port == MPU_PORT_STATUS)
        command(p, (uint8_t)value);
    else if (p->uart)
        mpu401_midi_byte(m, (uint8_t)value);
}

void mpu401_init(Machine *m)
{
    Mpu *p = calloc(1, sizeof *p);
    if (!p)
        fatal("out of memory");
    m->mpu = p;
    port_register(m, MPU_PORT_DATA, MPU_PORT_STATUS, mpu_in, mpu_out);
}

void mpu401_shutdown(Machine *m)
{
    Mpu *p = mpu(m);
    if (!p)
        return;
    if (p->file && midi_file_close(p->file) != 0)
        log_msg("mpu401: error writing the MIDI file");
    free(p);
    m->mpu = NULL;
}

void mpu401_set_sink(Machine *m, MidiSinkFn fn, void *ctx)
{
    mpu(m)->sink = fn;
    mpu(m)->sink_ctx = ctx;
}

int mpu401_open_midi_out(Machine *m, const char *path)
{
    Mpu *p = mpu(m);
    if (p->file)
        midi_file_close(p->file);
    p->file = midi_file_open(path);
    if (!p->file)
        return -1;
    mpu401_set_sink(m, midi_file_byte, p->file);
    return 0;
}

void mpu401_midi_byte(Machine *m, uint8_t byte)
{
    Mpu *p = mpu(m);
    if (p && p->sink)
        p->sink(p->sink_ctx, machine_now_ns(m), byte);
}

bool mpu401_uart_mode(Machine *m) { return mpu(m)->uart; }

/* ---- Standard MIDI File ---- */

struct MidiFile {
    FILE *f;
    long track_start; /* offset of the MTrk length */
    uint64_t last_tick;
    bool error;
    uint8_t running, msg[3]; /* running status; the message being collected */
    int have, need;
    uint8_t *sysex;
    size_t sysex_len, sysex_cap;
    bool in_sysex;
};

static void put(MidiFile *mf, const void *p, size_t n)
{
    if (n && fwrite(p, 1, n, mf->f) != n)
        mf->error = true;
}

static void put_varlen(MidiFile *mf, uint32_t v)
{
    uint8_t buf[5];
    int n = 0;
    buf[4 - n++] = v & 0x7F;
    while ((v >>= 7) && n < 5)
        buf[4 - n++] = (uint8_t)(0x80 | (v & 0x7F));
    put(mf, buf + 5 - n, (size_t)n);
}

static void put_be32(MidiFile *mf, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    put(mf, b, 4);
}

static void put_delta(MidiFile *mf, uint64_t ns)
{
    uint64_t tick = ns / NS_PER_TICK;
    if (tick < mf->last_tick)
        tick = mf->last_tick;
    uint64_t delta = tick - mf->last_tick;
    put_varlen(mf, delta > 0x0FFFFFFF ? 0x0FFFFFFF : (uint32_t)delta);
    mf->last_tick = tick;
}

/* Bytes that form no event of their own: an F7 escape carries them as they are. */
static void put_escape(MidiFile *mf, uint64_t ns, const uint8_t *b, size_t n)
{
    uint8_t esc = 0xF7;
    put_delta(mf, ns);
    put(mf, &esc, 1);
    put_varlen(mf, (uint32_t)n);
    put(mf, b, n);
}

static void flush_sysex(MidiFile *mf, uint64_t ns)
{
    uint8_t f0 = 0xF0;
    put_delta(mf, ns);
    put(mf, &f0, 1);
    put_varlen(mf, (uint32_t)mf->sysex_len);
    put(mf, mf->sysex, mf->sysex_len);
    mf->sysex_len = 0;
    mf->in_sysex = false;
}

static void sysex_add(MidiFile *mf, uint8_t b)
{
    if (mf->sysex_len == mf->sysex_cap) {
        size_t cap = mf->sysex_cap ? mf->sysex_cap * 2 : 256;
        uint8_t *p = realloc(mf->sysex, cap);
        if (!p)
            fatal("out of memory");
        mf->sysex = p;
        mf->sysex_cap = cap;
    }
    mf->sysex[mf->sysex_len++] = b;
}

/* Data bytes a status takes: channel messages and system common. */
static int data_bytes(uint8_t status)
{
    switch (status & 0xF0) {
    case 0xC0: case 0xD0: return 1;
    case 0xF0:
        return status == 0xF2 ? 2 : status == 0xF1 || status == 0xF3 ? 1 : 0;
    default: return 2;
    }
}

MidiFile *midi_file_open(const char *path)
{
    MidiFile *mf = calloc(1, sizeof *mf);
    if (!mf)
        fatal("out of memory");
    if (!(mf->f = fopen(path, "wb"))) {
        free(mf);
        return NULL;
    }
    static const uint8_t header[] = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1,
                                      SMF_TICKS_PER_QUARTER >> 8, SMF_TICKS_PER_QUARTER & 0xFF, 'M', 'T', 'r', 'k' };
    put(mf, header, sizeof header);
    mf->track_start = ftell(mf->f);
    put_be32(mf, 0);
    static const uint8_t tempo[] = { 0, 0xFF, 0x51, 3, SMF_TEMPO_US >> 16 & 0xFF, SMF_TEMPO_US >> 8 & 0xFF, SMF_TEMPO_US & 0xFF };
    put(mf, tempo, sizeof tempo);
    return mf;
}

void midi_file_byte(void *ctx, uint64_t ns, uint8_t b)
{
    MidiFile *mf = ctx;
    if (b >= 0xF8) { /* real-time: may fall inside any message */
        put_escape(mf, ns, &b, 1);
        return;
    }
    if (mf->in_sysex) {
        if (b < 0x80 || b == 0xF7) {
            sysex_add(mf, b);
            if (b == 0xF7)
                flush_sysex(mf, ns);
            return;
        }
        flush_sysex(mf, ns); /* cut short by a status byte */
    }
    if (b >= 0x80) {
        if (mf->have) /* an unfinished message */
            put_escape(mf, ns, mf->msg, (size_t)mf->have);
        mf->have = 0;
        mf->running = b < 0xF0 ? b : 0; /* system messages cancel running status */
        if (b == 0xF0) {
            mf->in_sysex = true;
            return;
        }
        mf->msg[mf->have++] = b;
        mf->need = 1 + data_bytes(b);
    } else if (!mf->have) {
        if (!mf->running) { /* a data byte with no status to belong to */
            put_escape(mf, ns, &b, 1);
            return;
        }
        mf->msg[mf->have++] = mf->running;
        mf->need = 1 + data_bytes(mf->running);
        mf->msg[mf->have++] = b;
    } else {
        mf->msg[mf->have++] = b;
    }
    if (mf->have < mf->need)
        return;
    if (mf->msg[0] >= 0xF0) {
        put_escape(mf, ns, mf->msg, (size_t)mf->have);
    } else {
        put_delta(mf, ns);
        put(mf, mf->msg, (size_t)mf->have);
    }
    mf->have = 0;
}

int midi_file_close(MidiFile *mf)
{
    if (!mf)
        return 0;
    if (mf->in_sysex)
        flush_sysex(mf, mf->last_tick * NS_PER_TICK);
    if (mf->have)
        put_escape(mf, mf->last_tick * NS_PER_TICK, mf->msg, (size_t)mf->have);
    static const uint8_t end[] = { 0, 0xFF, 0x2F, 0 };
    put(mf, end, sizeof end);
    long end_pos = ftell(mf->f);
    if (end_pos < 0 || fseek(mf->f, mf->track_start, SEEK_SET) != 0)
        mf->error = true;
    else
        put_be32(mf, (uint32_t)(end_pos - mf->track_start - 4));
    bool error = mf->error;
    if (fclose(mf->f) != 0)
        error = true;
    free(mf->sysex);
    free(mf);
    return error ? -1 : 0;
}
