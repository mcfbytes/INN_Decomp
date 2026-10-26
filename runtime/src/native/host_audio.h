/* Host audio (docs/native.md section 9, package 5). The OPL2 core, mixer and sinks are the existing
 * ones; only the register access changes from ports to calls, at the native clock's time. Every call
 * calls native_safepoint first. */
#ifndef DOSRT_HOST_AUDIO_H
#define DOSRT_HOST_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "machine.h"

void host_audio_opl_write(Machine *m, uint8_t reg, uint8_t value);
uint8_t host_audio_opl_status(Machine *m);                 /* timer flags, for the detection probe */
/* Sound Blaster digital output, 8-bit mono from guest memory; raises IRQ 7 at each block end. */
int host_audio_pcm_start(Machine *m, uint32_t linear, uint16_t bytes, uint32_t rate_hz, bool auto_init);
void host_audio_pcm_stop(Machine *m);

/* ---- Sound Blaster digital output in detail (docs/native.md section 9, "host_audio.h") ----
 * Each call stands for one DSP or DMA port sequence. Native mode plays through the host PCM source
 * (audio.h); emulated mode, hybrid-hw builds included, performs the sequence on the emulated devices,
 * so a shadow run's lifted half is canonicalised into the same semantic events as the original's.
 *
 * host_audio_pcm_start: 8237 channel 1 masked, flip-flop cleared, mode 49h (59h auto-init), address,
 * count, page 83h, unmasked; then DSP 14h with the length (auto-init: 48h with the length, then 1Ch).
 * bytes 0 is 65536. rate_hz is first turned into a time constant and sent as 40h (a byte every
 * 256 - tc us, so the rate is that of host_audio_sb_rate); rate_hz 0 keeps the time constant already
 * set. Returns 0, or -1 with nothing done when 1000000 / rate_hz does not round to 1-256 us (no time
 * constant gives it: below about 3900 Hz). The buffer and the DSP block are the same bytes: one IRQ 7 per pass. */
enum { HOST_AUDIO_SB_IRQ = 7, HOST_AUDIO_SB_RESET_ACK = 0xAA, HOST_AUDIO_SB_STATUS_READY = 0x7F };

/* DSP reset: 226h 1 then 0, then the data byte (AAh). Stops playback and mutes the speaker. */
uint8_t host_audio_sb_reset(Machine *m);
/* E0h with v: the DSP answers ~v. */
uint8_t host_audio_sb_identify(Machine *m, uint8_t v);
/* E1h: major << 8 | minor (0201h). */
uint16_t host_audio_sb_version(Machine *m);
/* D1h (on) or D3h (off). */
void host_audio_sb_speaker(Machine *m, bool on);
/* 40h tc. Applies from the next byte; host_audio_pcm_start with rate_hz 0 uses it. */
void host_audio_sb_time_constant(Machine *m, uint8_t tc);
/* The rate a time constant gives: 1000000 / (256 - tc) Hz. */
uint32_t host_audio_sb_rate(uint8_t tc);
/* Channel 1 masked on its own (0Ah 05h): no more bytes move, the block stalls. host_audio_pcm_stop is
 * the DSP halt (D0h) followed by this. */
void host_audio_pcm_mask(Machine *m);
/* The interrupt acknowledge (22Eh read); returns the status byte. */
uint8_t host_audio_sb_ack(Machine *m);

/* ---- for the native core ---- */

/* At every safe point: brings the host PCM up to now and raises IRQ 7 for a block that ended. */
void host_audio_poll(Machine *m);
/* Emulated time of the next block end (now when an IRQ is waiting; UINT64_MAX when none), for waits. */
uint64_t host_audio_next_due_us(Machine *m);
/* Mixed-mode hand-off (docs/native.md section 11): the host PCM's block, phase, speaker and output
 * filter go to the DSP and DMA channel 1, and back. */
void host_audio_to_devices(Machine *m);
void host_audio_from_devices(Machine *m);

#endif
