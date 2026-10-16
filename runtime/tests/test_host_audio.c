/* Host audio: OPL register writes and status match the port handlers, a shadow replay does not touch
 * the chip, and the native path charges a safe point. PCM playback is not implemented. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "audio.h"
#include "devices.h"
#include "host_audio.h"
#include "native.h"

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

enum { RATE = AUDIO_DEFAULT_RATE, TONE_US = 100000, FRAMES = RATE / 10 };

static Machine *boot(void)
{
    Machine *m = machine_create();
    bios_init(m);
    audio_enable(m, true);
    return m;
}

static void finish(Machine *m)
{
    bios_shutdown(m);
    machine_destroy(m);
}

/* AdLib timer probe: mask, reset, preset FFh, start timer 1, then read after `us`. */
static uint8_t probe(Machine *m, bool via_host, uint64_t us)
{
    void (*wr)(Machine *, uint8_t, uint8_t) = via_host ? host_audio_opl_write : opl_write_reg;
    wr(m, 0x04, 0x60);
    wr(m, 0x04, 0x80);
    wr(m, 0x02, 0xFF);
    wr(m, 0x04, 0x21);
    m->virtual_us += us;
    return via_host ? host_audio_opl_status(m) : opl_read_status(m);
}

static void test_timers_match_ports(void)
{
    Machine *m = boot();
    port_out(m, 0x388, 0x04, 1);
    port_out(m, 0x389, 0x60, 1);
    port_out(m, 0x388, 0x04, 1);
    port_out(m, 0x389, 0x80, 1);
    uint8_t port_reset = (uint8_t)port_in(m, 0x388, 1);
    port_out(m, 0x388, 0x02, 1);
    port_out(m, 0x389, 0xFF, 1);
    port_out(m, 0x388, 0x04, 1);
    port_out(m, 0x389, 0x21, 1);
    m->virtual_us += 200;
    uint8_t port_later = (uint8_t)port_in(m, 0x388, 1);
    finish(m);

    m = boot();
    opl_write_reg(m, 0x04, 0x60);
    opl_write_reg(m, 0x04, 0x80);
    uint8_t fn_reset = opl_read_status(m);
    opl_write_reg(m, 0x02, 0xFF);
    opl_write_reg(m, 0x04, 0x21);
    m->virtual_us += 200;
    uint8_t fn_later = opl_read_status(m);
    finish(m);

    m = boot();
    uint8_t host_later = probe(m, true, 200);
    uint8_t host_early = 0;
    finish(m);
    m = boot();
    host_early = probe(m, true, 40);
    finish(m);

    CHECK((port_reset & 0xE0) == 0 && (fn_reset & 0xE0) == 0, "reset status port %02X fn %02X", port_reset, fn_reset);
    CHECK(port_later == fn_later, "status after 200 us: port %02X fn %02X", port_later, fn_later);
    CHECK((port_later & 0xE0) == 0xC0 && (port_later & 0x07) == 0x06, "timer 1 expired, OPL2 id %02X", port_later);
    CHECK(host_later == port_later, "host status %02X port %02X", host_later, port_later);
    CHECK((host_early & 0xE0) == 0, "timer 1 still running after 40 us: %02X", host_early);
}

static void play_a440(Machine *m, bool via_host)
{
    static const uint8_t REGS[][2] = {
        { 0x20, 0x01 }, { 0x23, 0x21 }, { 0x40, 0x3F }, { 0x43, 0x00 }, { 0x60, 0xF0 },
        { 0x63, 0xF0 }, { 0x80, 0x00 }, { 0x83, 0x00 }, { 0xC0, 0x00 }, { 0xA0, 0x44 },
        { 0xB0, 0x32 },
    };
    for (size_t i = 0; i < sizeof REGS / sizeof REGS[0]; i++) {
        if (via_host)
            host_audio_opl_write(m, REGS[i][0], REGS[i][1]);
        else
            opl_write_reg(m, REGS[i][0], REGS[i][1]);
    }
}

static int read_tone(Machine *m, int16_t *buf)
{
    m->virtual_us += TONE_US;
    return audio_read(m, buf, FRAMES);
}

static void test_write_sounds_like_ports(void)
{
    int16_t a[FRAMES * 2], b[FRAMES * 2];
    Machine *m = boot();
    play_a440(m, false);
    int na = read_tone(m, a);
    finish(m);
    m = boot();
    play_a440(m, true);
    int nb = read_tone(m, b);
    finish(m);
    CHECK(na == FRAMES && nb == na, "frames port %d host %d", na, nb);
    CHECK(memcmp(a, b, (size_t)na * 2 * sizeof(int16_t)) == 0, "host writes render the same samples as the ports");
    double sum = 0;
    for (int i = 0; i < na * 2; i++)
        sum += (double)a[i] * (double)a[i];
    CHECK(sum > 0, "the tone is silent");
}

/* A replay must not apply the write: timer 1 stays overflowed across a reset that would clear it. */
static void test_replay_does_not_write(void)
{
    Machine *m = boot();
    uint8_t overflow = probe(m, false, 200);
    m->io_mode = IO_REPLAY;
    host_audio_opl_write(m, 0x04, 0x80);
    uint8_t during = opl_read_status(m);
    m->io_mode = IO_LIVE;
    host_audio_opl_write(m, 0x04, 0x80);
    uint8_t after = host_audio_opl_status(m);
    finish(m);
    CHECK((overflow & 0xE0) == 0xC0, "probe did not overflow: %02X", overflow);
    CHECK(during == overflow, "replay applied the reset: before %02X during %02X", overflow, during);
    CHECK((after & 0xE0) == 0, "live reset left the flags set: %02X", after);
}

static void test_native_safepoint_charges(void)
{
    Machine *m = boot();
    NativeConfig cfg = { .strict = true, .deterministic = true, .call_cost_ns = 1000, .yield_every_us = 4000 };
    native_init(m, &cfg);
    native_set_active(m, true);
    m->cpu.flags &= ~(uint32_t)F_IF;
    uint64_t t0 = m->virtual_us;
    host_audio_opl_write(m, 0x20, 0x01);
    CHECK(m->virtual_us == t0 + 1, "write charged %llu us", (unsigned long long)(m->virtual_us - t0));
    uint8_t st = host_audio_opl_status(m);
    CHECK(m->virtual_us == t0 + 2, "status charged %llu us", (unsigned long long)(m->virtual_us - t0));
    CHECK((st & 0x07) == 0x06, "native status id %02X", st);
    /* The register reached the chip: a note still renders after further writes. */
    play_a440(m, true);
    int16_t buf[FRAMES * 2];
    int n = read_tone(m, buf);
    double sum = 0;
    for (int i = 0; i < n * 2; i++)
        sum += (double)buf[i] * (double)buf[i];
    CHECK(n == FRAMES && sum > 0, "native write produced no samples (n %d)", n);
    finish(m);
}

static void pcm_start(void)
{
    host_audio_pcm_start(NULL, 0, 0, 0, false);
}

static void pcm_stop(void)
{
    host_audio_pcm_stop(NULL);
}

static int run_child(void (*fn)(void), char *out, size_t cap)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 2);
        close(fds[0]);
        fn();
        _exit(0);
    }
    close(fds[1]);
    size_t len = 0;
    ssize_t got;
    while (len + 1 < cap && (got = read(fds[0], out + len, cap - 1 - len)) > 0)
        len += (size_t)got;
    out[len] = 0;
    close(fds[0]);
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void test_pcm_unimplemented(void)
{
    char out[256];
    int status = run_child(pcm_start, out, sizeof out);
    CHECK(status == 1 && strstr(out, "not implemented"), "pcm start: status %d %s", status, out);
    status = run_child(pcm_stop, out, sizeof out);
    CHECK(status == 1 && strstr(out, "not implemented"), "pcm stop: status %d %s", status, out);
}

int main(void)
{
    test_timers_match_ports();
    test_write_sounds_like_ports();
    test_replay_does_not_write();
    test_native_safepoint_charges();
    test_pcm_unimplemented();
    if (failures) {
        fprintf(stderr, "test_host_audio: %d failure(s)\n", failures);
        return 1;
    }
    printf("host audio ok\n");
    return 0;
}
