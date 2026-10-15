/* Host link stubs until package 7 implements them; the safe point's pump does nothing yet. */
#include "host_link.h"

int host_link_open(Machine *m, const char *spec) { fatal("host_link_open: not implemented (package 7)"); }
bool host_link_carrier(Machine *m) { fatal("host_link_carrier: not implemented (package 7)"); }
int host_link_send(Machine *m, const uint8_t *buf, int len) { fatal("host_link_send: not implemented (package 7)"); }
int host_link_recv(Machine *m, uint8_t *buf, int cap) { fatal("host_link_recv: not implemented (package 7)"); }
void host_link_set_dtr(Machine *m, bool on) { fatal("host_link_set_dtr: not implemented (package 7)"); }
void host_link_break(Machine *m) { fatal("host_link_break: not implemented (package 7)"); }
void host_link_set_pump(Machine *m, void (*pump)(Machine *m)) { fatal("host_link_set_pump: not implemented (package 7)"); }
void host_link_pump(Machine *m) { (void)m; }
