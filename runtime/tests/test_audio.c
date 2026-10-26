/* Sound tests: AdLib timer detection as a driver runs it, an OPL2 sine note and a PC speaker
 * tone measured through the WAV writer, and the paced reader's latency control. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "devices.h"

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            failures++;                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                      \
            fputc('\n', stderr);                               \
        }                                                      \
    } while (0)

enum {
    CODE_SEG = 0x0700, STACK_SEG = 0x0600, STACK_TOP = 0x0FFE, RESULT = 0x500,
    OPL_ADDR = 0x388, OPL_DATA = 0x389,
    RATE = AUDIO_DEFAULT_RATE, TONE_MS = 1000, STEP_MS = 100, SETTLE_MS = 50,
    WAV_HEADER = 44,
};

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

/* The classic AdLib probe: reset the timers, read the status, start timer 1 at FFh (one 80 us
 * tick), poll the status `polls` times, read it again. Results land at 0:0500 and 0:0501. */
static void run_detection(Machine *m, uint8_t polls)
{
    const uint8_t code[] = {
        0xBA, 0x88, 0x03,                         /* mov dx,388h */
        0xB0, 0x04, 0xEE, 0x42, 0xB0, 0x60, 0xEE, /* reg 4 <- 60h: mask both timers */
        0x4A, 0xB0, 0x04, 0xEE, 0x42, 0xB0, 0x80, 0xEE, /* reg 4 <- 80h: reset flags */
        0x4A, 0xEC, 0xA2, 0x00, 0x05,             /* status -> [500h] */
        0xB0, 0x02, 0xEE, 0x42, 0xB0, 0xFF, 0xEE, /* reg 2 <- FFh */
        0x4A, 0xB0, 0x04, 0xEE, 0x42, 0xB0, 0x21, 0xEE, /* reg 4 <- 21h: start timer 1 */
        0x4A, 0xB9, polls, 0x00,                  /* mov cx,polls */
        0xEC, 0xE2, 0xFD,                         /* in al,dx; loop */
        0xA2, 0x01, 0x05, 0xF4,                   /* status -> [501h]; hlt */
    };
    memcpy(m->mem + real_addr(CODE_SEG, 0), code, sizeof code);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    cpu_set_seg(&m->cpu, S_DS, 0);
    m->cpu.eip = 0;
    SP = STACK_TOP;
    m->cpu.flags = 0x0002; /* interrupts off: only the probe runs */
    machine_run_for(m, 1000);
}

/* Each poll costs one ISA cycle (1 us); 78 polls end inside the first 100 us slice, past
 * timer 1's 80.4 us, so this passes only if time advances within a slice. */
static void test_detection(void)
{
    Machine *m = boot();
    run_detection(m, 78);
    CHECK((m->mem[RESULT] & 0xE0) == 0x00, "status after reset %02X", m->mem[RESULT]);
    CHECK((m->mem[RESULT + 1] & 0xE0) == 0xC0, "status after timer 1 %02X", m->mem[RESULT + 1]);
    CHECK((m->mem[RESULT + 1] & 0x07) == 0x06, "OPL2 id bits %02X", m->mem[RESULT + 1]);
    CHECK(m->virtual_us <= 1000, "probe finished in the first slices");
    finish(m);

    m = boot();
    run_detection(m, 40);
    CHECK((m->mem[RESULT + 1] & 0xE0) == 0x00, "timer 1 not yet expired after ~55 us: %02X", m->mem[RESULT + 1]);
    finish(m);
}

static void opl_write(Machine *m, uint8_t reg, uint8_t v)
{
    port_out(m, OPL_ADDR, reg, 1);
    port_out(m, OPL_DATA, v, 1);
}

/* Channel 0, modulator silenced, carrier a sustained sine at 440 Hz (F-number 580, block 4). */
static void opl_play_a440(Machine *m)
{
    static const uint8_t REGS[][2] = {
        { 0x20, 0x01 }, { 0x23, 0x21 }, { 0x40, 0x3F }, { 0x43, 0x00 }, { 0x60, 0xF0 },
        { 0x63, 0xF0 }, { 0x80, 0x00 }, { 0x83, 0x00 }, { 0xC0, 0x00 }, { 0xA0, 0x44 },
        { 0xB0, 0x32 },
    };
    for (size_t i = 0; i < sizeof REGS / sizeof REGS[0]; i++)
        opl_write(m, REGS[i][0], REGS[i][1]);
}

static void speaker_play_1khz(Machine *m)
{
    enum { COUNT = PIT_CLOCK_HZ / 1000 };
    port_out(m, 0x43, 0xB6, 1);
    port_out(m, 0x42, COUNT & 0xFF, 1);
    port_out(m, 0x42, COUNT >> 8, 1);
    port_out(m, 0x61, 0x03, 1);
}

typedef struct Tone { double hz, rms; } Tone;

/* Rising zero crossings of the left channel after the attack, and its RMS. */
static Tone measure(const int16_t *frames, int count)
{
    int start = RATE * SETTLE_MS / 1000, crossings = 0;
    double sum = 0;
    for (int i = start; i < count; i++) {
        double v = frames[i * AUDIO_CHANNELS];
        sum += v * v;
        if (i > start && frames[(i - 1) * AUDIO_CHANNELS] < 0 && v >= 0)
            crossings++;
    }
    double seconds = (double)(count - start) / RATE;
    return (Tone){ crossings / seconds, sqrt(sum / (count - start)) };
}

/* Renders TONE_MS through the WAV writer and reads the samples back from the file. */
static Tone record(void (*play)(Machine *), const char *path)
{
    enum { FRAMES = RATE * TONE_MS / 1000 };
    static int16_t frames[FRAMES * AUDIO_CHANNELS];
    Machine *m = boot();
    m->cpu.halted = true;
    m->cpu.flags = 0x0002;
    audio_enable(m, true);
    WavWriter *w = wav_open(path, RATE);
    CHECK(w, "open %s", path);
    play(m);
    int n = 0;
    for (int ms = 0; ms < TONE_MS; ms += STEP_MS) {
        machine_run_for(m, STEP_MS * 1000);
        n += audio_read(m, frames + n * AUDIO_CHANNELS, FRAMES - n);
    }
    CHECK(n >= FRAMES - 1, "rendered %d frames for %d ms", n, TONE_MS);
    CHECK(w && wav_write(w, frames, n) == 0 && wav_close(w) == 0, "write %s", path);
    finish(m);

    FILE *f = fopen(path, "rb");
    memset(frames, 0, sizeof frames);
    size_t got = 0;
    if (f && fseek(f, WAV_HEADER, SEEK_SET) == 0)
        got = fread(frames, sizeof(int16_t) * AUDIO_CHANNELS, (size_t)n, f);
    CHECK(got == (size_t)n, "read back %zu of %d frames", got, n);
    if (f)
        fclose(f);
    remove(path);
    return measure(frames, n);
}

static void test_tones(void)
{
    Tone a = record(opl_play_a440, "test_audio_opl.wav");
    CHECK(fabs(a.hz - 440.0) < 2.0, "OPL2 note at %.1f Hz, want 440", a.hz);
    CHECK(a.rms > 1000, "OPL2 note RMS %.0f", a.rms);
    Tone s = record(speaker_play_1khz, "test_audio_speaker.wav");
    CHECK(fabs(s.hz - 1000.0) < 3.0, "speaker tone at %.1f Hz, want 1000", s.hz);
    CHECK(s.rms > 1000, "speaker RMS %.0f", s.rms);
    printf("OPL2 %.1f Hz rms %.0f, speaker %.1f Hz rms %.0f\n", a.hz, a.rms, s.hz, s.rms);
}

/* The tap counts what each source renders, by name. */
static void test_tap(void)
{
    Machine *m = boot();
    audio_enable(m, true);
    audio_tap_enable(m, true);
    speaker_play_1khz(m);
    const uint8_t spin[] = { 0xEB, 0xFE };
    memcpy(m->mem + real_addr(CODE_SEG, 0), spin, sizeof spin);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    m->cpu.eip = 0;
    m->cpu.flags = 0x0002;
    machine_run_for(m, 300 * 1000);
    audio_sync(m);
    CHECK(audio_tap_peak(m, "speaker") > 1000, "speaker tap peak %d", (int)audio_tap_peak(m, "speaker"));
    CHECK(audio_tap_peak(m, "opl") == 0, "opl tap peak %d, want 0", (int)audio_tap_peak(m, "opl"));
    CHECK(audio_tap_peak(m, "sb") == 0, "sb tap peak %d, want 0", (int)audio_tap_peak(m, "sb"));
    CHECK(audio_tap_peak(m, "nothing") == 0, "unknown name");
    finish(m);
}

/* F2h (IRQ test) raises IRQ 7 a little after the command, not inside it: drivers that probe the IRQ clear
 * their flag after sending the command and wait for the interrupt (GOLF.EXE's Sound Blaster detection). */
static void test_sb_irq_test(void)
{
    Machine *m = boot();
    port_out(m, SB_BASE + 6, 1, 1);
    port_out(m, SB_BASE + 6, 0, 1);
    port_out(m, 0x21, 0x00, 1); /* unmask every IRQ */
    const uint8_t spin[] = { 0xEB, 0xFE }; /* jmp $ with interrupts off: time passes, nothing is taken */
    memcpy(m->mem + real_addr(CODE_SEG, 0), spin, sizeof spin);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    m->cpu.eip = 0;
    m->cpu.flags = 0x0002;
    port_out(m, SB_BASE + 0xC, 0xF2, 1);
    CHECK(!m->irq_pending, "the IRQ arrived inside the command");
    machine_run_for(m, 500);
    CHECK(m->irq_pending, "the IRQ never arrived");
    finish(m);
}

/* A device clock 0.3% fast against emulated time: the queue must settle near the target
 * without underruns once primed, and never exceed the latency cap. */
static void test_pacing(void)
{
    enum { FRAME_US = 16667, FRAMES = 1800, CAP = AUDIO_RING_FRAMES * 2 };
    static int16_t buf[CAP * AUDIO_CHANNELS];
    Machine *m = boot();
    m->cpu.halted = true;
    m->cpu.flags = 0x0002;
    audio_enable(m, true);
    double queued = 0, consume = RATE * (FRAME_US / 1e6) * 1.003;
    int underruns = 0, max_queued = 0;
    for (int i = 0; i < FRAMES; i++) {
        machine_run_for(m, FRAME_US);
        queued = queued > consume ? queued - consume : 0;
        if (i > 60 && queued == 0)
            underruns++;
        queued += audio_read_paced(m, buf, CAP, (int)queued);
        max_queued = (int)queued > max_queued ? (int)queued : max_queued;
    }
    int target = RATE * AUDIO_TARGET_MS / 1000;
    CHECK(underruns == 0, "%d underruns", underruns);
    CHECK(max_queued <= RATE * AUDIO_MAX_LATENCY_MS / 1000, "queue peaked at %d frames", max_queued);
    CHECK(fabs(queued - target) < target / 2.0, "queue settled at %.0f frames, target %d", queued, target);
    finish(m);
}

int main(void)
{
    test_detection();
    test_tones();
    test_pacing();
    test_tap();
    test_sb_irq_test();
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("test_audio: all passed\n");
    return 0;
}
