/* Stand-in for TSNEXEC's INT 14h export table, so a DOS game client (Red Baron, Golf, the Ybarra
 * programs) can run alone against a scripted host. docs/runtime.md, "TSN host stand-in". */
#ifndef TSN_HLE_H
#define TSN_HLE_H

#include <stdint.h>

#include "machine.h"

/* Called for every message the client sends (Send and multicast alike), with its bytes. */
typedef void (*TsnSendHook)(Machine *m, const uint8_t *msg, uint16_t len, void *user);

/* Hooks INT 14h (after bios_init) to return the stand-in's export table. */
void tsn_hle_install(Machine *m, TsnSendHook on_send, void *user);
void tsn_hle_set_send_hook(Machine *m, TsnSendHook on_send, void *user);
void tsn_hle_shutdown(Machine *m);

/* Shared block contents (at most 256 bytes) and the name GetPreviousProgram returns. */
void tsn_hle_set_shared(Machine *m, uint16_t offset, const uint8_t *data, uint16_t len);
void tsn_hle_set_previous(Machine *m, const char *name);

/* Queues a host-to-client message for the client's next Receive. */
void tsn_hle_deliver(Machine *m, const uint8_t *msg, uint16_t len);

#endif
