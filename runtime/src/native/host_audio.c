/* Host audio: OPL register access and Sound Blaster digital output at the native clock, or the devices
 * in emulated mode (docs/native.md section 9). */
#include "audio.h"
#include "devices.h"
#include "host_audio.h"
#include "lift_semantic.h"
#include "native.h"

void host_audio_opl_write(Machine *m, uint8_t reg, uint8_t value)
{
    if (m->native) {
        native_safepoint(m);
        opl_write_reg(m, reg, value);
        return;
    }
    /* Shadow replay: the original's writes already reached the chip. Record the meaning only. */
    if (m->io_mode == IO_REPLAY) {
        lift_semantic_event(m, SEM_OPL_WRITE, reg, value);
        return;
    }
    opl_write_reg(m, reg, value);
}

uint8_t host_audio_opl_status(Machine *m)
{
    if (m->native)
        native_safepoint(m);
    return opl_read_status(m); /* delay reads are dropped from the semantic comparison */
}

/* ---- Sound Blaster: the port sequences of emulated mode ----
 * In a shadow run's lifted half (IO_REPLAY) these ports do not reach the devices: lift_verify.c
 * canonicalises them into the same semantic events as the original's port log, and the reads return
 * the original's values. The DSP is always ready on the emulated card, so no write polls 22Ch. */

enum {
    SB_PORT_RESET = SB_BASE + 0x6, SB_PORT_READ = SB_BASE + 0xA, SB_PORT_WRITE = SB_BASE + 0xC,
    SB_PORT_STATUS = SB_BASE + 0xE,
    DSP_DMA_SINGLE = 0x14, DSP_DMA_AUTO = 0x1C, DSP_TIME_CONSTANT = 0x40, DSP_BLOCK_SIZE = 0x48,
    DSP_HALT = 0xD0, DSP_SPEAKER_ON = 0xD1, DSP_SPEAKER_OFF = 0xD3, DSP_IDENTIFY = 0xE0, DSP_VERSION = 0xE1,
    DMA_PORT_ADDRESS1 = 0x02, DMA_PORT_COUNT1 = 0x03, DMA_PORT_MASK = 0x0A, DMA_PORT_MODE = 0x0B,
    DMA_PORT_FLIP_FLOP = 0x0C, DMA_PORT_PAGE1 = 0x83,
    DMA_SET_MASK1 = 0x04 | SB_DMA_CHANNEL, DMA_CLEAR_MASK1 = SB_DMA_CHANNEL,
    DMA_MODE_READ = 0x48, DMA_MODE_AUTO = 0x10, DMA_MODE_DECREMENT = 0x20, DMA_MODE_TYPE = 0x0C,
    DMA_TYPE_READ = 0x08,
    US_PER_S = 1000000, NS_PER_US = 1000, MAX_PERIOD_US = 256,
};

static void dsp_out(Machine *m, uint8_t v) { port_out(m, SB_PORT_WRITE, v, 1); }
static uint8_t dsp_in(Machine *m) { return (uint8_t)port_in(m, SB_PORT_READ, 1); }
static void dma_out(Machine *m, uint16_t port, uint8_t v) { port_out(m, port, v, 1); }

uint32_t host_audio_sb_rate(uint8_t tc)
{
    return US_PER_S / (uint32_t)(256 - tc);
}

/* The time constant whose period is nearest 1 / rate_hz; false when none is. */
static bool time_constant_for(uint32_t rate_hz, uint8_t *tc)
{
    uint32_t us = (US_PER_S + rate_hz / 2) / rate_hz;
    if (us < 1 || us > MAX_PERIOD_US)
        return false;
    *tc = (uint8_t)(256 - us);
    return true;
}

static uint64_t period_ns_of(uint8_t tc) { return (uint64_t)(256 - tc) * NS_PER_US; }

/* ---- native: the host PCM ---- */

static void native_time_constant(Machine *m, uint8_t tc)
{
    AudioPcm *p = audio_pcm(m);
    audio_pcm_sync(m);
    p->time_constant = tc;
    p->v.period_ns = period_ns_of(tc);
}

static void native_pcm_start(Machine *m, uint32_t linear, uint32_t len, bool auto_init)
{
    AudioPcm *p = audio_pcm(m);
    audio_pcm_sync(m);
    PcmVoice *v = &p->v;
    p->dma_base = linear & 0xFFFFFF;
    p->dma_len = len;
    p->dma_pos = 0;
    p->dma_auto = auto_init;
    p->dma_held = false;
    v->active = true;
    v->auto_init = auto_init;
    v->paused = false;
    v->block = v->left = len;
    v->next_ns = machine_now_ns(m) + v->period_ns;
}

/* The DSP halt and the mask: nothing more is fetched and no block ends; the output holds its last
 * byte, which the AC coupling fades. */
static void native_pcm_halt(Machine *m)
{
    audio_pcm_sync(m);
    audio_pcm(m)->v.active = false;
}

int host_audio_pcm_start(Machine *m, uint32_t linear, uint16_t bytes, uint32_t rate_hz, bool auto_init)
{
    uint8_t tc = 0;
    if (rate_hz && !time_constant_for(rate_hz, &tc))
        return -1;
    uint32_t len = bytes ? bytes : 0x10000u, count = len - 1;
    if (m->native) {
        native_safepoint(m);
        if (rate_hz)
            native_time_constant(m, tc);
        native_pcm_start(m, linear, len, auto_init);
        return 0;
    }
    if (rate_hz) {
        dsp_out(m, DSP_TIME_CONSTANT);
        dsp_out(m, tc);
    }
    dma_out(m, DMA_PORT_MASK, DMA_SET_MASK1);
    dma_out(m, DMA_PORT_FLIP_FLOP, 0);
    dma_out(m, DMA_PORT_MODE, (uint8_t)(DMA_MODE_READ | SB_DMA_CHANNEL | (auto_init ? DMA_MODE_AUTO : 0)));
    dma_out(m, DMA_PORT_ADDRESS1, (uint8_t)linear);
    dma_out(m, DMA_PORT_ADDRESS1, (uint8_t)(linear >> 8));
    dma_out(m, DMA_PORT_COUNT1, (uint8_t)count);
    dma_out(m, DMA_PORT_COUNT1, (uint8_t)(count >> 8));
    dma_out(m, DMA_PORT_PAGE1, (uint8_t)(linear >> 16));
    dma_out(m, DMA_PORT_MASK, DMA_CLEAR_MASK1);
    if (auto_init) {
        dsp_out(m, DSP_BLOCK_SIZE);
        dsp_out(m, (uint8_t)count);
        dsp_out(m, (uint8_t)(count >> 8));
        dsp_out(m, DSP_DMA_AUTO);
    } else {
        dsp_out(m, DSP_DMA_SINGLE);
        dsp_out(m, (uint8_t)count);
        dsp_out(m, (uint8_t)(count >> 8));
    }
    return 0;
}

void host_audio_pcm_stop(Machine *m)
{
    if (m->native) {
        native_safepoint(m);
        native_pcm_halt(m);
        return;
    }
    dsp_out(m, DSP_HALT);
    dma_out(m, DMA_PORT_MASK, DMA_SET_MASK1);
}

void host_audio_pcm_mask(Machine *m)
{
    if (m->native) {
        native_safepoint(m);
        audio_pcm_sync(m);
        audio_pcm(m)->dma_held = true;
        return;
    }
    dma_out(m, DMA_PORT_MASK, DMA_SET_MASK1);
}

uint8_t host_audio_sb_reset(Machine *m)
{
    if (m->native) {
        native_safepoint(m);
        AudioPcm *p = audio_pcm(m);
        audio_pcm_sync(m);
        /* As sb.c's reset: playback stops, the speaker mutes, the output drops to silence. */
        uint64_t now = machine_now_ns(m);
        PcmVoice *v = &p->v;
        v->active = v->paused = v->speaker = false;
        v->prev = v->cur = PCM_SILENCE;
        v->cur_ns = now;
        v->cur_period = 0;
        p->dma_len = 0;
        p->irq = false;
        return HOST_AUDIO_SB_RESET_ACK;
    }
    port_out(m, SB_PORT_RESET, 1, 1);
    port_out(m, SB_PORT_RESET, 0, 1);
    return dsp_in(m);
}

uint8_t host_audio_sb_identify(Machine *m, uint8_t v)
{
    if (m->native) {
        native_safepoint(m);
        return (uint8_t)~v;
    }
    dsp_out(m, DSP_IDENTIFY);
    dsp_out(m, v);
    return dsp_in(m);
}

uint16_t host_audio_sb_version(Machine *m)
{
    if (m->native) {
        native_safepoint(m);
        return SB_DSP_VERSION;
    }
    dsp_out(m, DSP_VERSION);
    uint8_t major = dsp_in(m);
    return (uint16_t)(major << 8 | dsp_in(m));
}

void host_audio_sb_speaker(Machine *m, bool on)
{
    if (m->native) {
        native_safepoint(m);
        audio_pcm_sync(m);
        audio_pcm(m)->v.speaker = on;
        return;
    }
    dsp_out(m, on ? DSP_SPEAKER_ON : DSP_SPEAKER_OFF);
}

void host_audio_sb_time_constant(Machine *m, uint8_t tc)
{
    if (m->native) {
        native_safepoint(m);
        native_time_constant(m, tc);
        return;
    }
    dsp_out(m, DSP_TIME_CONSTANT);
    dsp_out(m, tc);
}

uint8_t host_audio_sb_ack(Machine *m)
{
    if (m->native) {
        native_safepoint(m);
        return HOST_AUDIO_SB_STATUS_READY;
    }
    return (uint8_t)port_in(m, SB_PORT_STATUS, 1);
}

/* ---- native core ---- */

static void voice_idle(PcmVoice *v)
{
    v->active = v->paused = false;
    v->prev = v->cur = PCM_SILENCE;
    v->cur_period = 0;
    v->prev_in = v->prev_out = 0;
}

/* A machine without the sound devices (unit tests of the native core) has nothing to play or hand over. */
static bool have_devices(const Machine *m) { return m->audio && m->sb && m->dma; }

void host_audio_poll(Machine *m)
{
    if (!m->audio)
        return;
    AudioPcm *p = audio_pcm(m);
    if (p->v.active && audio_pcm_block_end_ns(m) <= machine_now_ns(m))
        audio_pcm_sync(m);
    if (p->irq) {
        p->irq = false;
        native_irq_raise(m, HOST_AUDIO_SB_IRQ);
    }
}

uint64_t host_audio_next_due_us(Machine *m)
{
    if (!m->audio)
        return UINT64_MAX;
    if (audio_pcm(m)->irq)
        return m->virtual_us;
    uint64_t end = audio_pcm_block_end_ns(m);
    return end == UINT64_MAX ? end : (end + NS_PER_US - 1) / NS_PER_US;
}

/* The host block becomes channel 1's registers and the DSP's voice: what is left of the buffer and of
 * the block, the time of the next byte, the speaker and the output filter. A block end the native IRQ
 * model has not raised yet is raised there, so it goes to the PIC with the other requests. The host
 * source is left idle and silent. */
void host_audio_to_devices(Machine *m)
{
    if (!have_devices(m))
        return;
    AudioPcm *p = audio_pcm(m);
    audio_pcm_sync(m);
    if (p->irq) {
        p->irq = false;
        native_irq_raise(m, HOST_AUDIO_SB_IRQ);
    }
    if (p->dma_len) {
        uint32_t base = p->dma_base;
        uint16_t count = (uint16_t)(p->dma_len - 1);
        DmaChannelState c = {
            .base_addr = (uint16_t)base, .base_count = count,
            .addr = (uint16_t)(base + p->dma_pos), .count = (uint16_t)(count - p->dma_pos),
            .page = (uint8_t)(base >> 16),
            .mode = (uint8_t)(DMA_MODE_READ | SB_DMA_CHANNEL | (p->dma_auto ? DMA_MODE_AUTO : 0)),
            .masked = p->dma_held,
        };
        if (p->dma_held) { /* terminal count: the address went past the end, the count wrapped */
            c.addr = (uint16_t)(base + p->dma_len);
            c.count = 0xFFFF;
        }
        dma_channel_set(m, SB_DMA_CHANNEL, &c);
    }
    sb_voice_set(m, &p->v);
    voice_idle(&p->v);
    p->dma_len = p->dma_pos = 0;
    p->dma_held = p->dma_auto = false;
}

/* The reverse: the DSP's voice and channel 1's buffer position come over, the DSP is left idle with
 * its IRQ line low and the channel masked. A block end the game had not taken is dropped, as the
 * PIC's requests are. A channel programmed to count down cannot be played from here and stops. */
void host_audio_from_devices(Machine *m)
{
    if (!have_devices(m))
        return;
    AudioPcm *p = audio_pcm(m);
    PcmVoice v;
    sb_voice_take(m, &v);
    DmaChannelState c;
    dma_channel_get(m, SB_DMA_CHANNEL, &c);
    p->v = v;
    p->time_constant = sb_time_constant(m);
    p->irq = false;
    p->dma_len = p->dma_pos = 0;
    p->dma_auto = p->dma_held = false;
    if (v.active) {
        if ((c.mode & DMA_MODE_DECREMENT) || (c.mode & DMA_MODE_TYPE) != DMA_TYPE_READ) {
            log_msg("native: DMA channel 1 left in mode %02X by the game; its block stops", c.mode);
            p->v.active = false;
        } else {
            p->dma_base = (uint32_t)c.page << 16 | c.base_addr;
            p->dma_len = (uint32_t)c.base_count + 1;
            p->dma_pos = (uint16_t)(c.base_count - c.count);
            if (p->dma_pos >= p->dma_len)
                p->dma_pos = 0;
            p->dma_auto = c.mode & DMA_MODE_AUTO;
            p->dma_held = c.masked;
        }
    }
    c.masked = true;
    dma_channel_set(m, SB_DMA_CHANNEL, &c);
}
