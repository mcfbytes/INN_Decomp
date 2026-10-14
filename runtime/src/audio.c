/* Audio mixer. Output frame n is emulated time base_ns + n / rate, so sound follows the emulated
 * clock; sources render lazily, before each change to their sound (audio_sync) and on reads. */
#include <stdlib.h>
#include <string.h>

#include "audio.h"

enum {
    CHUNK_FRAMES = 256,
    TICK_SYNC_DIVISOR = 200, /* the ticker renders once 1/200 s (5 ms) is pending */
    NS_PER_S = 1000000000,
};

static const double ERROR_SMOOTHING = 0.05; /* per paced read, about 60 per second */
static const double STRETCH_GAIN = 0.01;    /* relative stretch per unit of queue error */
static const double RESYNC_ERROR = 0.5;     /* a sustained excess the stretch would take seconds to drain */

typedef struct Audio {
    int rate;
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

static void render(Machine *m, Audio *a, uint64_t frames)
{
    int32_t mix[CHUNK_FRAMES * AUDIO_CHANNELS];
    while (frames > 0) {
        int n = frames < CHUNK_FRAMES ? (int)frames : CHUNK_FRAMES;
        memset(mix, 0, sizeof mix);
        for (int s = 0; s < a->source_count; s++)
            a->sources[s](m, mix, n);
        ring_push(a, mix, n);
        a->frames_done += (uint64_t)n;
        frames -= (uint64_t)n;
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
    int target = a->rate * AUDIO_TARGET_MS / 1000, n = 0;
    if (queued == 0 && a->count < target) {
        n = target - a->count < cap ? target - a->count : cap;
        memset(out, 0, sizeof(int16_t) * AUDIO_CHANNELS * (size_t)n);
    }
    int excess = queued + a->count - a->rate * AUDIO_MAX_LATENCY_MS / 1000;
    if (excess > 0)
        ring_drop(a, excess);
    if (track_error(a, queued + n + a->count, target) > RESYNC_ERROR) {
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
    a->read = a->count = 0;
    a->phase = -1.0;
    a->error_avg = 0;
}

int audio_rate(Machine *m) { return audio_state(m)->rate; }

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
    machine_add_ticker(m, audio_tick);
}

void audio_shutdown(Machine *m)
{
    free(m->audio);
    m->audio = NULL;
}
