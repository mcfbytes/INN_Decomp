/* Roland MPU-401 at 330h/331h, as far as a UART-mode driver uses it, and a Standard MIDI File writer
 * for what it sends (docs/runtime.md, "MPU-401"). Nothing is synthesised: the bytes only go to a sink. */
#ifndef DOSRT_MPU401_H
#define DOSRT_MPU401_H

#include <stdbool.h>
#include <stdint.h>

#include "machine.h"

enum {
    MPU_PORT_DATA = 0x330,
    MPU_PORT_STATUS = 0x331, /* status on read, command on write */
    MPU_STATUS_DRR = 0x40,   /* clear: the MPU takes a byte */
    MPU_STATUS_DSR = 0x80,   /* clear: a byte waits at 330h */
    MPU_CMD_UART = 0x3F,
    MPU_CMD_RESET = 0xFF,
    MPU_ACK = 0xFE,
};

/* A byte on the MIDI OUT jack, at emulated time ns. */
typedef void (*MidiSinkFn)(void *ctx, uint64_t ns, uint8_t byte);

/* Called by bios_init / bios_shutdown (the shutdown closes a --midi-out file). */
void mpu401_init(Machine *m);
void mpu401_shutdown(Machine *m);
/* One sink at a time; NULL detaches. */
void mpu401_set_sink(Machine *m, MidiSinkFn fn, void *ctx);
/* --midi-out: a type 0 Standard MIDI File, one tick per millisecond of emulated time; 0 or -1. */
int mpu401_open_midi_out(Machine *m, const char *path);
/* A byte to MIDI OUT now, whatever the mode (host_audio_midi_write; a 330h write in UART mode). */
void mpu401_midi_byte(Machine *m, uint8_t byte);
bool mpu401_uart_mode(Machine *m);

/* ---- Standard MIDI File writer ---- */

typedef struct MidiFile MidiFile;

MidiFile *midi_file_open(const char *path);
/* A MidiSinkFn: parses the stream into events (running status expanded, system exclusive whole,
 * anything else as an F7 escape) stamped at the byte that completes them. */
void midi_file_byte(void *ctx, uint64_t ns, uint8_t byte);
/* Writes what is still open, ends the track, patches the length and closes; 0 or -1 on I/O error. */
int midi_file_close(MidiFile *f);

#endif
