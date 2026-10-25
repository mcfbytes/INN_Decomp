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
/* True once an "end" event has been applied: the caller ends the run. */
bool input_script_ended(const InputScript *s);
/* Applies every event due at or before m->virtual_us. */
void input_script_apply(InputScript *s, Machine *m);

#endif
