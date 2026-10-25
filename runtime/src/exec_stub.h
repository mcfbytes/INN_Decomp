/* Stand-in for TSNEXEC's INT 14h export table, so a DOS game client (Red Baron, Golf, the Ybarra
 * programs) can run alone against a scripted host. docs/runtime.md, "TSN host stand-in". */
#ifndef EXEC_STUB_H
#define EXEC_STUB_H

#include <stdint.h>

#include "machine.h"

/* Called for every message the client sends (Send and multicast alike), with its bytes. */
typedef void (*ExecSendHook)(Machine *m, const uint8_t *msg, uint16_t len, void *user);

/* Called at every stand-in Send, Receive and Poll callback (a host that acts on emulated time). */
typedef void (*ExecPollHook)(Machine *m, void *user);
void exec_stub_set_poll_hook(Machine *m, ExecPollHook on_poll, void *user);

/* Hooks INT 14h (after bios_init) to return the stand-in's export table. */
void exec_stub_install(Machine *m, ExecSendHook on_send, void *user);
void exec_stub_set_send_hook(Machine *m, ExecSendHook on_send, void *user);
void exec_stub_shutdown(Machine *m);

/* Shared block contents (at most 256 bytes) and the name GetPreviousProgram returns. */
void exec_stub_set_shared(Machine *m, uint16_t offset, const uint8_t *data, uint16_t len);
void exec_stub_set_previous(Machine *m, const char *name);

/* Queues a host-to-client message for the client's next Receive. */
void exec_stub_deliver(Machine *m, const uint8_t *msg, uint16_t len);

#endif
