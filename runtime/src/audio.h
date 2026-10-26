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
    AUDIO_MAX_SOURCES = 6,
};

/* Adds frames of interleaved stereo to mix, using the source's state as it is now. */
typedef void (*AudioRenderFn)(Machine *m, int32_t *mix, int frames);
/* Inside a render callback: the emulated time of frame i of the chunk being rendered. */
uint64_t audio_frame_ns(Machine *m, int i);

/* ---- mixer ---- */

/* Debug tap (--audiotap, page ?audiotap=1): per source, the frames rendered and their peak and RMS, logged as
 * "audio: tap" once per second of output. Off by default; when off the mixer takes one branch per chunk. */
void audio_tap_enable(Machine *m, bool on);
/* Names source index (from audio_add_source) for the tap; the name is not copied. */
void audio_name_source(Machine *m, int source, const char *name);
/* Peak |sample| a named source has rendered since the tap was enabled (rolled into the last whole second
 * plus the one in progress); 0 when the name is unknown. For tests and tools. */
int32_t audio_tap_peak(Machine *m, const char *name);

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
/* Glitches since the last call, for --perf: the output ran dry, or queued audio was thrown away
 * to bound latency (in one go when the level stayed far over target). */
typedef struct AudioStats { uint32_t underruns, resyncs, dropped_frames; } AudioStats;
AudioStats audio_take_stats(Machine *m);

/* Queued audio to aim for (default AUDIO_TARGET_MS); the drop limit scales with it. A host whose
 * frames can run their emulated time in bursts (the browser, with lifted code) wants more. */
void audio_set_target_ms(Machine *m, int ms);

enum { AUDIO_TARGET_MS = 50, AUDIO_MAX_LATENCY_MS = 200, AUDIO_MAX_STRETCH_PPM = 5000 };
int audio_read_paced(Machine *m, int16_t *out, int cap, int queued);

/* ---- devices ---- */

/* OPL2 at 388h/389h and the Sound Blaster mirrors 220h-223h and 228h/229h, timers included. */
void opl_init(Machine *m);
void opl_shutdown(Machine *m);
/* What the address/data and status ports do: audio_sync before a sound change, timers from machine_now_ns. */
void opl_write_reg(Machine *m, uint8_t reg, uint8_t value);
uint8_t opl_read_status(Machine *m);
/* Sound Blaster 2.0 DSP at 220h (IRQ 7, DMA 1): 8-bit mono DMA playback and the direct DAC. */
enum { SB_BASE = 0x220, SB_DMA_CHANNEL = 1, SB_DSP_VERSION = 0x0201 };
void sb_init(Machine *m);
void sb_shutdown(Machine *m);

/* ---- digital voice: 8-bit unsigned mono, as the Sound Blaster DSP plays it ----
 * Byte i of a block is fetched (i + 1) periods after the start; the output ramps to each byte over one
 * period and goes through the AC coupling of the line output. The DSP (sb.c) and the host PCM below
 * use the same model, so a block can change hands between them (docs/native.md section 11). */
typedef struct PcmVoice {
    bool active, auto_init, paused, speaker;
    uint32_t block, left;       /* bytes per block; bytes still to fetch in this one */
    uint64_t period_ns;         /* time between bytes */
    uint64_t next_ns;           /* when the next byte is fetched */
    uint64_t pause_left_ns;     /* paused: time from the continue to the next byte */
    uint8_t prev, cur;          /* the output ramps from prev to cur over cur_period from cur_ns */
    uint64_t cur_ns, cur_period;
    double prev_in, prev_out;   /* AC coupling filter */
} PcmVoice;

enum { PCM_SILENCE = 0x80 };

/* The DSP's voice for the mixed-mode hand-off. get copies it (a pending exit from auto-init counts as
 * single-cycle); take copies it and leaves the DSP idle and silent, its IRQ line low; set replaces it. */
void sb_voice_get(Machine *m, PcmVoice *v);
void sb_voice_take(Machine *m, PcmVoice *v);
void sb_voice_set(Machine *m, const PcmVoice *v);
/* DSP time constant (40h): a byte every 256 - tc microseconds. */
uint8_t sb_time_constant(Machine *m);

/* Host PCM (host_audio_pcm_start, native mode): a mixer source that plays from guest memory with no
 * DSP and no DMA controller. The buffer is read as channel 1 would read it: dma_len bytes from
 * dma_base, wrapping within its 64K page, restarting at its end when dma_auto, else holding the DSP
 * (no more bytes, no block end) once it ran out. A block end sets irq; the native core raises IRQ 7. */
typedef struct AudioPcm {
    PcmVoice v;
    uint32_t dma_base, dma_len, dma_pos;
    bool dma_auto, dma_held;
    bool irq;                   /* a block ended and IRQ 7 is not yet raised */
    uint8_t time_constant;      /* the last one set (host_audio_sb_time_constant) */
} AudioPcm;

AudioPcm *audio_pcm(Machine *m);
/* Renders what is due under the old state and fetches every byte due by now; call before a change. */
void audio_pcm_sync(Machine *m);
/* Emulated time at which the current block ends (UINT64_MAX when none will). */
uint64_t audio_pcm_block_end_ns(Machine *m);

/* Sound Blaster DSP, 8237 DMA controller and host PCM state, for shadow and synthetic snapshots
 * (lift_verify.c, lift_synth.c): plain bytes, no pointers. */
size_t sb_snapshot_size(void);
void sb_snapshot_save(Machine *m, void *dst);
void sb_snapshot_load(Machine *m, const void *src);
size_t sound_snapshot_size(void);
void sound_snapshot_save(Machine *m, void *dst);
void sound_snapshot_load(Machine *m, const void *src);

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
