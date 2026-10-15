/* Host video (docs/native.md section 9, package 4). In native mode A0000-BFFFF are plain memory: mode
 * 13h pixels at A0000 (linear, as chain-4 maps them), text cells at B8000. The host video owns the
 * mode, the 256-entry DAC (6-bit values), the CRTC start address and the presentation. Every call that
 * stands for a port access calls native_safepoint first. */
#ifndef DOSRT_HOST_VIDEO_H
#define DOSRT_HOST_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#include "machine.h"

enum { HOST_VIDEO_TEXT80 = 0x03, HOST_VIDEO_320X200X8 = 0x13 };

void host_video_init(Machine *m);
void host_video_set_mode(Machine *m, uint8_t mode, bool clear);
uint8_t host_video_mode(const Machine *m);
void host_video_set_dac(Machine *m, uint8_t first, int count, const uint8_t *rgb6); /* 3 bytes each */
void host_video_get_dac(Machine *m, uint8_t first, int count, uint8_t *rgb6);
void host_video_set_start(Machine *m, uint16_t start);   /* CRTC start address (shake, scroll) */
void host_video_wait_retrace(Machine *m);                /* present if due, wait for the 70 Hz boundary */
bool host_video_in_retrace(Machine *m);                  /* for code that polls the status bit */
void host_video_render(Machine *m, uint32_t *argb, int *w, int *h); /* VGA_MAX_W * VGA_MAX_H buffer */
int host_video_write_ppm(Machine *m, const char *path);
void host_video_to_vga(Machine *m);      /* mixed mode: hand mode, DAC and pixels to the VGA device */
void host_video_from_vga(Machine *m);    /* and take them back */

#endif
