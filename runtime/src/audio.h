/* dosrt sound: a mixer rendering every device in step with emulated time (OPL2, Sound Blaster DSP,
 * PC speaker) and a WAV writer. Design notes: docs/runtime.md. */
#ifndef DOSRT_AUDIO_H
#define DOSRT_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "machine.h"

enum {
    AUDIO_DEFAULT_RATE = 48000,
    AUDIO_CHANNELS = 2,      /* interleaved stereo int16 frames everywhere */
    AUDIO_RING_FRAMES = 32768, /* about 0.7 s at 48 kHz; the oldest frames drop on overflow */
    AUDIO_MAX_SOURCES = 4,
};

/* Adds frames of interleaved stereo to mix, using the source's state as it is now. */
typedef void (*AudioRenderFn)(Machine *m, int32_t *mix, int frames);
/* Inside a render callback: the emulated time of frame i of the chunk being rendered. */
uint64_t audio_frame_ns(Machine *m, int i);

/* ---- mixer ---- */

void audio_init(Machine *m);
void audio_shutdown(Machine *m);
/* Returns the source index, or -1 when the table is full. */
int audio_add_source(Machine *m, AudioRenderFn fn);
/* Output rate in Hz; restarts the sample clock (call before the guest makes sound). */
void audio_set_rate(Machine *m, int rate);
int audio_rate(Machine *m);
/* Rendering is off until a sink (device or WAV) wants samples; the clock keeps running. */
void audio_enable(Machine *m, bool on);
/* Renders up to machine_now_ns; devices call it before changing what they would play. */
void audio_sync(Machine *m);
/* Rendered frames waiting to be read. */
int audio_available(Machine *m);
/* Copies up to frames rendered frames into out; returns the count. */
int audio_read(Machine *m, int16_t *out, int frames);

/* For a real-time sink with queued frames buffered: fills out (at most cap) so the queue stays near
 * the target. Drift is stretched away (AUDIO_MAX_STRETCH_PPM at most); underruns get silence. */
enum { AUDIO_TARGET_MS = 50, AUDIO_MAX_LATENCY_MS = 200, AUDIO_MAX_STRETCH_PPM = 5000 };
int audio_read_paced(Machine *m, int16_t *out, int cap, int queued);

/* ---- devices ---- */

/* OPL2 at 388h/389h and the Sound Blaster mirrors 220h-223h and 228h/229h, timers included. */
void opl_init(Machine *m);
void opl_shutdown(Machine *m);
/* Sound Blaster 2.0 DSP at 220h (IRQ 7, DMA 1): 8-bit mono DMA playback and the direct DAC. */
enum { SB_BASE = 0x220, SB_DMA_CHANNEL = 1, SB_DSP_VERSION = 0x0201 };
void sb_init(Machine *m);
void sb_shutdown(Machine *m);
/* Square wave from PIT channel 2 gated by port 61h. */
void speaker_init(Machine *m);
void speaker_shutdown(Machine *m);

/* ---- WAV output (16-bit stereo PCM) ---- */

typedef struct WavWriter WavWriter;

WavWriter *wav_open(const char *path, int rate);
/* Returns 0, or -1 on I/O error. */
int wav_write(WavWriter *w, const int16_t *frames, int count);
/* Patches the header sizes and closes; returns 0, or -1 on I/O error. */
int wav_close(WavWriter *w);

#endif
