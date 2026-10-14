/* Frontend audio sinks: the SDL2 device in queue mode (no audio thread touches the machine, so
 * it also works under Emscripten) and an optional WAV capture of the same samples. */
#ifndef INN_AUDIO_SDL_H
#define INN_AUDIO_SDL_H

#include <stdbool.h>

#include "machine.h"

/* Opens the sinks: the host device when device is true, a WAV file when wav_path is set.
 * Returns 0, or -1 when a requested sink failed (sound stays off for that sink). */
int frontend_audio_open(Machine *m, bool device, const char *wav_path);
/* Moves rendered audio to the sinks; call after each machine_run_for. */
void frontend_audio_pump(Machine *m);
/* Flushes and closes the sinks; returns 0, or -1 if the WAV could not be finished. */
int frontend_audio_close(Machine *m);

#endif
