/* Sound Blaster 2.0 DSP at 220h (reset 226h, data 22Ah, command 22Ch, status and IRQ ack 22Eh) with
 * 8-bit DMA on channel 1 and IRQ 7. Bytes are fetched on the DSP's own clock: docs/runtime.md, Sound. */
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "devices.h"

enum {
    PORT_RESET = 0x6, PORT_READ = 0xA, PORT_WRITE = 0xC, PORT_STATUS = 0xE,
    RESET_ACK = 0xAA,
    STATUS_READY = 0x7F, STATUS_DATA = 0x80, /* bit 7: write busy (22Ch) or data waiting (22Eh) */
    OUT_QUEUE = 16,
    SILENCE = 0x80,
    SAMPLE_SHIFT = 8,
    NS_PER_US = 1000,
    IRQ_TEST_DELAY_NS = 30000, /* F2h raises the IRQ about 30 us after the command */
    DEFAULT_TIME_CONSTANT = 0xD2, /* 22.7 kHz, a common default */
    IRQ_TEST_NS = 10 * NS_PER_US, /* F2h: the DSP raises the IRQ a few us after the command, not at once */
};

enum {
    CMD_DIRECT_DAC = 0x10, CMD_DMA_SINGLE = 0x14, CMD_DMA_AUTO = 0x1C, CMD_DIRECT_ADC = 0x20,
    CMD_TIME_CONSTANT = 0x40, CMD_BLOCK_SIZE = 0x48,
    CMD_HS_AUTO = 0x90, CMD_HS_SINGLE = 0x91,
    CMD_PAUSE = 0xD0, CMD_SPEAKER_ON = 0xD1, CMD_SPEAKER_OFF = 0xD3, CMD_CONTINUE = 0xD4,
    CMD_SPEAKER_STATUS = 0xD8, CMD_EXIT_AUTO = 0xDA,
    CMD_IDENTIFY = 0xE0, CMD_VERSION = 0xE1, CMD_WRITE_TEST = 0xE4, CMD_READ_TEST = 0xE8,
    CMD_IRQ_TEST = 0xF2,
};

typedef struct Sb {
    uint8_t cmd, args[2];
    int args_needed, args_got;
    uint8_t out[OUT_QUEUE];
    int out_head, out_count;
    uint8_t out_last, test_reg;
    bool resetting, speaker, irq_line;
    bool irq_test_due;   /* F2h answers a little later, as the chip does */
    uint64_t irq_test_ns;

    uint64_t period_ns; /* (256 - time constant) us */
    uint32_t block;     /* bytes, from 48h or the last 14h */
    bool active, auto_init, exit_auto, paused;
    uint32_t left;      /* bytes still to fetch in this block */
    uint64_t next_ns;   /* when the next byte is fetched */
    uint64_t pause_left_ns;
    uint8_t prev, cur;  /* the output ramps from prev to cur over cur_period from cur_ns */
    uint64_t cur_ns, cur_period;
    double prev_in, prev_out; /* output coupling filter */
} Sb;

static const double DC_BLOCK_POLE = 0.999; /* the line output is AC-coupled, so a held DAC level fades */

static Sb *sb_state(Machine *m) { return m->sb; }

static int args_for(uint8_t cmd)
{
    switch (cmd) {
    case CMD_DIRECT_DAC: case CMD_TIME_CONSTANT: case CMD_IDENTIFY: case CMD_WRITE_TEST:
    case 0x38: /* MIDI write */
        return 1;
    case CMD_DMA_SINGLE: case CMD_BLOCK_SIZE:
    case 0x16: case 0x17: case 0x24: case 0x74: case 0x75: case 0x76: case 0x77: case 0x80:
        return 2; /* ADPCM, recording and silence blocks: parsed so the stream stays in step, not played */
    default:
        return 0;
    }
}

static void push_out(Sb *s, uint8_t v)
{
    if (s->out_count == OUT_QUEUE)
        return;
    s->out[(s->out_head + s->out_count++) % OUT_QUEUE] = v;
}

static uint8_t pop_out(Sb *s)
{
    if (s->out_count) {
        s->out_last = s->out[s->out_head];
        s->out_head = (s->out_head + 1) % OUT_QUEUE;
        s->out_count--;
    }
    return s->out_last;
}

static void raise_irq(Machine *m, Sb *s)
{
    s->irq_line = true;
    pic_raise_irq(m, PIC_IRQ_SB);
}

static void set_sample(Sb *s, uint8_t v, uint64_t at, uint64_t ramp)
{
    s->prev = s->cur;
    s->cur = v;
    s->cur_ns = at;
    s->cur_period = ramp;
}

static void block_done(Machine *m, Sb *s)
{
    raise_irq(m, s);
    if (s->auto_init && !s->exit_auto)
        s->left = s->block;
    else
        s->active = false;
}

/* Fetches every byte due by t. A masked channel holds the DSP's request, so the block waits. */
static void advance(Machine *m, Sb *s, uint64_t t)
{
    while (s->active && !s->paused && s->next_ns <= t) {
        uint8_t b;
        if (dma_read(m, SB_DMA_CHANNEL, &b, 1, NULL) == 0) {
            s->next_ns += ((t - s->next_ns) / s->period_ns + 1) * s->period_ns;
            break;
        }
        set_sample(s, b, s->next_ns, s->period_ns);
        s->next_ns += s->period_ns;
        if (--s->left == 0)
            block_done(m, s);
    }
}

static void irq_test_due(Machine *m, Sb *s, uint64_t t)
{
    if (s->irq_test_ns && s->irq_test_ns <= t) {
        s->irq_test_ns = 0;
        s->irq_test_due = false;
        raise_irq(m, s);
    }
}

/* Renders what was due under the old state, then moves the DSP up to now. */
static void catch_up(Machine *m)
{
    audio_sync(m);
    advance(m, sb_state(m), machine_now_ns(m));
    irq_test_due(m, sb_state(m), machine_now_ns(m));
}

static double level(const Sb *s, uint64_t t)
{
    if (!s->speaker)
        return 0;
    double v = t < s->cur_ns ? s->prev : s->cur;
    if (t >= s->cur_ns && t < s->cur_ns + s->cur_period)
        v = s->prev + (double)(s->cur - s->prev) * (double)(t - s->cur_ns) / (double)s->cur_period;
    return (v - SILENCE) * (1 << SAMPLE_SHIFT);
}

static void sb_render(Machine *m, int32_t *mix, int frames)
{
    Sb *s = sb_state(m);
    uint64_t t0 = audio_frame_ns(m, 0);
    if (!s->active && t0 >= s->cur_ns + s->cur_period && level(s, t0) == s->prev_in && s->prev_out > -1 &&
        s->prev_out < 1)
        return; /* idle and settled */
    for (int i = 0; i < frames; i++) {
        uint64_t t = audio_frame_ns(m, i);
        advance(m, s, t);
        double in = level(s, t);
        s->prev_out = in - s->prev_in + DC_BLOCK_POLE * s->prev_out;
        s->prev_in = in;
        int32_t v = (int32_t)s->prev_out;
        mix[i * AUDIO_CHANNELS] += v;
        mix[i * AUDIO_CHANNELS + 1] += v;
    }
}

static void start_dma(Machine *m, Sb *s, bool auto_init)
{
    s->active = true;
    s->auto_init = auto_init;
    s->exit_auto = s->paused = false;
    s->left = s->block;
    s->next_ns = machine_now_ns(m) + s->period_ns;
}

static void execute(Machine *m, Sb *s)
{
    uint64_t now = machine_now_ns(m);
    uint32_t len = (uint32_t)(s->args[0] | s->args[1] << 8) + 1;
    switch (s->cmd) {
    case CMD_DIRECT_DAC: set_sample(s, s->args[0], now, 0); break;
    case CMD_DMA_SINGLE:
        s->block = len;
        start_dma(m, s, false);
        break;
    case CMD_DMA_AUTO: case CMD_HS_AUTO: start_dma(m, s, true); break;
    case CMD_HS_SINGLE: start_dma(m, s, false); break;
    case CMD_DIRECT_ADC: push_out(s, SILENCE); break;
    case CMD_TIME_CONSTANT: s->period_ns = (uint64_t)(256 - s->args[0]) * NS_PER_US; break;
    case CMD_BLOCK_SIZE: s->block = len; break;
    case CMD_PAUSE:
        if (s->active && !s->paused) {
            s->paused = true;
            s->pause_left_ns = s->next_ns > now ? s->next_ns - now : 0;
        }
        break;
    case CMD_CONTINUE:
        if (s->paused) {
            s->paused = false;
            s->next_ns = now + s->pause_left_ns;
        }
        break;
    case CMD_SPEAKER_ON: s->speaker = true; break;
    case CMD_SPEAKER_OFF: s->speaker = false; break;
    case CMD_SPEAKER_STATUS: push_out(s, s->speaker ? 0xFF : 0x00); break;
    case CMD_EXIT_AUTO: s->exit_auto = true; break;
    case CMD_IDENTIFY: push_out(s, (uint8_t)~s->args[0]); break;
    case CMD_VERSION:
        push_out(s, SB_DSP_VERSION >> 8);
        push_out(s, SB_DSP_VERSION & 0xFF);
        break;
    case CMD_WRITE_TEST: s->test_reg = s->args[0]; break;
    case CMD_READ_TEST: push_out(s, s->test_reg); break;
    case CMD_IRQ_TEST:
        s->irq_test_due = true;
        s->irq_test_ns = machine_now_ns(m) + IRQ_TEST_NS;
        break;
    default: break;
    }
}

static void write_byte(Machine *m, Sb *s, uint8_t v)
{
    if (s->args_needed > s->args_got) {
        s->args[s->args_got++] = v;
    } else {
        s->cmd = v;
        s->args_needed = args_for(v);
        s->args_got = 0;
    }
    if (s->args_got == s->args_needed) {
        execute(m, s);
        s->args_needed = s->args_got = 0;
    }
}

static void ack_irq(Machine *m, Sb *s)
{
    if (!s->irq_line)
        return;
    s->irq_line = false;
    pic_lower_irq(m, PIC_IRQ_SB);
}

/* Stops playback, mutes the speaker and drops the output to silence; the time constant stays. */
static void reset(Machine *m, Sb *s)
{
    ack_irq(m, s);
    *s = (Sb){ .period_ns = s->period_ns, .block = 1, .prev = SILENCE, .cur = SILENCE,
               .prev_in = s->prev_in, .prev_out = s->prev_out };
}

static uint32_t sb_in(Machine *m, uint16_t port, int size)
{
    Sb *s = sb_state(m);
    catch_up(m);
    switch (port - SB_BASE) {
    case PORT_READ: return pop_out(s);
    case PORT_WRITE: return STATUS_READY;
    case PORT_STATUS:
        ack_irq(m, s);
        return s->out_count ? STATUS_DATA | STATUS_READY : STATUS_READY;
    default: return 0xFF;
    }
}

static void sb_out(Machine *m, uint16_t port, uint32_t value, int size)
{
    Sb *s = sb_state(m);
    catch_up(m);
    uint8_t v = (uint8_t)value;
    if (port - SB_BASE == PORT_RESET) {
        if (v & 1) {
            reset(m, s);
            s->resetting = true;
        } else if (s->resetting) {
            s->resetting = false;
            push_out(s, RESET_ACK);
        }
    } else if (port - SB_BASE == PORT_WRITE && !s->resetting) {
        write_byte(m, s, v);
    }
}

/* Block ends raise the IRQ on time even with no sink rendering. */
static void sb_tick(Machine *m)
{
    Sb *s = sb_state(m);
    if (s->irq_test_due && machine_now_ns(m) >= s->irq_test_ns) {
        s->irq_test_due = false;
        raise_irq(m, s);
    }
    if (s->active && !s->paused && s->next_ns + (s->left - 1) * s->period_ns <= machine_now_ns(m))
        catch_up(m);
    irq_test_due(m, s, machine_now_ns(m));
}

static void sb_dma_sync(Machine *m)
{
    if (sb_state(m)->active)
        catch_up(m);
}

/* ---- mixed-mode hand-off and snapshots ---- */

void sb_voice_get(Machine *m, PcmVoice *v)
{
    const Sb *s = sb_state(m);
    *v = (PcmVoice){ .active = s->active, .auto_init = s->auto_init && !s->exit_auto, .paused = s->paused,
                     .speaker = s->speaker, .block = s->block, .left = s->left, .period_ns = s->period_ns,
                     .next_ns = s->next_ns, .pause_left_ns = s->pause_left_ns, .prev = s->prev, .cur = s->cur,
                     .cur_ns = s->cur_ns, .cur_period = s->cur_period, .prev_in = s->prev_in,
                     .prev_out = s->prev_out };
}

void sb_voice_take(Machine *m, PcmVoice *v)
{
    Sb *s = sb_state(m);
    catch_up(m);
    sb_voice_get(m, v);
    ack_irq(m, s);
    s->active = s->paused = s->exit_auto = false;
    s->prev = s->cur = SILENCE;
    s->cur_period = 0;
    s->prev_in = s->prev_out = 0;
}

void sb_voice_set(Machine *m, const PcmVoice *v)
{
    Sb *s = sb_state(m);
    audio_sync(m);
    s->active = v->active;
    s->auto_init = v->auto_init;
    s->exit_auto = false;
    s->paused = v->paused;
    s->speaker = v->speaker;
    s->block = v->block;
    s->left = v->left;
    s->period_ns = v->period_ns;
    s->next_ns = v->next_ns;
    s->pause_left_ns = v->pause_left_ns;
    s->prev = v->prev;
    s->cur = v->cur;
    s->cur_ns = v->cur_ns;
    s->cur_period = v->cur_period;
    s->prev_in = v->prev_in;
    s->prev_out = v->prev_out;
}

uint8_t sb_time_constant(Machine *m)
{
    uint64_t us = sb_state(m)->period_ns / NS_PER_US;
    return (uint8_t)(us >= 1 && us <= 256 ? 256 - us : DEFAULT_TIME_CONSTANT);
}

size_t sb_snapshot_size(void) { return sizeof(Sb); }

/* A machine without the device (unit tests) saves zeros and loads nothing. */
void sb_snapshot_save(Machine *m, void *dst)
{
    if (sb_state(m))
        memcpy(dst, sb_state(m), sizeof(Sb));
    else
        memset(dst, 0, sizeof(Sb));
}

void sb_snapshot_load(Machine *m, const void *src)
{
    if (sb_state(m))
        memcpy(sb_state(m), src, sizeof(Sb));
}

void sb_init(Machine *m)
{
    Sb *s = calloc(1, sizeof *s);
    if (!s)
        fatal("out of memory");
    m->sb = s;
    s->period_ns = (uint64_t)(256 - DEFAULT_TIME_CONSTANT) * NS_PER_US;
    reset(m, s);
    port_register(m, SB_BASE + PORT_RESET, SB_BASE + PORT_RESET, sb_in, sb_out);
    port_register(m, SB_BASE + PORT_READ, SB_BASE + PORT_READ, sb_in, sb_out);
    port_register(m, SB_BASE + PORT_WRITE, SB_BASE + PORT_WRITE, sb_in, sb_out);
    port_register(m, SB_BASE + PORT_STATUS, SB_BASE + PORT_STATUS, sb_in, sb_out);
    dma_set_sync(m, SB_DMA_CHANNEL, sb_dma_sync);
    audio_name_source(m, audio_add_source(m, sb_render), "sb");
    machine_add_ticker(m, sb_tick);
}

void sb_shutdown(Machine *m)
{
    free(m->sb);
    m->sb = NULL;
}
