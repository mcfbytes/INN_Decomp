/* PC speaker: the cone follows PIT channel 2 while port 61h bit 1 is set. With the gate off the
 * output is held high, so drivers that toggle bit 1 directly (pulse-width sound) are heard. */
#include <stdlib.h>

#include "audio.h"
#include "devices.h"

enum { SPEAKER_AMPLITUDE = 6000 };

static const double DC_BLOCK_POLE = 0.999; /* a real cone cannot hold a constant offset */

typedef struct Speaker {
    double phase; /* fraction of the current square-wave period */
    double prev_in, prev_out;
} Speaker;

static Speaker *speaker_state(Machine *m) { return m->speaker; }

/* Cone drive in [0, 1] for one output frame. */
static double drive(Speaker *s, bool tone, double hz, bool held, int rate)
{
    if (!tone)
        return held ? 1.0 : 0.0;
    if (hz * 2 >= rate)
        return 0.5; /* ultrasonic: only the average position is audible */
    double v = s->phase < 0.5 ? 1.0 : 0.0;
    s->phase += hz / rate;
    s->phase -= (int)s->phase;
    return v;
}

static void speaker_render(Machine *m, int32_t *mix, int frames)
{
    Speaker *s = speaker_state(m);
    double hz = 0;
    bool tone = pit_speaker(m, &hz), held = pit_speaker_held(m);
    if (!tone && !held && s->prev_in == 0 && s->prev_out > -1 && s->prev_out < 1)
        return;
    int rate = audio_rate(m);
    for (int i = 0; i < frames; i++) {
        double in = drive(s, tone, hz, held, rate) * SPEAKER_AMPLITUDE;
        s->prev_out = in - s->prev_in + DC_BLOCK_POLE * s->prev_out;
        s->prev_in = in;
        int32_t v = (int32_t)s->prev_out;
        mix[i * AUDIO_CHANNELS] += v;
        mix[i * AUDIO_CHANNELS + 1] += v;
    }
}

void speaker_init(Machine *m)
{
    Speaker *s = calloc(1, sizeof *s);
    if (!s)
        fatal("out of memory");
    m->speaker = s;
    audio_add_source(m, speaker_render);
}

void speaker_shutdown(Machine *m)
{
    free(m->speaker);
    m->speaker = NULL;
}
