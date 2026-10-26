/* Host audio: OPL register writes and status match the port handlers, a shadow replay does not touch
 * the chip, and the native path charges a safe point. Sound Blaster output: the host PCM renders a block
 * as the emulated DSP does, raises IRQ 7 at each block end (auto-init repeats, stop and mask end it), takes
 * its rate from the time constant, hands a block to the devices and back without a seam, and the
 * emulated-mode port sequences canonicalise to the semantic events a shadow run compares. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "devices.h"
#include "host_audio.h"
#include "lift_semantic.h"
#include "native.h"
#include "native_internal.h"

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

/* ---- Sound Blaster digital output ---- */

enum {
    BUF = 0x20000, BLOCK = 1000, TC_100US = 256 - 100, RATE_10K = 10000, PERIOD_US = 100,
    IRQ7_BIT = 0x80, T0_US = 1000,
};

/* Native mode with free calls, so the API's safe points do not move the clock. */
static Machine *boot_native(void)
{
    Machine *m = boot();
    NativeConfig cfg = { .strict = true, .deterministic = true, .call_cost_ns = 0, .yield_every_us = 4000 };
    native_init(m, &cfg);
    native_set_active(m, true);
    m->cpu.flags &= ~(uint32_t)F_IF;
    return m;
}

static void fill_square(Machine *m)
{
    for (int i = 0; i < BLOCK; i++)
        m->mem[BUF + i] = i / 10 % 2 ? 0xC0 : 0x40;
}

static bool irq7(Machine *m) { return native_state(m)->pic[0].irr & IRQ7_BIT; }

/* Polls at us; true when IRQ 7 was raised there (and takes it back). */
static bool irq_at(Machine *m, uint64_t us)
{
    m->virtual_us = us;
    host_audio_poll(m);
    bool raised = irq7(m);
    native_irq_lower(m, HOST_AUDIO_SB_IRQ);
    return raised;
}

static double energy(const int16_t *buf, int frames)
{
    double sum = 0;
    for (int i = 0; i < frames * 2; i++)
        sum += (double)buf[i] * (double)buf[i];
    return sum;
}

/* The same block through the emulated DSP and DMA (emulated mode: the API's port sequence) and through
 * the host PCM (native mode) renders the same samples. */
static void test_pcm_renders_like_sb(void)
{
    enum { PLAY_US = 150000, N = RATE * 15 / 100 };
    static int16_t a[N * 2], b[N * 2];
    int n[2];
    for (int native = 0; native < 2; native++) {
        Machine *m = native ? boot_native() : boot();
        fill_square(m);
        m->virtual_us = T0_US;
        uint8_t ack = host_audio_sb_reset(m);
        host_audio_sb_speaker(m, true);
        int r = host_audio_pcm_start(m, BUF, BLOCK, RATE_10K, false);
        CHECK(ack == 0xAA && r == 0, "%s: reset %02X start %d", native ? "native" : "emulated", ack, r);
        m->virtual_us = T0_US + PLAY_US;
        n[native] = audio_read(m, native ? b : a, N);
        if (!native) {
            CHECK(pic_irq_pending(m, PIC_IRQ_SB), "emulated: the block end raised IRQ 7");
            CHECK(sb_time_constant(m) == TC_100US, "emulated: time constant %02X", sb_time_constant(m));
        } else {
            host_audio_poll(m);
            CHECK(irq7(m), "native: the block end raised IRQ 7");
        }
        finish(m);
    }
    CHECK(n[0] == n[1] && n[0] >= N - 1, "frames emulated %d native %d", n[0], n[1]);
    CHECK(memcmp(a, b, (size_t)n[0] * 2 * sizeof(int16_t)) == 0, "the host PCM renders what the DSP renders");
    CHECK(energy(a, n[0] / 2) > 0, "the block is silent");
}

/* A single-cycle block ends BLOCK periods after the start: IRQ 7 then, not before, and only once. */
static void test_pcm_irq_at_block_end(void)
{
    Machine *m = boot_native();
    m->virtual_us = T0_US;
    host_audio_sb_reset(m);
    host_audio_pcm_start(m, BUF, BLOCK, RATE_10K, false);
    uint64_t end = T0_US + (uint64_t)BLOCK * PERIOD_US;
    CHECK(host_audio_next_due_us(m) == end, "next due %llu, want %llu", (unsigned long long)host_audio_next_due_us(m),
          (unsigned long long)end);
    CHECK(!irq_at(m, end - 1), "no IRQ 1 us before the block end");
    CHECK(irq_at(m, end), "IRQ 7 at the block end");
    CHECK(host_audio_next_due_us(m) == UINT64_MAX, "single cycle: nothing more due");
    CHECK(!irq_at(m, end + 3 * BLOCK * PERIOD_US), "single cycle stops");
    finish(m);
}

static void test_pcm_auto_init_and_stop(void)
{
    enum { AUTO_BLOCK = 100 };
    Machine *m = boot_native();
    m->virtual_us = T0_US;
    host_audio_pcm_start(m, BUF, AUTO_BLOCK, RATE_10K, true);
    for (int k = 1; k <= 3; k++) {
        uint64_t end = T0_US + (uint64_t)k * AUTO_BLOCK * PERIOD_US;
        CHECK(!irq_at(m, end - 1), "auto-init block %d: no IRQ early", k);
        CHECK(irq_at(m, end), "auto-init block %d: IRQ 7 at its end", k);
    }
    m->virtual_us = T0_US + 35 * AUTO_BLOCK * PERIOD_US / 10;
    host_audio_pcm_stop(m);
    CHECK(host_audio_next_due_us(m) == UINT64_MAX, "stopped: nothing due");
    CHECK(!irq_at(m, T0_US + 10 * AUTO_BLOCK * PERIOD_US), "stopped: no IRQ");

    /* The mask alone stalls the block: no block end while it holds. */
    host_audio_pcm_start(m, BUF, AUTO_BLOCK, RATE_10K, true);
    host_audio_pcm_mask(m);
    CHECK(host_audio_next_due_us(m) == UINT64_MAX, "masked: nothing due");
    CHECK(!irq_at(m, m->virtual_us + 5 * AUTO_BLOCK * PERIOD_US), "masked: no IRQ");
    finish(m);
}

static void test_pcm_rate(void)
{
    CHECK(host_audio_sb_rate(TC_100US) == RATE_10K, "rate of %02X: %u", TC_100US, host_audio_sb_rate(TC_100US));
    CHECK(host_audio_sb_rate(0xD2) == 21739, "rate of D2h: %u", host_audio_sb_rate(0xD2));
    Machine *m = boot_native();
    m->virtual_us = T0_US;
    host_audio_sb_time_constant(m, 0xD2);
    host_audio_pcm_start(m, BUF, 100, 0, false); /* rate 0: the time constant set before */
    CHECK(host_audio_next_due_us(m) == T0_US + 100 * 46, "D2h: 46 us a byte, due %llu",
          (unsigned long long)host_audio_next_due_us(m));
    host_audio_pcm_start(m, BUF, 100, 22050, false); /* 45.35 us: time constant 211, 45 us */
    CHECK(host_audio_next_due_us(m) == T0_US + 100 * 45, "22050 Hz: due %llu",
          (unsigned long long)host_audio_next_due_us(m));
    CHECK(audio_pcm(m)->time_constant == 211, "22050 Hz: time constant %u", audio_pcm(m)->time_constant);
    CHECK(host_audio_pcm_start(m, BUF, 100, 1000, false) == -1, "1 kHz has no time constant");
    CHECK(host_audio_next_due_us(m) == T0_US + 100 * 45, "a refused start changes nothing");
    host_audio_pcm_start(m, BUF, 0, RATE_10K, false); /* 0 is 64K */
    CHECK(host_audio_next_due_us(m) == T0_US + 65536ull * PERIOD_US, "64K block due %llu",
          (unsigned long long)host_audio_next_due_us(m));
    finish(m);
}

static void test_dsp_answers(void)
{
    for (int native = 0; native < 2; native++) {
        Machine *m = native ? boot_native() : boot();
        const char *how = native ? "native" : "emulated";
        CHECK(host_audio_sb_reset(m) == 0xAA, "%s reset", how);
        CHECK(host_audio_sb_identify(m, 0xAA) == 0x55, "%s identify", how);
        CHECK(host_audio_sb_version(m) == 0x0201, "%s version %04X", how, host_audio_sb_version(m));
        CHECK(host_audio_sb_ack(m) == HOST_AUDIO_SB_STATUS_READY, "%s ack", how);
        finish(m);
    }
}

/* A block that goes to the emulated DSP and DMA a third of the way through and comes back at two thirds
 * renders exactly as one played natively throughout, and ends with IRQ 7 at the same time. */
static void test_handoff_round_trip(void)
{
    enum { N = RATE * 4 / 100 };
    static const uint64_t MARKS[] = { 30000, 60000, 120000 };
    static int16_t out[2][3][N * 2];
    int got[2][3];
    for (int hand = 0; hand < 2; hand++) {
        Machine *m = boot_native();
        fill_square(m);
        m->virtual_us = T0_US;
        host_audio_sb_speaker(m, true);
        host_audio_pcm_start(m, BUF, BLOCK, RATE_10K, false);
        for (int k = 0; k < 3; k++) {
            m->virtual_us = T0_US + MARKS[k];
            if (k == 2) {
                host_audio_poll(m);
                CHECK(irq7(m), "hand-off %d: IRQ 7 by the end", hand);
            }
            got[hand][k] = audio_read(m, out[hand][k], N);
            if (!hand)
                continue;
            if (k == 0) {
                host_audio_to_devices(m);
                m->native = false;
                port_out(m, 0x0C, 0, 1);
                uint16_t lo = (uint16_t)port_in(m, 0x03, 1), count = (uint16_t)(lo | port_in(m, 0x03, 1) << 8);
                CHECK(count == BLOCK - 1 - 300, "on the DMA controller: count %u after 300 bytes", count);
            } else if (k == 1) {
                CHECK(!pic_irq_pending(m, PIC_IRQ_SB), "no block end on the emulated side");
                host_audio_from_devices(m);
                CHECK(port_in(m, 0x0F, 1) & 0x02, "channel 1 masked after the hand-back");
                m->native = true;
            }
        }
        finish(m);
    }
    for (int k = 0; k < 3; k++) {
        CHECK(got[0][k] == got[1][k], "part %d: %d and %d frames", k, got[0][k], got[1][k]);
        CHECK(memcmp(out[0][k], out[1][k], (size_t)got[0][k] * 2 * sizeof(int16_t)) == 0,
              "part %d renders the same across the hand-off", k);
    }
    CHECK(energy(out[1][1], got[1][1]) > 0, "the emulated part is silent");
}

/* ---- emulated mode: the port sequences, canonicalised as a shadow run would ---- */

static SemTrace captured;

static uint32_t capture_in(Machine *m, uint16_t port, int size)
{
    uint32_t v = port == 0x22A ? 0xAA : 0xFF;
    sem_trace_port(&captured, port, size, v, false);
    return v;
}

static void capture_out(Machine *m, uint16_t port, uint32_t value, int size)
{
    sem_trace_port(&captured, port, size, value, true);
}

static void expect_event(size_t k, SemKind kind, uint32_t a, uint32_t b)
{
    const SemEvent *e = k < captured.nsem ? &captured.sem[k] : NULL;
    CHECK(e && e->kind == kind && e->a == a && e->b == b, "event %zu: want kind %d (%X, %X), got kind %d (%X, %X)",
          k, kind, a, b, e ? (int)e->kind : -1, e ? e->a : 0, e ? e->b : 0);
}

static void test_port_sequences_are_events(void)
{
    static const uint16_t PORTS[] = { 0x02, 0x03, 0x0A, 0x0B, 0x0C, 0x83, 0x226, 0x22A, 0x22C, 0x22E };
    Machine *m = boot();
    for (size_t i = 0; i < sizeof PORTS / sizeof PORTS[0]; i++)
        port_register(m, PORTS[i], PORTS[i], capture_in, capture_out);
    memset(&captured, 0, sizeof captured);
    sem_trace_init(&captured);
    host_audio_sb_reset(m);
    host_audio_sb_identify(m, 0x5A);
    host_audio_sb_version(m);
    host_audio_sb_speaker(m, true);
    host_audio_sb_time_constant(m, TC_100US);
    host_audio_pcm_start(m, 0x12345, 1000, 0, false);
    host_audio_pcm_start(m, 0x30000, 512, 22050, true);
    host_audio_pcm_stop(m);
    host_audio_pcm_mask(m);
    host_audio_sb_ack(m);
    host_audio_sb_speaker(m, false);
    sem_trace_finish(&captured);
    size_t k = 0;
    expect_event(k++, SEM_SB_RESET, 0, 0);
    expect_event(k++, SEM_SB_DATA, 0, 0xAA);
    expect_event(k++, SEM_SB_IDENT, 0xE0, 0x5A);
    expect_event(k++, SEM_SB_DATA, 0, 0xAA);
    expect_event(k++, SEM_SB_IDENT, 0xE1, 0);
    expect_event(k++, SEM_SB_DATA, 0, 0xAA);
    expect_event(k++, SEM_SB_DATA, 0, 0xAA);
    expect_event(k++, SEM_SB_SPEAKER, 1, 0);
    expect_event(k++, SEM_SB_TIME_CONSTANT, TC_100US, 0);
    expect_event(k++, SEM_DMA_MASK, 1, 1);
    expect_event(k++, SEM_DMA_PROGRAM, 0x49012345, 999);
    expect_event(k++, SEM_DMA_MASK, 1, 0);
    expect_event(k++, SEM_SB_PCM_START, 0, 999);
    expect_event(k++, SEM_SB_TIME_CONSTANT, 211, 0);
    expect_event(k++, SEM_DMA_MASK, 1, 1);
    expect_event(k++, SEM_DMA_PROGRAM, 0x59030000, 511);
    expect_event(k++, SEM_DMA_MASK, 1, 0);
    expect_event(k++, SEM_SB_BLOCK_SIZE, 511, 0);
    expect_event(k++, SEM_SB_PCM_START, 1, 511);
    expect_event(k++, SEM_SB_PCM_STOP, 0, 0);
    expect_event(k++, SEM_DMA_MASK, 1, 1);
    expect_event(k++, SEM_DMA_MASK, 1, 1);
    expect_event(k++, SEM_SB_SPEAKER, 0, 0);
    CHECK(captured.nsem == k && captured.nraw == 0, "%zu events (want %zu), %zu raw", captured.nsem, k, captured.nraw);
    sem_trace_free(&captured);
    finish(m);
}

int main(void)
{
    test_timers_match_ports();
    test_write_sounds_like_ports();
    test_replay_does_not_write();
    test_native_safepoint_charges();
    test_pcm_renders_like_sb();
    test_pcm_irq_at_block_end();
    test_pcm_auto_init_and_stop();
    test_pcm_rate();
    test_dsp_answers();
    test_handoff_round_trip();
    test_port_sequences_are_events();
    if (failures) {
        fprintf(stderr, "test_host_audio: %d failure(s)\n", failures);
        return 1;
    }
    printf("host audio ok\n");
    return 0;
}
