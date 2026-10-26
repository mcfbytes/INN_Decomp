/* PIT channel 0 as a host timer (docs/native.md sections 5 and 9): IRQ 0 is due by the native clock.
 * Every call is a safe point for the caller's purposes: the native core polls the timer there. */
#ifndef DOSRT_HOST_TIMER_H
#define DOSRT_HOST_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#include "machine.h"

void host_timer_set_divisor(Machine *m, uint16_t divisor); /* PIT channel 0, mode 3; 0 = 65536 */
uint16_t host_timer_divisor(const Machine *m);
uint64_t host_timer_next_due_us(const Machine *m);         /* for native_idle */
void host_timer_speaker(Machine *m, uint16_t divisor, bool on); /* channel 2 square wave and port 61h gates */

/* Native core: turns elapsed clock time into IRQ 0 requests, at most HOST_TIMER_MAX_CATCH_UP overdue
 * ticks, one raised per call while the previous one is no longer requested. */
enum { HOST_TIMER_MAX_CATCH_UP = 5 };
void host_timer_poll(Machine *m);

#endif
