/* Host video device backend: DAC writes and reads leave the same VGA state as the ports. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "host_video.h"
#include "lift.h"
#include "native.h"
#include "vga_internal.h"

static int failures;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

static void test_dac_matches_ports(void)
{
    Machine *m = machine_create();
    bios_init(m);
    size_t bytes = vga_snapshot_size();
    uint8_t *before = malloc(bytes);
    uint8_t *via_ports = malloc(bytes);
    uint8_t *via_host = malloc(bytes);
    CHECK(before && via_ports && via_host, "snapshot buffers");
    if (!before || !via_ports || !via_host) {
        bios_shutdown(m);
        machine_destroy(m);
        free(before);
        free(via_ports);
        free(via_host);
        return;
    }
    vga_snapshot_save(m, before);

    const uint8_t rgb[6] = { 0x0A | 0xC0, 0x14, 0x1E, 0x3F, 0x00, 0x01 };
    port_out(m, 0x3C8, 5, 1);
    for (int i = 0; i < 6; i++)
        port_out(m, 0x3C9, rgb[i], 1);
    vga_snapshot_save(m, via_ports);

    vga_snapshot_load(m, before);
    host_video_set_dac(m, 5, 2, rgb);
    vga_snapshot_save(m, via_host);
    CHECK(memcmp(via_ports, via_host, bytes) == 0, "set_dac VGA state differs from the port run");
    CHECK((uint8_t)port_in(m, 0x3C8, 1) == 7, "write index after two colours");
    CHECK((uint8_t)port_in(m, 0x3C7, 1) == 0, "last DAC op was a write");

    vga_snapshot_load(m, before);
    const uint8_t one[3] = { 0x0A, 0x14, 0x1E };
    host_video_set_dac(m, 0xC8, 1, one);
    vga_snapshot_save(m, before);

    uint8_t pr = 0, pg = 0, pb = 0;
    port_out(m, 0x3C7, 0xC8, 1);
    pr = (uint8_t)port_in(m, 0x3C9, 1);
    pg = (uint8_t)port_in(m, 0x3C9, 1);
    pb = (uint8_t)port_in(m, 0x3C9, 1);
    vga_snapshot_save(m, via_ports);

    uint8_t got[3];
    vga_snapshot_load(m, before);
    host_video_get_dac(m, 0xC8, 1, got);
    vga_snapshot_save(m, via_host);
    CHECK(got[0] == 0x0A && got[1] == 0x14 && got[2] == 0x1E, "readback %02X %02X %02X", got[0], got[1], got[2]);
    CHECK(got[0] == pr && got[1] == pg && got[2] == pb, "get_dac bytes differ from the port reads");
    CHECK(memcmp(via_ports, via_host, bytes) == 0, "get_dac VGA state differs from the port run");

    host_video_set_dac(m, 255, 1, one);
    CHECK((uint8_t)port_in(m, 0x3C8, 1) == 0, "write index wraps");

    bios_shutdown(m);
    machine_destroy(m);
    free(before);
    free(via_ports);
    free(via_host);
}


enum { STACK_SEG = 0x2000, STACK_TOP = 0xF000, SCREEN_PIXELS = 320 * 200 * 4, FRAME_US = 14286 };

static Machine *make_machine(void)
{
    Machine *m = machine_create();
    bios_init(m);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    m->cpu.r[R_SP].w = STACK_TOP;
    return m;
}

static void fill_text(Machine *m)
{
    const char *msg = "host video text";
    for (int row = 0; row < 25; row++)
        for (int col = 0; col < 80; col++) {
            uint32_t at = 0xB8000 + (uint32_t)(row * 80 + col) * 2;
            mem_wr8(m, at, (uint8_t)msg[(row + col) % 15]);
            mem_wr8(m, at + 1, (uint8_t)(row * 7 + col));
        }
}

static void fill_pixels(Machine *m)
{
    uint8_t rgb[3 * 256];
    for (int i = 0; i < 256; i++) {
        rgb[3 * i] = (uint8_t)(i & 63);
        rgb[3 * i + 1] = (uint8_t)((i * 3) & 63);
        rgb[3 * i + 2] = (uint8_t)(63 - (i & 63));
    }
    host_video_set_dac(m, 0, 256, rgb);
    for (uint32_t n = 0; n < 64000; n++)
        mem_wr8(m, 0xA0000 + n, (uint8_t)(n * 7 + n / 320));
}

static uint32_t *frame_vga(Machine *m, int *w, int *h)
{
    static uint32_t buf[VGA_MAX_W * VGA_MAX_H];
    memset(buf, 0, sizeof buf);
    vga_render(m, buf, w, h);
    return buf;
}

static void compare_with_vga(const char *what, Machine *m, const uint32_t *want, int ww, int wh)
{
    static uint32_t got[VGA_MAX_W * VGA_MAX_H];
    int w = 0, h = 0;
    memset(got, 0, sizeof got);
    host_video_render(m, got, &w, &h);
    CHECK(w == ww && h == wh, "%s: size %dx%d, want %dx%d", what, w, h, ww, wh);
    CHECK(w == ww && h == wh && memcmp(got, want, (size_t)w * h * 4) == 0, "%s: pixels differ from vga_render", what);
}

/* A picture drawn through the VGA device, taken over by the host video, renders the same pixels; handed
 * back and changed in plain memory it renders the same as the device does for those bytes. */
static void test_render_and_handoff(void)
{
    static uint32_t saved[VGA_MAX_W * VGA_MAX_H];
    Machine *m = make_machine();
    vga_set_mode(m, 0x03, true);
    fill_text(m);
    int w, h;
    memcpy(saved, frame_vga(m, &w, &h), sizeof saved);
    host_video_init(m);
    CHECK(m->native_video, "init routes A0000-BFFFF to memory");
    CHECK(mem_rd8(m, 0xB8000) == 'h', "text cell moved into memory");
    compare_with_vga("text", m, saved, w, h);

    mem_wr8(m, 0xB8000 + 2 * 81, 'Z');
    mem_wr8(m, 0xB8000 + 2 * 81 + 1, 0x1E);
    host_video_to_vga(m);
    CHECK(!m->native_video, "to_vga routes back to the device");
    CHECK(mem_rd8(m, 0xB8000 + 2 * 81) == 'Z' && mem_rd8(m, 0xB8000 + 2 * 81 + 1) == 0x1E, "cell reached the device");
    CHECK(vga_mode(m) == 0x03, "mode 03h kept");
    memcpy(saved, frame_vga(m, &w, &h), sizeof saved);
    host_video_from_vga(m);
    compare_with_vga("text after round trip", m, saved, w, h);
    host_video_to_vga(m);

    vga_set_mode(m, 0x13, true);
    host_video_init(m);
    fill_pixels(m);
    host_video_set_start(m, 0);
    uint8_t first[3] = { 0, 0, 0 };
    host_video_get_dac(m, 0, 1, first);
    host_video_to_vga(m);
    memcpy(saved, frame_vga(m, &w, &h), sizeof saved);
    CHECK(w == 320 && h == 200, "mode 13h frame %dx%d", w, h);
    CHECK(mem_rd8(m, 0xA0000 + 5) == (uint8_t)(5 * 7) && mem_rd8(m, 0xA0000 + 321) == (uint8_t)(321 * 7 + 1),
          "pixels reached the planes");
    host_video_from_vga(m);
    compare_with_vga("mode 13h", m, saved, w, h);
    CHECK(host_video_mode(m) == 0x13, "mode");

    host_video_set_start(m, 80); /* one row down: the picture scrolls by 320 bytes */
    static uint32_t got[VGA_MAX_W * VGA_MAX_H];
    host_video_render(m, got, &w, &h);
    CHECK(got[0] == saved[320] && got[100] == saved[420], "start address scrolls the picture");
    host_video_set_start(m, 0);

    host_video_set_mode(m, 0x03, true);
    CHECK(mem_rd8(m, 0xB8000) == ' ' && mem_rd8(m, 0xB8001) == 0x07 && mem_rd8(m, 0xA0000 + 5) == 0,
          "mode set clears pixels and text");
    host_video_to_vga(m);
    bios_shutdown(m);
    machine_destroy(m);
}

static void test_retrace(void)
{
    Machine *m = make_machine();
    host_video_init(m);
    for (int i = 0; i < 5; i++) {
        host_video_wait_retrace(m);
        CHECK(m->virtual_us % FRAME_US == 0, "wait ends on a frame boundary (%llu)", (unsigned long long)m->virtual_us);
        CHECK(host_video_in_retrace(m), "in retrace right after the wait");
        native_wait_until(m, m->virtual_us + FRAME_US / 2);
        CHECK(!host_video_in_retrace(m), "not in retrace half a frame later");
    }
    uint64_t before = m->virtual_us;
    host_video_wait_retrace(m);
    CHECK(m->virtual_us > before && m->virtual_us - before <= FRAME_US, "a wait takes at most one frame");
    host_video_to_vga(m);
    bios_shutdown(m);
    machine_destroy(m);
}

static void int10(Machine *m, uint16_t ax)
{
    AX = ax;
    guest_int(m, 0x10);
}

static void test_bios_native(void)
{
    Machine *m = make_machine();
    host_video_init(m);
    m->native = true;

    int10(m, 0x0013);
    CHECK(vga_mode(m) == 0x13 && m->native_video, "set mode 13h");
    mem_wr8(m, 0xA0000 + 100, 9);
    int10(m, 0x0F00);
    CHECK(AL == 0x13, "get mode returns 13h (AL=%02X)", AL);
    int10(m, 0x0093); /* bit 7: keep memory */
    CHECK(mem_rd8(m, 0xA0000 + 100) == 9, "mode set with bit 7 keeps the pixels");
    int10(m, 0x0013);
    CHECK(mem_rd8(m, 0xA0000 + 100) == 0, "mode set clears the pixels");

    const uint8_t pal[6] = { 1, 2, 3, 63, 0x7F, 5 };
    for (int i = 0; i < 6; i++)
        mem_wr8(m, 0x30000 + (uint32_t)i, pal[i]);
    cpu_set_seg(&m->cpu, S_ES, 0x3000);
    BX = 10, CX = 2, DX = 0;
    int10(m, 0x1012);
    uint8_t got[3];
    host_video_get_dac(m, 11, 1, got);
    CHECK(got[0] == 63 && got[1] == 0x3F && got[2] == 5, "block write masks to 6 bits");
    for (int i = 0; i < 6; i++)
        mem_wr8(m, 0x30100 + (uint32_t)i, 0xEE);
    BX = 10, CX = 2, DX = 0x100;
    int10(m, 0x1017);
    CHECK(mem_rd8(m, 0x30100) == 1 && mem_rd8(m, 0x30105) == 5, "block read returns the colours");
    BX = 10;
    int10(m, 0x1015);
    CHECK(DH == 1 && CH == 2 && CL == 3, "single read");
    DH = 0x41, CH = 0x22, CL = 0x03;
    BX = 12;
    int10(m, 0x1010);
    host_video_get_dac(m, 12, 1, got);
    CHECK(got[0] == 1 && got[1] == 0x22 && got[2] == 3, "single write");

    int10(m, 0x0E41); /* teletype 'A' in mode 13h draws the 8x8 glyph into memory */
    bool drawn = false;
    for (int i = 0; i < 8 * 320 && !drawn; i++)
        drawn = mem_rd8(m, 0xA0000 + (uint32_t)i) != 0;
    CHECK(drawn, "teletype draws into the host framebuffer");
    BX = 0x000F;
    int10(m, 0x0E42);

    int10(m, 0x0003);
    CHECK(vga_mode(m) == 0x03 && mem_rd8(m, 0xB8000) == ' ', "set text mode");
    BH = 0;
    DX = 0x0502;
    int10(m, 0x0200);
    DH = 0;
    BH = 0;
    int10(m, 0x0300);
    CHECK(DX == 0x0502, "cursor position round trip (DX=%04X)", DX);
    BX = 0x0007;
    int10(m, 0x0E58);
    CHECK(mem_rd8(m, 0xB8000 + 2 * (5 * 80 + 2)) == 'X', "teletype writes the text cell in memory");

    m->native = false;
    host_video_to_vga(m);
    bios_shutdown(m);
    machine_destroy(m);
}

int main(void)
{
    test_dac_matches_ports();
    test_render_and_handoff();
    test_retrace();
    test_bios_native();
    if (failures)
        return 1;
    printf("host video ok\n");
    return 0;
}
