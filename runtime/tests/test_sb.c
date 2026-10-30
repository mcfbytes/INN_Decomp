/* Sound Blaster tests: DSP commands, single-cycle and auto-init DMA, block-end IRQs with no sink,
 * pause, stalls on a masked channel, IRQs held by irq_hold, the rendered samples, and the snapshot
 * that shadow and synthetic runs restore. */
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
    CODE_SEG = 0x0700, STACK_SEG = 0x0600, STACK_TOP = 0x0FFE, HANDLER_OFF = 0x100,
    IRQ_COUNT = 0x510, IRQ7_VECTOR = 0x0F,
    DSP_RESET = 0x226, DSP_READ = 0x22A, DSP_WRITE = 0x22C, DSP_STATUS = 0x22E,
    BUF = 0x20000, BUF_PAGE = 0x02,
    TC_100US = 256 - 100, /* one byte every 100 us: 10 kHz */
    MODE_SINGLE_READ_CH1 = 0x49, MODE_AUTO_READ_CH1 = 0x59,
};

/* sti; hlt; jmp back to hlt. IRQ7: ack the DSP, count, EOI. */
static const uint8_t IDLE[] = { 0xFB, 0xF4, 0xEB, 0xFD };
static const uint8_t HANDLER[] = {
    0x50, 0x52, 0xBA, 0x2E, 0x02, 0xEC,  /* push ax; push dx; mov dx,22Eh; in al,dx */
    0xFF, 0x06, 0x10, 0x05,              /* inc word [0510h] */
    0xB0, 0x20, 0xE6, 0x20, 0x5A, 0x58, 0xCF, /* EOI; pop dx; pop ax; iret */
};

static Machine *boot(void)
{
    Machine *m = machine_create();
    bios_init(m);
    memcpy(m->mem + real_addr(CODE_SEG, 0), IDLE, sizeof IDLE);
    memcpy(m->mem + real_addr(CODE_SEG, HANDLER_OFF), HANDLER, sizeof HANDLER);
    mem_wr16(m, IRQ7_VECTOR * 4, HANDLER_OFF);
    mem_wr16(m, IRQ7_VECTOR * 4 + 2, CODE_SEG);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    cpu_set_seg(&m->cpu, S_DS, 0);
    m->cpu.eip = 0;
    SP = STACK_TOP;
    port_out(m, 0x21, port_in(m, 0x21, 1) & 0x7F, 1); /* unmask IRQ7 */
    return m;
}

static void finish(Machine *m)
{
    bios_shutdown(m);
    machine_destroy(m);
}

static uint16_t irqs(Machine *m) { return mem_rd16(m, IRQ_COUNT); }

static void dsp(Machine *m, uint8_t v) { port_out(m, DSP_WRITE, v, 1); }

static uint8_t dsp_read(Machine *m)
{
    CHECK(port_in(m, DSP_STATUS, 1) & 0x80, "DSP has data");
    return (uint8_t)port_in(m, DSP_READ, 1);
}

static void dma_program(Machine *m, uint8_t mode, uint16_t count)
{
    port_out(m, 0x0A, 0x05, 1); /* mask channel 1 */
    port_out(m, 0x0C, 0, 1);
    port_out(m, 0x0B, mode, 1);
    port_out(m, 0x02, BUF & 0xFF, 1);
    port_out(m, 0x02, BUF >> 8 & 0xFF, 1);
    port_out(m, 0x83, BUF_PAGE, 1);
    port_out(m, 0x03, count & 0xFF, 1);
    port_out(m, 0x03, count >> 8, 1);
    port_out(m, 0x0A, 0x01, 1);
}

static uint16_t dma_count(Machine *m)
{
    port_out(m, 0x0C, 0, 1);
    uint16_t lo = (uint16_t)port_in(m, 0x03, 1);
    return (uint16_t)(lo | port_in(m, 0x03, 1) << 8);
}

static void run_ms(Machine *m, int ms) { machine_run_for(m, (uint64_t)ms * 1000); }

static void test_commands(void)
{
    Machine *m = boot();
    CHECK(!(port_in(m, DSP_STATUS, 1) & 0x80), "no data before reset");
    port_out(m, DSP_RESET, 1, 1);
    port_out(m, DSP_RESET, 0, 1);
    CHECK(dsp_read(m) == 0xAA, "reset answers AAh");
    CHECK(!(port_in(m, DSP_WRITE, 1) & 0x80), "ready for commands");
    dsp(m, 0xE1);
    uint8_t major = dsp_read(m), minor = dsp_read(m);
    CHECK(major == 2 && minor == 1, "version %d.%02d", major, minor);
    dsp(m, 0xE0);
    dsp(m, 0xAA);
    CHECK(dsp_read(m) == 0x55, "E0 returns the inverted byte");
    dsp(m, 0xD8);
    CHECK(dsp_read(m) == 0x00, "speaker off after reset");
    dsp(m, 0xD1);
    dsp(m, 0xD8);
    CHECK(dsp_read(m) == 0xFF, "speaker on");
    dsp(m, 0xE4);
    dsp(m, 0x5A);
    dsp(m, 0xE8);
    CHECK(dsp_read(m) == 0x5A, "test register");
    CHECK((port_in(m, 0x220, 1) & 0x07) == 0x06 && (port_in(m, 0x228, 1) & 0x07) == 0x06,
          "OPL2 status at 220h and 228h");

    dsp(m, 0xF2);
    uint8_t irr, isr, imr;
    pic_get_state(m, 0, &irr, &isr, &imr);
    CHECK(!(irr & 0x80), "F2's IRQ comes a few us after the command, not with it"); /* GOLF clears its flag after the OUT */
    run_ms(m, 1);
    CHECK(irqs(m) == 1, "F2 raises one IRQ, got %d", irqs(m));
    m->irq_hold = true; /* as during a shadow run */
    dsp(m, 0xF2);
    run_ms(m, 1);
    CHECK(irqs(m) == 1, "IRQ held while irq_hold is set");
    m->irq_hold = false;
    run_ms(m, 1);
    CHECK(irqs(m) == 2, "held IRQ delivered afterwards, got %d", irqs(m));
    finish(m);
}

/* 1000 bytes at 10 kHz take 100 ms; the IRQ comes from the ticker with no sink rendering. */
static void test_single_cycle(void)
{
    Machine *m = boot();
    for (int i = 0; i < 1000; i++)
        m->mem[BUF + i] = (uint8_t)i;
    dma_program(m, MODE_SINGLE_READ_CH1, 999);
    dsp(m, 0x40);
    dsp(m, TC_100US);
    dsp(m, 0x14);
    dsp(m, 999 & 0xFF);
    dsp(m, 999 >> 8);
    run_ms(m, 50);
    uint16_t left = dma_count(m);
    CHECK(left >= 498 && left <= 502, "half way: DMA count %u", left);
    CHECK(irqs(m) == 0, "no IRQ before the block ends");
    run_ms(m, 51);
    CHECK(irqs(m) == 1, "one IRQ at the block end, got %d", irqs(m));
    CHECK(dma_count(m) == 0xFFFF, "count wrapped to FFFFh at terminal count");
    CHECK(port_in(m, 0x08, 1) & 0x02, "status reports TC on channel 1");
    CHECK(!(port_in(m, 0x08, 1) & 0x02), "reading the status clears TC");
    CHECK(port_in(m, 0x0F, 1) & 0x02, "single-cycle channel masked itself");
    run_ms(m, 100);
    CHECK(irqs(m) == 1, "single cycle stops, got %d", irqs(m));
    finish(m);
}

static void test_auto_init(void)
{
    Machine *m = boot();
    dma_program(m, MODE_AUTO_READ_CH1, 199);
    dsp(m, 0x40);
    dsp(m, TC_100US);
    dsp(m, 0x48);
    dsp(m, 99);
    dsp(m, 0);
    dsp(m, 0x1C);
    run_ms(m, 1000);
    CHECK(irqs(m) >= 99 && irqs(m) <= 100, "100-byte blocks for 1 s: %d IRQs", irqs(m));

    dsp(m, 0xD0);
    run_ms(m, 1); /* an IRQ raised by the last tick is taken now */
    uint16_t paused = irqs(m);
    run_ms(m, 100);
    CHECK(irqs(m) == paused, "paused: %d -> %d", paused, irqs(m));
    dsp(m, 0xD4);
    run_ms(m, 100);
    CHECK(irqs(m) >= paused + 9 && irqs(m) <= paused + 10, "continued: %d -> %d", paused, irqs(m));

    run_ms(m, 1);
    uint16_t at_exit = irqs(m);
    dsp(m, 0xDA);
    run_ms(m, 100);
    CHECK(irqs(m) == at_exit + 1, "DAh finishes the current block only: %d -> %d", at_exit, irqs(m));
    CHECK(!(port_in(m, 0x0F, 1) & 0x02), "auto-init channel stays unmasked");
    finish(m);
}

/* The DSP waits for DMA: nothing moves while channel 1 is masked. */
static void test_masked_stall(void)
{
    Machine *m = boot();
    dma_program(m, MODE_SINGLE_READ_CH1, 99);
    port_out(m, 0x0A, 0x05, 1);
    dsp(m, 0x40);
    dsp(m, TC_100US);
    dsp(m, 0x14);
    dsp(m, 99);
    dsp(m, 0);
    run_ms(m, 100);
    CHECK(irqs(m) == 0 && dma_count(m) == 99, "stalled while masked: %d IRQs, count %u", irqs(m),
          dma_count(m));
    port_out(m, 0x0A, 0x01, 1);
    run_ms(m, 12);
    CHECK(irqs(m) == 1, "finishes 10 ms after unmasking, got %d", irqs(m));
    finish(m);
}

/* A 500 Hz square wave of 40h/C0h bytes renders near +-16384 and fades after the block end. */
static void test_render(void)
{
    enum { BYTES = 1000, FRAMES = AUDIO_DEFAULT_RATE / 5 };
    static int16_t out[FRAMES * AUDIO_CHANNELS];
    Machine *m = boot();
    audio_enable(m, true);
    for (int i = 0; i < BYTES; i++)
        m->mem[BUF + i] = i / 10 % 2 ? 0xC0 : 0x40;
    dma_program(m, MODE_SINGLE_READ_CH1, BYTES - 1);
    dsp(m, 0xD1);
    dsp(m, 0x40);
    dsp(m, TC_100US);
    dsp(m, 0x14);
    dsp(m, (BYTES - 1) & 0xFF);
    dsp(m, (BYTES - 1) >> 8);
    run_ms(m, 200);
    int n = audio_read(m, out, FRAMES);
    CHECK(n >= FRAMES - 1, "rendered %d frames", n);
    int loud = 0, quiet_tail = 0, crossings = 0;
    for (int i = 0; i < n; i++) {
        int v = out[i * AUDIO_CHANNELS];
        if (i < n / 2 && (v > 14000 || v < -14000))
            loud++;
        if (i > n * 9 / 10 && v > -500 && v < 500)
            quiet_tail++;
        if (i > 0 && i < n / 2 && (v > 0) != (out[(i - 1) * AUDIO_CHANNELS] > 0))
            crossings++;
    }
    CHECK(loud > n / 2 * 9 / 10, "%d of %d frames at full level while playing", loud, n / 2);
    CHECK(crossings >= 95 && crossings <= 101, "%d sign changes in 100 ms, want about 100", crossings);
    CHECK(quiet_tail >= n / 10 - 1, "the held last byte fades after the block: %d frames quiet", quiet_tail);
    CHECK(irqs(m) == 1, "one IRQ while rendering, got %d", irqs(m));
    finish(m);
}

/* The DSP and DMA state saved in the middle of a block (and of a DSP command) and loaded again after the
 * block ended: the channel is unmasked with its count as it was, the DSP finishes the rest of the block
 * and the command, and saving again gives the same bytes. */
static void test_snapshot_round_trip(void)
{
    Machine *m = boot();
    dma_program(m, MODE_SINGLE_READ_CH1, 999);
    dsp(m, 0x40);
    dsp(m, TC_100US);
    dsp(m, 0x14);
    dsp(m, 999 & 0xFF);
    dsp(m, 999 >> 8);
    run_ms(m, 50);
    dsp(m, 0xE0); /* identify, its argument still to come */
    size_t size = sound_snapshot_size();
    uint8_t *mid = malloc(size), *again = malloc(size);
    sound_snapshot_save(m, mid);
    uint16_t count_mid = dma_count(m);
    sound_snapshot_save(m, mid); /* the count read moved the flip-flop: save after it */
    dsp(m, 0x0F);
    CHECK(dsp_read(m) == 0xF0, "identify answers before the load");
    run_ms(m, 60);
    CHECK(irqs(m) == 1, "block ended before the load, %d IRQs", irqs(m));
    CHECK(port_in(m, 0x0F, 1) & 0x02, "channel masked at terminal count");

    sound_snapshot_load(m, mid);
    sound_snapshot_save(m, again);
    CHECK(memcmp(mid, again, size) == 0, "save after load gives the same bytes");
    DmaChannelState c;
    dma_channel_get(m, 1, &c); /* no sync: a port read would let the DSP catch up first */
    CHECK(!c.masked && c.count == count_mid, "after the load: masked %d, count %u (%u when saved)", c.masked, c.count,
          count_mid);
    dsp(m, 0x33);
    CHECK(dsp_read(m) == 0xCC, "the half-received identify completes after the load");
    run_ms(m, 1); /* the rest of the block was due long ago */
    CHECK(irqs(m) == 2, "the loaded block ends again, %d IRQs", irqs(m));
    free(mid);
    free(again);
    finish(m);
}

int main(void)
{
    test_commands();
    test_single_cycle();
    test_auto_init();
    test_masked_stall();
    test_render();
    test_snapshot_round_trip();
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("test_sb: all passed\n");
    return 0;
}
