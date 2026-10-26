/* Semantic port comparison: hand-made port logs canonicalised and compared against event lists. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lift_semantic.h"

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

static void out(SemTrace *t, uint16_t port, uint32_t v) { sem_trace_port(t, port, 1, v, true); }
static void in(SemTrace *t, uint16_t port, uint32_t v) { sem_trace_port(t, port, 1, v, false); }

static void fresh(SemTrace *t)
{
    memset(t, 0, sizeof *t); /* a trace starts zeroed; init only empties a used one */
    sem_trace_init(t);
}

static void expect(const SemTrace *t, size_t k, SemKind kind, uint32_t a, uint32_t b, const char *what)
{
    if (k >= t->nsem) {
        CHECK(false, "%s: no event %zu (have %zu)", what, k, t->nsem);
        return;
    }
    CHECK(t->sem[k].kind == kind && t->sem[k].a == a && t->sem[k].b == b, "%s: event %zu is kind %d (%X, %X)", what, k,
          t->sem[k].kind, t->sem[k].a, t->sem[k].b);
}

static void test_opl(void)
{
    SemTrace t;
    fresh(&t);
    out(&t, 0x388, 0x20);
    in(&t, 0x388, 0);
    out(&t, 0x389, 0x01);
    out(&t, 0x228, 0xB0);
    out(&t, 0x229, 0x31);
    out(&t, 0x389, 0x55); /* data without an index is raw */
    sem_trace_finish(&t);
    CHECK(t.nsem == 2 && t.nraw == 1, "opl: %zu sem, %zu raw", t.nsem, t.nraw);
    expect(&t, 0, SEM_OPL_WRITE, 0x20, 0x01, "opl");
    expect(&t, 1, SEM_OPL_WRITE, 0xB0, 0x31, "opl");
    sem_trace_free(&t);
}

static void test_dac(void)
{
    SemTrace t;
    fresh(&t);
    out(&t, 0x3C8, 5);
    out(&t, 0x3C9, 1); out(&t, 0x3C9, 2); out(&t, 0x3C9, 3);
    out(&t, 0x3C9, 4); out(&t, 0x3C9, 5); out(&t, 0x3C9, 6);
    out(&t, 0x3C7, 9);
    in(&t, 0x3C9, 0x3F); in(&t, 0x3C9, 0x20); in(&t, 0x3C9, 0x10);
    in(&t, 0x3DA, 8);
    out(&t, 0x3C8, 0); out(&t, 0x3C9, 7); /* partial group at the end becomes raw */
    sem_trace_finish(&t);
    CHECK(t.nsem == 3 && t.nraw == 1, "dac: %zu sem, %zu raw", t.nsem, t.nraw);
    expect(&t, 0, SEM_DAC_SET, 5, 0x010203, "dac");
    expect(&t, 1, SEM_DAC_SET, 6, 0x040506, "dac");
    expect(&t, 2, SEM_DAC_GET, 9, 0x3F2010, "dac");
    sem_trace_free(&t);
}

static void test_crtc_pit_pic_kbd(void)
{
    SemTrace t;
    fresh(&t);
    out(&t, 0x3D4, 0x0C); out(&t, 0x3D5, 0x12);
    out(&t, 0x3D4, 0x0D); out(&t, 0x3D5, 0x34);
    out(&t, 0x3D4, 0x0A); out(&t, 0x3D5, 0x00); /* another register: raw */
    out(&t, 0x43, 0x36); out(&t, 0x40, 0x9C); out(&t, 0x40, 0x2E);
    out(&t, 0x43, 0x76); /* channel 1: raw */
    out(&t, 0x20, 0x20); out(&t, 0xA0, 0x20); out(&t, 0x20, 0x0B);
    out(&t, 0x21, 0xFC); in(&t, 0x21, 0xFC); out(&t, 0xA1, 0xFF);
    in(&t, 0x60, 0x1E);
    in(&t, 0x61, 0x4C); out(&t, 0x61, 0xCC); out(&t, 0x61, 0x4C);
    in(&t, 0x61, 0x4C); /* lone access: raw */
    sem_trace_finish(&t);
    size_t k = 0;
    expect(&t, k++, SEM_CRTC_START, 0x1234, 0, "crtc");
    expect(&t, k++, SEM_PIT_DIVISOR, 0, 0x2E9C, "pit");
    expect(&t, k++, SEM_PIC_EOI, 0, 0, "eoi");
    expect(&t, k++, SEM_PIC_EOI, 1, 0, "eoi");
    expect(&t, k++, SEM_PIC_MASK, 0, 0xFC, "mask");
    expect(&t, k++, SEM_PIC_MASK, 1, 0xFF, "mask");
    expect(&t, k++, SEM_KBD_DATA, 0, 0x1E, "kbd");
    expect(&t, k++, SEM_KBD_ACK, 0, 0, "ack");
    CHECK(t.nsem == k, "%zu events", t.nsem);
    /* raw: 3D4/3D5 other register (2), 43h channel 1, 20h 0Bh, lone 61h read */
    CHECK(t.nraw == 5, "%zu raw", t.nraw);
    sem_trace_free(&t);
}

static void test_compare(void)
{
    SemTrace lifted, orig;
    char why[160];
    fresh(&lifted);
    fresh(&orig);
    out(&orig, 0x388, 0x20); out(&orig, 0x389, 0x01);
    out(&orig, 0x20, 0x20);
    in(&orig, 0x60, 0x1E);
    out(&orig, 0x300, 7);
    sem_trace_finish(&orig);

    sem_trace_event(&lifted, SEM_OPL_WRITE, 0x20, 0x01);
    sem_trace_event(&lifted, SEM_PIC_EOI, 0, 0);
    sem_trace_event(&lifted, SEM_KBD_DATA, 0, 0x1E);
    out(&lifted, 0x300, 7);
    sem_trace_finish(&lifted);
    CHECK(sem_trace_compare(&lifted, &orig, why, sizeof why), "equal traces differ: %s", why);

    /* replayed reads: the n-th read of a kind returns the original's value */
    uint32_t b = 0;
    CHECK(sem_trace_next_read(&orig, SEM_KBD_DATA, &b) && b == 0x1E, "kbd read value");
    CHECK(!sem_trace_next_read(&orig, SEM_KBD_DATA, &b), "second kbd read should be missing");

    sem_trace_free(&lifted);
    fresh(&lifted);
    sem_trace_event(&lifted, SEM_OPL_WRITE, 0x20, 0x02);
    sem_trace_event(&lifted, SEM_PIC_EOI, 0, 0);
    sem_trace_event(&lifted, SEM_KBD_DATA, 0, 0x1E);
    out(&lifted, 0x300, 7);
    sem_trace_finish(&lifted);
    CHECK(!sem_trace_compare(&lifted, &orig, why, sizeof why), "different value passed");
    CHECK(strcmp(why, "semantic event 0: lifted opl_write(20, 2), original opl_write(20, 1)") == 0, "message: %s", why);

    sem_trace_free(&lifted);
    fresh(&lifted);
    sem_trace_event(&lifted, SEM_OPL_WRITE, 0x20, 0x01);
    sem_trace_event(&lifted, SEM_PIC_EOI, 0, 0);
    sem_trace_event(&lifted, SEM_KBD_DATA, 0, 0x1E);
    sem_trace_finish(&lifted);
    CHECK(!sem_trace_compare(&lifted, &orig, why, sizeof why), "missing raw access passed");
    CHECK(strcmp(why, "port event 0: lifted none, original out 300 size 1 value 7") == 0, "message: %s", why);

    sem_trace_free(&lifted);
    fresh(&lifted);
    sem_trace_event(&lifted, SEM_OPL_WRITE, 0x20, 0x01);
    sem_trace_event(&lifted, SEM_PIC_EOI, 0, 0);
    sem_trace_event(&lifted, SEM_KBD_DATA, 0, 0x1E);
    sem_trace_event(&lifted, SEM_KBD_ACK, 0, 0);
    out(&lifted, 0x300, 7);
    sem_trace_finish(&lifted);
    CHECK(!sem_trace_compare(&lifted, &orig, why, sizeof why), "extra event passed");
    CHECK(strcmp(why, "semantic event 3: lifted kbd_ack(0, 0), original none") == 0, "message: %s", why);
    sem_trace_free(&lifted);
    sem_trace_free(&orig);
}

static void test_no_run(void)
{
    /* Outside a lifted shadow run the entry points do nothing and reads return the live value. */
    Machine *m = calloc(1, sizeof *m);
    CHECK(m != NULL, "alloc");
    if (!m)
        return;
    lift_semantic_event(m, SEM_PIC_EOI, 0, 0);
    CHECK(lift_semantic_read(m, SEM_KBD_DATA, 0, 0x55) == 0x55, "live read value");
    free(m);
}

/* A Sound Blaster driver's port log (write-ready polls before each DSP byte, the reset with its data
 * poll, identify and version, a single-cycle block on channel 1, an auto-init block with 48h and the
 * high-speed start, halt, continue, speaker, the lone mask at the end and the IRQ acknowledge) becomes
 * SB and DMA events; the polls disappear. */
static void dsp_byte(SemTrace *t, uint8_t v)
{
    in(t, 0x22C, 0x80); /* busy */
    in(t, 0x22C, 0x00);
    out(t, 0x22C, v);
}

static void dma1(SemTrace *t, uint8_t mode, uint32_t linear, uint16_t count, bool page_first)
{
    out(t, 0x0A, 0x05);
    out(t, 0x0C, 0);
    out(t, 0x0B, mode);
    if (page_first)
        out(t, 0x83, linear >> 16);
    out(t, 0x02, linear & 0xFF);
    out(t, 0x02, linear >> 8 & 0xFF);
    out(t, 0x03, count & 0xFF);
    out(t, 0x03, count >> 8);
    if (!page_first)
        out(t, 0x83, linear >> 16);
    out(t, 0x0A, 0x01);
}

static void test_sound_blaster(void)
{
    SemTrace t;
    fresh(&t);
    out(&t, 0x226, 1);
    in(&t, 0x388, 0); /* a delay between the two reset writes */
    out(&t, 0x226, 0);
    in(&t, 0x22E, 0x00);
    in(&t, 0x22E, 0x80);
    in(&t, 0x22A, 0xAA);
    dsp_byte(&t, 0xE0);
    dsp_byte(&t, 0x3C);
    in(&t, 0x22A, 0xC3);
    dsp_byte(&t, 0xE1);
    in(&t, 0x22A, 0x02);
    in(&t, 0x22A, 0x01);
    dsp_byte(&t, 0xD1);
    dsp_byte(&t, 0x40);
    dsp_byte(&t, 0xA5);
    dma1(&t, 0x49, 0x2ABCD, 0x1234, false);
    dsp_byte(&t, 0x14);
    dsp_byte(&t, 0x34);
    dsp_byte(&t, 0x12);
    dma1(&t, 0x59, 0x40000, 0x07FF, true); /* page first: the same program */
    dsp_byte(&t, 0x48);
    dsp_byte(&t, 0xFF);
    dsp_byte(&t, 0x03);
    dsp_byte(&t, 0x90);
    dsp_byte(&t, 0xD0);
    dsp_byte(&t, 0xD4);
    dsp_byte(&t, 0xDA);
    dsp_byte(&t, 0xD3);
    out(&t, 0x0A, 0x05);
    in(&t, 0x22E, 0x7F);
    dsp_byte(&t, 0xF2); /* no meaning here: raw */
    out(&t, 0x0B, 0x4A); /* channel 2: raw */
    sem_trace_finish(&t);
    static const struct { SemKind kind; uint32_t a, b; } WANT[] = {
        { SEM_SB_RESET, 0, 0 }, { SEM_SB_DATA, 0, 0xAA }, { SEM_SB_IDENT, 0xE0, 0x3C }, { SEM_SB_DATA, 0, 0xC3 },
        { SEM_SB_IDENT, 0xE1, 0 }, { SEM_SB_DATA, 0, 2 }, { SEM_SB_DATA, 0, 1 }, { SEM_SB_SPEAKER, 1, 0 },
        { SEM_SB_TIME_CONSTANT, 0xA5, 0 }, { SEM_DMA_MASK, 1, 1 }, { SEM_DMA_PROGRAM, 0x4902ABCD, 0x1234 },
        { SEM_DMA_MASK, 1, 0 }, { SEM_SB_PCM_START, 0, 0x1234 }, { SEM_DMA_MASK, 1, 1 },
        { SEM_DMA_PROGRAM, 0x59040000, 0x07FF }, { SEM_DMA_MASK, 1, 0 }, { SEM_SB_BLOCK_SIZE, 0x3FF, 0 },
        { SEM_SB_PCM_START, 1, 0x3FF }, { SEM_SB_PCM_STOP, 0, 0 }, { SEM_SB_PCM_CONTINUE, 0, 0 },
        { SEM_SB_PCM_STOP, 1, 0 }, { SEM_SB_SPEAKER, 0, 0 }, { SEM_DMA_MASK, 1, 1 },
    };
    size_t n = sizeof WANT / sizeof WANT[0];
    CHECK(t.nsem == n, "sb: %zu events, want %zu", t.nsem, n);
    for (size_t k = 0; k < n; k++)
        expect(&t, k, WANT[k].kind, WANT[k].a, WANT[k].b, "sb");
    CHECK(t.nraw == 2 && t.raw[0].port == 0x22C && t.raw[0].value == 0xF2 && t.raw[1].port == 0x0B,
          "sb: %zu raw (F2h and the channel 2 mode)", t.nraw);

    /* Incomplete groups stay raw: a program without its page, a reset never released, half a command. */
    fresh(&t);
    out(&t, 0x0C, 0);
    out(&t, 0x0B, 0x49);
    out(&t, 0x02, 0x00);
    out(&t, 0x02, 0x10);
    out(&t, 0x03, 0xFF);
    out(&t, 0x03, 0x00);
    out(&t, 0x226, 1);
    out(&t, 0x22C, 0x14);
    out(&t, 0x22C, 0x10);
    sem_trace_finish(&t);
    CHECK(t.nsem == 0 && t.nraw == 9, "sb partial: %zu sem, %zu raw", t.nsem, t.nraw);

    /* A different length or address is a semantic difference. */
    SemTrace o;
    fresh(&o);
    dma1(&o, 0x49, 0x10000, 99, false);
    dsp_byte(&o, 0x14);
    dsp_byte(&o, 99);
    dsp_byte(&o, 0);
    sem_trace_finish(&o);
    fresh(&t);
    dma1(&t, 0x49, 0x10000, 99, false);
    dsp_byte(&t, 0x14);
    dsp_byte(&t, 98);
    dsp_byte(&t, 0);
    sem_trace_finish(&t);
    char why[160];
    CHECK(!sem_trace_compare(&t, &o, why, sizeof why) && strstr(why, "sb_pcm_start"), "length difference: %s", why);
    sem_trace_free(&t);
    sem_trace_free(&o);
}

int main(void)
{
    test_opl();
    test_dac();
    test_crtc_pit_pic_kbd();
    test_compare();
    test_no_run();
    test_sound_blaster();
    if (failures)
        return 1;
    printf("semantic ok\n");
    return 0;
}
