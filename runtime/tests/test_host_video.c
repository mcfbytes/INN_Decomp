/* Host video device backend: DAC writes and reads leave the same VGA state as the ports. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "host_video.h"

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

int main(void)
{
    test_dac_matches_ports();
    if (failures)
        return 1;
    printf("host video ok\n");
    return 0;
}
