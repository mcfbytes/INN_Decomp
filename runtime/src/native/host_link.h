/* Host link (docs/native.md section 9, package 7): the existing SerialLink (TCP natively, WebSocket in
 * the browser). The client keeps its whole link stack; only the UART goes: the driver's rings in guest
 * memory are filled and drained by a pump. Every guest-side call calls native_safepoint first; the pump
 * calls native_wake when bytes arrive. */
#ifndef DOSRT_HOST_LINK_H
#define DOSRT_HOST_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "devices.h"
#include "machine.h"

int host_link_open(Machine *m, const char *spec);      /* host:port or ws:// / wss:// URL */
/* Takes ownership of a link built elsewhere (tests, other transports) in place of host_link_open. */
int host_link_attach(Machine *m, SerialLink link);
void host_link_close(Machine *m);
bool host_link_carrier(Machine *m);                    /* connected: DCD, DSR and CTS */
int host_link_send(Machine *m, const uint8_t *buf, int len); /* bytes taken; fewer while connecting */
int host_link_recv(Machine *m, uint8_t *buf, int cap);
/* The line rate the driver programmed. From then on send and recv carry one byte per byte time of it
 * (10 bits), as the UART did, unless INN_LINK_PACING=0. 0 baud turns pacing off. */
void host_link_set_rate(Machine *m, uint32_t baud);
void host_link_set_dtr(Machine *m, bool on);
/* serial_link_drain on the attached link; not a safe point (the machine is held still). */
void host_link_drain(Machine *m);
void host_link_break(Machine *m);                      /* sends the CR stand-in innkeeperd expects */
/* The driver's ring pump (the UART interrupt's work, done without a UART) lives with the lifted driver
 * and registers itself here; host_link_pump runs it at safe points in native mode and as a machine
 * ticker in emulated mode (games call TSNEXEC meanwhile). */
void host_link_set_pump(Machine *m, void (*pump)(Machine *m)); /* NULL unregisters */
/* Throttled to one run per millisecond of emulated time, and not while interrupts are held (a shadow
 * run); the driver's own kick calls its pump directly. */
void host_link_pump(Machine *m);
/* Runs the pump now, whatever the throttle, for bytes the driver just queued. Not re-entered. */
void host_link_kick(Machine *m);

#endif
