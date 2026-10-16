/* Host video. DAC programming has its device backend (hybrid-hw); the framebuffer, retrace and
 * hand-off are package 4.4-4.6. */
#include "host_video.h"

#include "devices.h"
#include "lift_semantic.h"
#include "vga_internal.h"

void host_video_init(Machine *m) { fatal("host_video_init: not implemented (package 4)"); }
void host_video_set_mode(Machine *m, uint8_t mode, bool clear) { fatal("host_video_set_mode: not implemented (package 4)"); }
uint8_t host_video_mode(const Machine *m) { fatal("host_video_mode: not implemented (package 4)"); }

/* After a whole run of colours the write/read index sits where 3C8h/3C7h plus the data bytes leave it,
 * so a shadow snapshot matches the original port sequence. */
static void dac_index_after(Machine *m, uint8_t first, int count, bool reading)
{
    Vga *v = vga_state(m);
    uint8_t next = (uint8_t)(first + count);
    if (reading) {
        v->dac_read = next;
        v->dac_read_sub = 0;
        v->dac_last_read = true;
    } else {
        v->dac_write = next;
        v->dac_write_sub = 0;
        v->dac_last_read = false;
    }
}

void host_video_set_dac(Machine *m, uint8_t first, int count, const uint8_t *rgb6)
{
    if (m->native)
        fatal("host_video_set_dac: native path is package 4.4");
    if (count <= 0)
        return;
    for (int i = 0; i < count; i++) {
        const uint8_t *p = rgb6 + 3 * i;
        uint8_t index = (uint8_t)(first + i);
        vga_dac_set(m, index, p[0], p[1], p[2]);
        lift_semantic_event(m, SEM_DAC_SET, index, (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]);
    }
    dac_index_after(m, first, count, false);
}

void host_video_get_dac(Machine *m, uint8_t first, int count, uint8_t *rgb6)
{
    if (m->native)
        fatal("host_video_get_dac: native path is package 4.4");
    if (count <= 0)
        return;
    for (int i = 0; i < count; i++) {
        uint8_t live[3];
        uint8_t index = (uint8_t)(first + i);
        vga_dac_get(m, index, live);
        uint32_t packed = lift_semantic_read(m, SEM_DAC_GET, index,
                                             (uint32_t)live[0] << 16 | (uint32_t)live[1] << 8 | live[2]);
        rgb6[3 * i] = (uint8_t)(packed >> 16);
        rgb6[3 * i + 1] = (uint8_t)(packed >> 8);
        rgb6[3 * i + 2] = (uint8_t)packed;
    }
    dac_index_after(m, first, count, true);
}

void host_video_set_start(Machine *m, uint16_t start) { fatal("host_video_set_start: not implemented (package 4)"); }
void host_video_wait_retrace(Machine *m) { fatal("host_video_wait_retrace: not implemented (package 4)"); }
bool host_video_in_retrace(Machine *m) { fatal("host_video_in_retrace: not implemented (package 4)"); }
void host_video_render(Machine *m, uint32_t *argb, int *w, int *h) { fatal("host_video_render: not implemented (package 4)"); }
int host_video_write_ppm(Machine *m, const char *path) { fatal("host_video_write_ppm: not implemented (package 4)"); }
void host_video_to_vga(Machine *m) { fatal("host_video_to_vga: not implemented (package 4)"); }
void host_video_from_vga(Machine *m) { fatal("host_video_from_vga: not implemented (package 4)"); }
