/* Scripted host for the TSNEXEC stand-in; the file format is described in host_rules.c. */
#ifndef HOST_RULES_H
#define HOST_RULES_H

#include "machine.h"

typedef struct HostRules HostRules;

/* Parses the file and applies its shared-block and previous-program lines (after exec_stub_install). */
HostRules *host_rules_load(Machine *m, const char *path);
/* ExecSendHook: answers a client message with every matching rule. */
void host_rules_on_send(Machine *m, const uint8_t *msg, uint16_t len, void *user);
/* ExecPollHook: queues the replies of "at <ms>" rules whose time has come. */
void host_rules_poll(Machine *m, void *user);
void host_rules_free(HostRules *s);

#endif
