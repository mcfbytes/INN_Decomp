/* Host input (docs/native.md section 9, package 6). In emulated mode the platform calls go to the 8042
 * and INT 33h models; in native mode keys are queued as scancode bytes and delivered one per IRQ 1, and
 * mouse events are handed to the client's INT 33h event handler at a safe point. Guest-side calls call
 * native_safepoint first; the platform side calls native_wake when it queues something. */
#ifndef DOSRT_HOST_INPUT_H
#define DOSRT_HOST_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "machine.h"

/* Platform side: the frontend and the input script call these in every mode. */
void host_input_key(Machine *m, uint16_t set1_scancode, bool down);  /* 0xE0xx = E0-prefixed */
void host_input_mouse_motion(Machine *m, int x, int y, int frame_w, int frame_h);
void host_input_mouse_button(Machine *m, int button, bool down);
/* Guest side, native mode: the byte delivered with the current IRQ 1 (port 60h equivalent). */
uint8_t host_input_kbd_data(Machine *m);
void host_input_kbd_ack(Machine *m);                       /* port 61h strobe: nothing to do */
/* Native core, at every safe point: latch the next key byte and raise IRQ 1, or hand a mouse event to
 * the INT 33h handler. */
void host_input_poll(Machine *m);

#endif
