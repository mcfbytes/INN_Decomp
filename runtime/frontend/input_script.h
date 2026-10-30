/* Timed input for headless runs: keys, text, mouse and screenshots from a small script file.
 * Format and examples: docs/runtime.md, "Input scripts". */
#ifndef DOSRT_INPUT_SCRIPT_H
#define DOSRT_INPUT_SCRIPT_H

#include <stdbool.h>

#include "machine.h"

typedef struct InputScript InputScript;

/* Returns NULL and logs the offending line on a parse error. */
InputScript *input_script_load(const char *path);
void input_script_free(InputScript *s);
/* Emulated time of the next pending event, or UINT64_MAX when none is left. */
uint64_t input_script_next_us(const InputScript *s);
/* Applies every event due at or before m->virtual_us. Returns -1, and applies nothing more, once a "wait"
 * timed out or a "signal"/"wait" failed (no sync directory, file not created). */
int input_script_apply(InputScript *s, Machine *m);
/* Cross-process sync for "signal" and "wait": their names are files in dir. A wait calls hold(ctx)
 * between checks of the wall clock, with the emulated clock held. */
void input_script_set_sync(InputScript *s, const char *dir, void (*hold)(void *ctx), void *ctx);
bool input_script_failed(const InputScript *s);
/* True after an "end" event: the caller should stop the run. */
bool input_script_ended(const InputScript *s);

#endif
