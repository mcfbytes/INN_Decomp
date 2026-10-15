/* Host input stubs until package 6 implements them; the safe point's poll does nothing yet. */
#include "host_input.h"

void host_input_key(Machine *m, uint16_t set1_scancode, bool down) { fatal("host_input_key: not implemented (package 6)"); }
void host_input_mouse_motion(Machine *m, int x, int y, int frame_w, int frame_h) { fatal("host_input_mouse_motion: not implemented (package 6)"); }
void host_input_mouse_button(Machine *m, int button, bool down) { fatal("host_input_mouse_button: not implemented (package 6)"); }
uint8_t host_input_kbd_data(Machine *m) { fatal("host_input_kbd_data: not implemented (package 6)"); }
void host_input_kbd_ack(Machine *m) { fatal("host_input_kbd_ack: not implemented (package 6)"); }
void host_input_poll(Machine *m) { (void)m; }
