/* Audio mixer. Output frame n is emulated time base_ns + n / rate, so sound follows the emulated
 * clock; sources render lazily, before each change to their sound (audio_sync) and on reads. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "devices.h"

enum {
    CHUNK_FRAMES = 256,
    TICK_SYNC_DIVISOR = 200, /* the ticker renders once 1/200 s (5 ms) is pending */
    NS_PER_S = 1000000000,
};

static const double ERROR_SMOOTHING = 0.05; /* per paced read, about 60 per second */
static const double STRETCH_GAIN = 0.01;    /* relative stretch per unit of queue error */
static const double RESYNC_ERROR = 0.5;     /* a sustained excess the stretch would take seconds to drain */

typedef struct TapSource {
    const char *name;
    uint64_t frames, sq_sum; /* this second: frames, and the sum of squared samples (both channels) */
    int32_t peak;            /* this second */
    uint64_t total_frames;
    int32_t max_peak;
} TapSource;

typedef struct Audio {
    bool tap;
    uint64_t tap_next; /* frames_done at which the next tap line is due */
    TapSource tapped[AUDIO_MAX_SOURCES];
    int rate;
    AudioStats stats;
    int target_ms; /* 0: AUDIO_TARGET_MS */
    bool enabled;
    AudioRenderFn sources[AUDIO_MAX_SOURCES];
    int source_count;
    uint64_t base_ns;     /* emulated time of frame 0 */
    uint64_t frames_done; /* frames rendered (or skipped while disabled) since base_ns */
    int16_t ring[AUDIO_RING_FRAMES * AUDIO_CHANNELS];
    int read, count;
    int16_t last[AUDIO_CHANNELS]; /* final frame of the previous paced read */
    double phase;                 /* paced resampler position, in [-1, 0) input frames */
    double error_avg;
    AudioPcm pcm;
} Audio;

static Audio *audio_state(Machine *m) { return m->audio; }

static uint64_t frames_due(const Audio *a, uint64_t now_ns)
{
    uint64_t dt = now_ns > a->base_ns ? now_ns - a->base_ns : 0;
    return dt / NS_PER_S * (uint64_t)a->rate + dt % NS_PER_S * (uint64_t)a->rate / NS_PER_S;
}

static void ring_push(Audio *a, const int32_t *mix, int frames)
{
    for (int i = 0; i < frames; i++) {
        if (a->count == AUDIO_RING_FRAMES) {
            a->read = (a->read + 1) % AUDIO_RING_FRAMES;
            a->count--;
        }
        int at = (a->read + a->count++) % AUDIO_RING_FRAMES;
        for (int c = 0; c < AUDIO_CHANNELS; c++) {
            int32_t v = mix[i * AUDIO_CHANNELS + c];
            a->ring[at * AUDIO_CHANNELS + c] = (int16_t)(v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v);
        }
    }
}

static void ring_drop(Audio *a, int frames)
{
    frames = frames < a->count ? frames : a->count;
    a->read = (a->read + frames) % AUDIO_RING_FRAMES;
    a->count -= frames;
}

static const int16_t *ring_at(const Audio *a, int i)
{
    return &a->ring[(a->read + i) % AUDIO_RING_FRAMES * AUDIO_CHANNELS];
}

static void tap_account(TapSource *t, const int32_t *before, const int32_t *after, int n)
{
    for (int i = 0; i < n * AUDIO_CHANNELS; i++) {
        int32_t v = after[i] - before[i], av = v < 0 ? -v : v;
        if (av > t->peak)
            t->peak = av;
        t->sq_sum += (uint64_t)((int64_t)v * v);
    }
    t->frames += (uint64_t)n;
}

static void tap_report(Audio *a)
{
    char line[256];
    int len = snprintf(line, sizeof line, "audio: tap t=%llu", (unsigned long long)(a->frames_done / (uint64_t)a->rate));
    for (int s = 0; s < a->source_count; s++) {
        TapSource *t = &a->tapped[s];
        double rms = t->frames ? sqrt((double)t->sq_sum / (double)(t->frames * AUDIO_CHANNELS)) : 0.0;
        t->total_frames += t->frames;
        if (t->peak > t->max_peak)
            t->max_peak = t->peak;
        if (len < (int)sizeof line)
            len += snprintf(line + len, sizeof line - (size_t)len, " %s frames=%llu peak=%d rms=%.0f total=%llu max=%d",
                            t->name ? t->name : "src", (unsigned long long)t->frames, t->peak, rms,
                            (unsigned long long)t->total_frames, t->max_peak);
        t->frames = t->sq_sum = 0;
        t->peak = 0;
    }
    log_msg("%s", line);
}

void audio_tap_enable(Machine *m, bool on)
{
    Audio *a = audio_state(m);
    a->tap = on;
    a->tap_next = a->frames_done + (uint64_t)a->rate;
}

void audio_name_source(Machine *m, int source, const char *name)
{
    if (source >= 0 && source < AUDIO_MAX_SOURCES)
        audio_state(m)->tapped[source].name = name;
}

int32_t audio_tap_peak(Machine *m, const char *name)
{
    const Audio *a = audio_state(m);
    for (int s = 0; s < a->source_count; s++) {
        const TapSource *t = &a->tapped[s];
        if (t->name && !strcmp(t->name, name))
            return t->peak > t->max_peak ? t->peak : t->max_peak;
    }
    return 0;
}

static void render(Machine *m, Audio *a, uint64_t frames)
{
    int32_t mix[CHUNK_FRAMES * AUDIO_CHANNELS];
    while (frames > 0) {
        int n = frames < CHUNK_FRAMES ? (int)frames : CHUNK_FRAMES;
        memset(mix, 0, sizeof mix);
        if (a->tap) {
            int32_t before[CHUNK_FRAMES * AUDIO_CHANNELS];
            for (int s = 0; s < a->source_count; s++) {
                memcpy(before, mix, sizeof(int32_t) * AUDIO_CHANNELS * (size_t)n);
                a->sources[s](m, mix, n);
                tap_account(&a->tapped[s], before, mix, n);
            }
        } else {
            for (int s = 0; s < a->source_count; s++)
                a->sources[s](m, mix, n);
        }
        ring_push(a, mix, n);
        a->frames_done += (uint64_t)n;
        frames -= (uint64_t)n;
        if (a->tap && a->frames_done >= a->tap_next) {
            tap_report(a);
            a->tap_next += (uint64_t)a->rate;
        }
    }
}

uint64_t audio_frame_ns(Machine *m, int i)
{
    const Audio *a = audio_state(m);
    uint64_t f = a->frames_done + (uint64_t)i, rate = (uint64_t)a->rate;
    return a->base_ns + f / rate * NS_PER_S + f % rate * NS_PER_S / rate;
}

void audio_sync(Machine *m)
{
    Audio *a = audio_state(m);
    if (!a)
        return;
    uint64_t due = frames_due(a, machine_now_ns(m));
    if (due <= a->frames_done)
        return;
    if (a->enabled)
        render(m, a, due - a->frames_done);
    else
        a->frames_done = due;
}

static void audio_tick(Machine *m)
{
    Audio *a = audio_state(m);
    if (a->enabled && frames_due(a, machine_now_ns(m)) - a->frames_done >= (uint64_t)(a->rate / TICK_SYNC_DIVISOR))
        audio_sync(m);
}

int audio_read(Machine *m, int16_t *out, int frames)
{
    Audio *a = audio_state(m);
    audio_sync(m);
    int n = frames < a->count ? frames : a->count;
    for (int i = 0; i < n; i++)
        memcpy(out + i * AUDIO_CHANNELS, ring_at(a, i), sizeof(int16_t) * AUDIO_CHANNELS);
    ring_drop(a, n);
    return n;
}

int audio_available(Machine *m)
{
    audio_sync(m);
    return audio_state(m)->count;
}

/* Linear interpolation over last + ring[0..in-1], consuming all in frames. */
static int resample(Audio *a, int16_t *out, int cap, int in, double step)
{
    int n = 0;
    double p = a->phase;
    for (; p < in - 1 && n < cap; p += step, n++) {
        int i = p < 0 ? -1 : (int)p;
        const int16_t *x0 = i < 0 ? a->last : ring_at(a, i), *x1 = ring_at(a, i + 1);
        double t = p - i;
        for (int c = 0; c < AUDIO_CHANNELS; c++)
            out[n * AUDIO_CHANNELS + c] = (int16_t)(x0[c] + (x1[c] - x0[c]) * t);
    }
    memcpy(a->last, ring_at(a, in - 1), sizeof a->last);
    a->phase = p - in;
    ring_drop(a, in);
    return n;
}

static double clampd(double v, double lim) { return v > lim ? lim : v < -lim ? -lim : v; }

/* Latency error as a fraction of the target, smoothed over about 20 reads. */
static double track_error(Audio *a, int latency, int target)
{
    double err = (double)(latency - target) / target;
    a->error_avg += (err - a->error_avg) * ERROR_SMOOTHING;
    return a->error_avg;
}

int audio_read_paced(Machine *m, int16_t *out, int cap, int queued)
{
    Audio *a = audio_state(m);
    audio_sync(m);
    int target_ms = a->target_ms ? a->target_ms : AUDIO_TARGET_MS;
    int target = a->rate * target_ms / 1000, n = 0;
    if (queued == 0 && a->count < target) {
        a->stats.underruns++;
        n = target - a->count < cap ? target - a->count : cap;
        memset(out, 0, sizeof(int16_t) * AUDIO_CHANNELS * (size_t)n);
    }
    int excess = queued + a->count - a->rate * (target_ms * AUDIO_MAX_LATENCY_MS / AUDIO_TARGET_MS) / 1000;
    if (excess > 0) {
        a->stats.dropped_frames += (uint32_t)excess;
        ring_drop(a, excess);
    }
    if (track_error(a, queued + n + a->count, target) > RESYNC_ERROR) {
        a->stats.dropped_frames += (uint32_t)(queued + n + a->count - target);
        a->stats.resyncs++;
        ring_drop(a, queued + n + a->count - target);
        a->error_avg = (double)(queued + n + a->count - target) / target;
    }
    double step = 1.0 + clampd(a->error_avg * STRETCH_GAIN, AUDIO_MAX_STRETCH_PPM / 1e6);
    int in = a->count;
    if (in > (int)((cap - n - 1) * step))
        in = (int)((cap - n - 1) * step);
    if (in > 0)
        n += resample(a, out + n * AUDIO_CHANNELS, cap - n, in, step);
    return n;
}

/* ---- host PCM: the DSP's voice model (sb.c) reading guest memory directly ---- */

enum { PCM_SAMPLE_SHIFT = 8, PCM_DEFAULT_TIME_CONSTANT = 0xD2, NS_PER_US = 1000, DMA_PAGE_MASK = 0xFF0000 };

static const double PCM_DC_BLOCK_POLE = 0.999; /* sb.c's: the line output is AC-coupled */

AudioPcm *audio_pcm(Machine *m) { return &audio_state(m)->pcm; }

/* The next buffer byte as channel 1 would move it; false while the buffer is held. */
static bool pcm_fetch(Machine *m, AudioPcm *p, uint8_t *b)
{
    if (p->dma_held || p->dma_len == 0)
        return false;
    uint32_t at = (p->dma_base & DMA_PAGE_MASK) | ((p->dma_base + p->dma_pos) & 0xFFFF);
    *b = mem_is_vga(at) ? 0xFF : m->mem[at];
    if (++p->dma_pos == p->dma_len) {
        p->dma_pos = 0;
        p->dma_held = !p->dma_auto;
    }
    return true;
}

/* Fetches every byte due by t, as sb.c's advance does; a held buffer stalls the block. */
static void pcm_advance(Machine *m, AudioPcm *p, uint64_t t)
{
    PcmVoice *v = &p->v;
    while (v->active && !v->paused && v->next_ns <= t) {
        uint8_t b;
        if (!pcm_fetch(m, p, &b)) {
            v->next_ns += ((t - v->next_ns) / v->period_ns + 1) * v->period_ns;
            break;
        }
        v->prev = v->cur;
        v->cur = b;
        v->cur_ns = v->next_ns;
        v->cur_period = v->period_ns;
        v->next_ns += v->period_ns;
        if (--v->left == 0) {
            p->irq = true;
            if (v->auto_init)
                v->left = v->block;
            else
                v->active = false;
        }
    }
}

static double pcm_level(const PcmVoice *v, uint64_t t)
{
    if (!v->speaker)
        return 0;
    double x = t < v->cur_ns ? v->prev : v->cur;
    if (t >= v->cur_ns && t < v->cur_ns + v->cur_period)
        x = v->prev + (double)(v->cur - v->prev) * (double)(t - v->cur_ns) / (double)v->cur_period;
    return (x - PCM_SILENCE) * (1 << PCM_SAMPLE_SHIFT);
}

static void pcm_render(Machine *m, int32_t *mix, int frames)
{
    AudioPcm *p = audio_pcm(m);
    PcmVoice *v = &p->v;
    uint64_t t0 = audio_frame_ns(m, 0);
    if (!v->active && t0 >= v->cur_ns + v->cur_period && pcm_level(v, t0) == v->prev_in && v->prev_out > -1 &&
        v->prev_out < 1)
        return; /* idle and settled */
    for (int i = 0; i < frames; i++) {
        uint64_t t = audio_frame_ns(m, i);
        pcm_advance(m, p, t);
        double in = pcm_level(v, t);
        v->prev_out = in - v->prev_in + PCM_DC_BLOCK_POLE * v->prev_out;
        v->prev_in = in;
        int32_t s = (int32_t)v->prev_out;
        mix[i * AUDIO_CHANNELS] += s;
        mix[i * AUDIO_CHANNELS + 1] += s;
    }
}

void audio_pcm_sync(Machine *m)
{
    audio_sync(m);
    pcm_advance(m, audio_pcm(m), machine_now_ns(m));
}

uint64_t audio_pcm_block_end_ns(Machine *m)
{
    const AudioPcm *p = audio_pcm(m);
    const PcmVoice *v = &p->v;
    if (!v->active || v->paused || p->dma_held || (!p->dma_auto && p->dma_len - p->dma_pos < v->left))
        return UINT64_MAX; /* stopped, or the buffer runs out first */
    return v->next_ns + (uint64_t)(v->left - 1) * v->period_ns;
}

static void pcm_init(AudioPcm *p)
{
    *p = (AudioPcm){ .time_constant = PCM_DEFAULT_TIME_CONSTANT };
    p->v.period_ns = (uint64_t)(256 - PCM_DEFAULT_TIME_CONSTANT) * NS_PER_US;
    p->v.prev = p->v.cur = PCM_SILENCE;
}

size_t sound_snapshot_size(void) { return sb_snapshot_size() + dma_snapshot_size() + sizeof(AudioPcm); }

void sound_snapshot_save(Machine *m, void *dst)
{
    uint8_t *d = dst;
    sb_snapshot_save(m, d);
    dma_snapshot_save(m, d + sb_snapshot_size());
    if (audio_state(m))
        memcpy(d + sb_snapshot_size() + dma_snapshot_size(), audio_pcm(m), sizeof(AudioPcm));
    else
        memset(d + sb_snapshot_size() + dma_snapshot_size(), 0, sizeof(AudioPcm));
}

void sound_snapshot_load(Machine *m, const void *src)
{
    const uint8_t *s = src;
    sb_snapshot_load(m, s);
    dma_snapshot_load(m, s + sb_snapshot_size());
    if (audio_state(m))
        memcpy(audio_pcm(m), s + sb_snapshot_size() + dma_snapshot_size(), sizeof(AudioPcm));
}

int audio_add_source(Machine *m, AudioRenderFn fn)
{
    Audio *a = audio_state(m);
    if (a->source_count >= AUDIO_MAX_SOURCES)
        return -1;
    a->sources[a->source_count] = fn;
    return a->source_count++;
}

void audio_set_rate(Machine *m, int rate)
{
    Audio *a = audio_state(m);
    a->rate = rate;
    a->base_ns = machine_now_ns(m);
    a->frames_done = 0;
    a->tap_next = (uint64_t)rate;
    a->read = a->count = 0;
    a->phase = -1.0;
    a->error_avg = 0;
}

int audio_rate(Machine *m) { return audio_state(m)->rate; }

void audio_set_target_ms(Machine *m, int ms)
{
    audio_state(m)->target_ms = ms;
}

AudioStats audio_take_stats(Machine *m)
{
    AudioStats s = audio_state(m)->stats;
    audio_state(m)->stats = (AudioStats){ 0 };
    return s;
}

void audio_enable(Machine *m, bool on)
{
    audio_sync(m);
    audio_state(m)->enabled = on;
}

void audio_init(Machine *m)
{
    Audio *a = calloc(1, sizeof *a);
    if (!a)
        fatal("out of memory");
    m->audio = a;
    audio_set_rate(m, AUDIO_DEFAULT_RATE);
    pcm_init(&a->pcm);
    audio_name_source(m, audio_add_source(m, pcm_render), "pcm");
    machine_add_ticker(m, audio_tick);
}

void audio_shutdown(Machine *m)
{
    free(m->audio);
    m->audio = NULL;
}
