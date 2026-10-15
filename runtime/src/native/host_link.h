/* Host link (docs/native.md section 9, package 7): the existing SerialLink (TCP natively, WebSocket in
 * the browser). The client keeps its whole link stack; only the UART goes: the driver's rings in guest
 * memory are filled and drained by a pump. Every guest-side call calls native_safepoint first; the pump
 * calls native_wake when bytes arrive. */
#ifndef DOSRT_HOST_LINK_H
#define DOSRT_HOST_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "machine.h"

int host_link_open(Machine *m, const char *spec);      /* host:port or ws:// / wss:// URL */
bool host_link_carrier(Machine *m);                    /* connected: DCD, DSR and CTS */
int host_link_send(Machine *m, const uint8_t *buf, int len); /* bytes taken; fewer while connecting */
int host_link_recv(Machine *m, uint8_t *buf, int cap);
void host_link_set_dtr(Machine *m, bool on);
void host_link_break(Machine *m);                      /* sends the CR stand-in innkeeperd expects */
/* The driver's ring pump (the UART interrupt's work, done without a UART) lives with the lifted driver
 * and registers itself here; host_link_pump runs it at safe points in native mode and as a machine
 * ticker in emulated mode (games call TSNEXEC meanwhile). */
void host_link_set_pump(Machine *m, void (*pump)(Machine *m));
void host_link_pump(Machine *m);

#endif
