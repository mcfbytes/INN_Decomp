/* MPU-401: reset and UART-mode acknowledges, status bits, data to the MIDI sink only in UART mode, the
 * Standard MIDI File writer, the host API and the semantic event for a 330h write. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "host_audio.h"
#include "lift_semantic.h"
#include "mpu401.h"

static int failures;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            failures++;                                          \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fputc('\n', stderr);                                 \
        }                                                        \
    } while (0)

typedef struct Capture { uint8_t bytes[64]; uint64_t ns[64]; int n; } Capture;

static void capture(void *ctx, uint64_t ns, uint8_t byte)
{
    Capture *c = ctx;
    if (c->n < 64)
        c->ns[c->n] = ns, c->bytes[c->n++] = byte;
}

static Machine *boot(void)
{
    Machine *m = machine_create();
    bios_init(m);
    return m;
}

static void finish(Machine *m)
{
    bios_shutdown(m);
    machine_destroy(m);
}

static uint8_t status(Machine *m) { return (uint8_t)port_in(m, MPU_PORT_STATUS, 1); }

static void test_commands(void)
{
    Machine *m = boot();
    Capture c = { 0 };
    mpu401_set_sink(m, capture, &c);
    CHECK(!(status(m) & MPU_STATUS_DRR), "ready for a command");
    CHECK(status(m) & MPU_STATUS_DSR, "nothing to read after power-on");
    port_out(m, MPU_PORT_DATA, 0x90, 1);
    CHECK(c.n == 0, "intelligent mode passes no data to MIDI OUT");

    port_out(m, MPU_PORT_STATUS, MPU_CMD_RESET, 1);
    CHECK(!(status(m) & MPU_STATUS_DSR), "reset queues an acknowledge");
    CHECK(port_in(m, MPU_PORT_DATA, 1) == MPU_ACK, "reset acknowledged with FEh");
    CHECK(status(m) & MPU_STATUS_DSR, "acknowledge read");

    port_out(m, MPU_PORT_STATUS, MPU_CMD_UART, 1);
    CHECK(mpu401_uart_mode(m), "3Fh enters UART mode");
    CHECK(port_in(m, MPU_PORT_DATA, 1) == MPU_ACK, "UART mode acknowledged");
    port_out(m, MPU_PORT_STATUS, MPU_CMD_UART, 1);
    CHECK(status(m) & MPU_STATUS_DSR, "UART mode ignores commands but reset");

    m->virtual_us = 5000;
    port_out(m, MPU_PORT_DATA, 0x91, 1);
    port_out(m, MPU_PORT_DATA, 0x3C, 1);
    port_out(m, MPU_PORT_DATA, 0x40, 1);
    CHECK(c.n == 3 && c.bytes[0] == 0x91 && c.bytes[1] == 0x3C && c.bytes[2] == 0x40, "data bytes reach the sink (%d)", c.n);
    CHECK(c.ns[0] >= 5000000, "stamped with emulated time (%llu)", (unsigned long long)c.ns[0]);

    host_audio_midi_write(m, 0x7B);
    CHECK(c.n == 4 && c.bytes[3] == 0x7B, "host API writes to MIDI OUT");

    port_out(m, MPU_PORT_STATUS, MPU_CMD_RESET, 1);
    CHECK(!mpu401_uart_mode(m), "reset leaves UART mode");
    CHECK(port_in(m, MPU_PORT_DATA, 1) == MPU_ACK, "reset in UART mode acknowledged");
    CHECK(port_in(m, MPU_PORT_DATA, 1) == MPU_ACK, "an empty queue reads FEh");
    finish(m);
}

static size_t read_file(const char *path, uint8_t *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static void test_midi_file(void)
{
    const char *path = "test_mpu401.mid";
    MidiFile *f = midi_file_open(path);
    CHECK(f, "open %s", path);
    if (!f)
        return;
    static const uint8_t stream[] = {
        0x90, 0x3C, 0x40,             /* note on, t=0 */
        0x3E, 0x40,                   /* running status, t=2 ms */
        0xF0, 0x41, 0x10, 0x16, 0xF7, /* system exclusive, t=3 ms */
        0x45,                         /* stray data byte, t=3 ms */
        0xC1, 0x05,                   /* program change, t=3 ms */
        0x80, 0x3C,                   /* left open at the end */
    };
    for (size_t i = 0; i < sizeof stream; i++) {
        uint64_t ns = i < 3 ? 0 : i < 5 ? 2000000 : 3000000;
        midi_file_byte(f, ns, stream[i]);
    }
    CHECK(midi_file_close(f) == 0, "close");
    uint8_t buf[256];
    size_t n = read_file(path, buf, sizeof buf);
    static const uint8_t want[] = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xF4, 'M', 'T', 'r', 'k', 0, 0, 0, 38,
        0, 0xFF, 0x51, 3, 0x07, 0xA1, 0x20,
        0, 0x90, 0x3C, 0x40,
        2, 0x90, 0x3E, 0x40,
        1, 0xF0, 4, 0x41, 0x10, 0x16, 0xF7,
        0, 0xF7, 1, 0x45,
        0, 0xC1, 0x05,
        0, 0xF7, 2, 0x80, 0x3C,
        0, 0xFF, 0x2F, 0,
    };
    CHECK(n == sizeof want && !memcmp(buf, want, sizeof want), "file bytes (%zu, want %zu)", n, sizeof want);
    for (size_t i = 0; i < n && i < sizeof want; i++)
        if (buf[i] != want[i]) {
            fprintf(stderr, "  first difference at %zu: %02X want %02X\n", i, buf[i], want[i]);
            break;
        }
    remove(path);
}

static void test_semantic(void)
{
    SemTrace t;
    memset(&t, 0, sizeof t);
    sem_trace_init(&t);
    sem_trace_port(&t, MPU_PORT_STATUS, 1, 0xBF, false);
    sem_trace_port(&t, MPU_PORT_DATA, 1, 0x90, true);
    sem_trace_port(&t, MPU_PORT_STATUS, 1, MPU_CMD_RESET, true);
    sem_trace_port(&t, MPU_PORT_DATA, 1, MPU_ACK, false);
    sem_trace_finish(&t);
    CHECK(t.nsem == 1 && t.sem[0].kind == SEM_MIDI_BYTE && t.sem[0].a == 0x90, "330h write is a MIDI byte");
    CHECK(t.nraw == 3, "status reads, commands and data reads stay raw (%zu)", t.nraw);
    sem_trace_free(&t);
}

int main(void)
{
    test_commands();
    test_midi_file();
    test_semantic();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_mpu401: ok\n");
    return 0;
}
