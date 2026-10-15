/* Host video stubs until package 4 implements them. */
#include "host_video.h"

void host_video_init(Machine *m) { fatal("host_video_init: not implemented (package 4)"); }
void host_video_set_mode(Machine *m, uint8_t mode, bool clear) { fatal("host_video_set_mode: not implemented (package 4)"); }
uint8_t host_video_mode(const Machine *m) { fatal("host_video_mode: not implemented (package 4)"); }
void host_video_set_dac(Machine *m, uint8_t first, int count, const uint8_t *rgb6) { fatal("host_video_set_dac: not implemented (package 4)"); }
void host_video_get_dac(Machine *m, uint8_t first, int count, uint8_t *rgb6) { fatal("host_video_get_dac: not implemented (package 4)"); }
void host_video_set_start(Machine *m, uint16_t start) { fatal("host_video_set_start: not implemented (package 4)"); }
void host_video_wait_retrace(Machine *m) { fatal("host_video_wait_retrace: not implemented (package 4)"); }
bool host_video_in_retrace(Machine *m) { fatal("host_video_in_retrace: not implemented (package 4)"); }
void host_video_render(Machine *m, uint32_t *argb, int *w, int *h) { fatal("host_video_render: not implemented (package 4)"); }
int host_video_write_ppm(Machine *m, const char *path) { fatal("host_video_write_ppm: not implemented (package 4)"); }
void host_video_to_vga(Machine *m) { fatal("host_video_to_vga: not implemented (package 4)"); }
void host_video_from_vga(Machine *m) { fatal("host_video_from_vga: not implemented (package 4)"); }
