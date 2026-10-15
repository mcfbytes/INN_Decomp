/* Scripted host for the TSNEXEC stand-in; the file format is described in tsn_script.c. */
#ifndef TSN_SCRIPT_H
#define TSN_SCRIPT_H

#include "machine.h"

typedef struct TsnScript TsnScript;

/* Parses the file and applies its shared-block and previous-program lines (after tsn_hle_install). */
TsnScript *tsn_script_load(Machine *m, const char *path);
/* TsnSendHook: answers a client message with every matching rule. */
void tsn_script_on_send(Machine *m, const uint8_t *msg, uint16_t len, void *user);
void tsn_script_free(TsnScript *s);

#endif
