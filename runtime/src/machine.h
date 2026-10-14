/* dosrt: the emulated real-mode PC that every execution tier (interpreter, recompiled, lifted)
 * shares. Design notes: docs/runtime.md. */
#ifndef DOSRT_MACHINE_H
#define DOSRT_MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MEM_CONVENTIONAL_END = 0xA0000,
    MEM_VGA_BASE = 0xA0000,
    MEM_ONE_MEG = 0x100000,
    MEM_SIZE = 0x1000000 + 0x10000, /* 16 MB for XMS plus wrap-around slack */
};

/* Register indices follow the x86 ModRM encoding order. */
enum { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };
enum { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS, S_NONE = 7 };

enum {
    F_CF = 1u << 0, F_PF = 1u << 2, F_AF = 1u << 4, F_ZF = 1u << 6, F_SF = 1u << 7,
    F_TF = 1u << 8, F_IF = 1u << 9, F_DF = 1u << 10, F_OF = 1u << 11,
    F_ARITH = F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF,
};

typedef union GpReg {
    uint32_t e;
    uint16_t w;
    struct { uint8_t l, h; } b;
} GpReg;

typedef struct Cpu {
    GpReg r[8];
    uint16_t seg[6];
    uint32_t seg_base[6]; /* seg << 4, cached */
    uint32_t eip;
    uint32_t flags;
    bool halted;
    bool yielded; /* a callback asked to be retried later (guest is waiting for input) */
    bool stop;    /* leave cpu_run now without ending the time slice (nested call returned) */
    bool irq_inhibit; /* one-instruction shadow after mov ss / pop ss / sti */
    uint64_t instructions;
    uint32_t stall; /* extra instruction slots consumed by slow bus cycles (port I/O) */
} Cpu;

typedef struct Machine Machine;

/* What an HLE callback asks the CPU loop to do next. */
typedef enum CallbackResult {
    CB_DONE,  /* continue after the callback instruction */
    CB_RETRY, /* re-execute the callback later: guest is blocked waiting for input */
} CallbackResult;

typedef CallbackResult (*CallbackFn)(Machine *m);

typedef void (*TickFn)(Machine *m);

typedef enum IoMode { IO_LIVE, IO_RECORD, IO_REPLAY } IoMode;

typedef uint32_t (*PortInFn)(Machine *m, uint16_t port, int size);
typedef void (*PortOutFn)(Machine *m, uint16_t port, uint32_t value, int size);

enum { CALLBACK_MAX = 256, PORT_COUNT = 0x10000 };

/* Callback stubs live in the BIOS segment: FE 38 id CF (callback, iret) at CALLBACK_SEG:id*4. */
enum { CALLBACK_SEG = 0xF000, CALLBACK_OFF_BASE = 0x1000 };

struct Machine {
    Cpu cpu;
    uint8_t *mem;
    bool a20;
    CallbackFn callbacks[CALLBACK_MAX];
    const char *callback_names[CALLBACK_MAX];
    int callback_count;
    PortInFn port_in[PORT_COUNT];
    PortOutFn port_out[PORT_COUNT];

    uint64_t irq_pending; /* bit n: interrupt vector n requested by a device */
    uint64_t virtual_us;  /* emulated time, advanced by the scheduler */
    uint64_t slice_start_insn; /* cpu.instructions when the current slice began */

    uint64_t ips; /* emulated instructions per second */
    uint32_t io_cost_ns; /* ISA bus time charged per port access */
    TickFn tickers[16];
    int ticker_count;
    uint64_t slice_left; /* instructions left in the current time slice */

    uint8_t *exec_hooks; /* one bit per linear address: a lifted function starts here */
    IoMode io_mode;      /* shadow verification records or replays port I/O */
    bool irq_hold;       /* no interrupt delivery while a shadow run compares two paths */
    void *lift;

    void *dos, *bios, *pic, *pit, *vga, *kbd, *mouse, *uart, *ems, *xms, *frontend;
    void *audio, *opl, *speaker, *dma, *sb;
    bool exit_requested;
    int exit_code;
    int trace; /* 0 off, 1 interrupts and DOS calls, 2 every instruction */
};

/* ---- memory ---- */

static inline uint32_t mem_wrap(const Machine *m, uint32_t linear)
{
    return m->a20 ? (linear & 0xFFFFFF) : (linear & 0xFFFFF);
}

uint8_t mem_rd8_slow(Machine *m, uint32_t linear);
void mem_wr8_slow(Machine *m, uint32_t linear, uint8_t v);

/* The VGA window is routed through the VGA device so planar modes work. */
static inline bool mem_is_vga(uint32_t linear) { return linear - MEM_VGA_BASE < 0x20000; }

static inline uint8_t mem_rd8(Machine *m, uint32_t linear)
{
    linear = mem_wrap(m, linear);
    if (mem_is_vga(linear))
        return mem_rd8_slow(m, linear);
    return m->mem[linear];
}

static inline void mem_wr8(Machine *m, uint32_t linear, uint8_t v)
{
    linear = mem_wrap(m, linear);
    if (mem_is_vga(linear) || (linear >= 0xC0000 && linear < MEM_ONE_MEG)) {
        mem_wr8_slow(m, linear, v);
        return;
    }
    m->mem[linear] = v;
}

static inline uint16_t mem_rd16(Machine *m, uint32_t linear)
{
    return (uint16_t)(mem_rd8(m, linear) | mem_rd8(m, linear + 1) << 8);
}

static inline uint32_t mem_rd32(Machine *m, uint32_t linear)
{
    return mem_rd16(m, linear) | (uint32_t)mem_rd16(m, linear + 2) << 16;
}

static inline void mem_wr16(Machine *m, uint32_t linear, uint16_t v)
{
    mem_wr8(m, linear, (uint8_t)v);
    mem_wr8(m, linear + 1, (uint8_t)(v >> 8));
}

static inline void mem_wr32(Machine *m, uint32_t linear, uint32_t v)
{
    mem_wr16(m, linear, (uint16_t)v);
    mem_wr16(m, linear + 2, (uint16_t)(v >> 16));
}

static inline uint32_t real_addr(uint16_t seg, uint16_t off) { return ((uint32_t)seg << 4) + off; }

/* Host pointer into conventional memory for bulk copies; the caller guarantees no VGA overlap. */
static inline uint8_t *mem_ptr(Machine *m, uint16_t seg, uint16_t off)
{
    return m->mem + real_addr(seg, off);
}

void mem_read_block(Machine *m, uint32_t linear, void *dst, size_t len);
void mem_write_block(Machine *m, uint32_t linear, const void *src, size_t len);
/* Copies a NUL-terminated guest string at seg:off into dst (always terminated). */
void mem_read_cstr(Machine *m, uint16_t seg, uint16_t off, char *dst, size_t cap);

/* ---- CPU ---- */

void cpu_reset(Machine *m);
void cpu_set_seg(Cpu *c, int sreg, uint16_t value);
/* Runs up to budget instructions; stops early on halt or exit. Returns instructions executed. */
uint64_t cpu_run(Machine *m, uint64_t budget);
/* Delivers interrupt vector n now, as INT n would. */
void cpu_interrupt(Machine *m, uint8_t vector);
void cpu_push16(Machine *m, uint16_t v);
uint16_t cpu_pop16(Machine *m);

static inline uint16_t reg16(const Machine *m, int r) { return m->cpu.r[r].w; }
static inline void set_reg16(Machine *m, int r, uint16_t v) { m->cpu.r[r].w = v; }
#define AX (m->cpu.r[R_AX].w)
#define BX (m->cpu.r[R_BX].w)
#define CX (m->cpu.r[R_CX].w)
#define DX (m->cpu.r[R_DX].w)
#define SI (m->cpu.r[R_SI].w)
#define DI (m->cpu.r[R_DI].w)
#define BP (m->cpu.r[R_BP].w)
#define SP (m->cpu.r[R_SP].w)
#define AL (m->cpu.r[R_AX].b.l)
#define AH (m->cpu.r[R_AX].b.h)
#define BL (m->cpu.r[R_BX].b.l)
#define BH (m->cpu.r[R_BX].b.h)
#define CL (m->cpu.r[R_CX].b.l)
#define CH (m->cpu.r[R_CX].b.h)
#define DL (m->cpu.r[R_DX].b.l)
#define DH (m->cpu.r[R_DX].b.h)
#define ES (m->cpu.seg[S_ES])
#define CS (m->cpu.seg[S_CS])
#define SS (m->cpu.seg[S_SS])
#define DS (m->cpu.seg[S_DS])

/* Callbacks return to the guest via the stub's IRET, so flags to return live in the pushed copy. */
void cb_set_carry(Machine *m, bool carry);
void cb_set_zero(Machine *m, bool zero);

/* ---- devices and services ---- */

/* Registers fn as callback and points interrupt vector at its stub (vector < 0: no vector). */
int callback_register(Machine *m, const char *name, CallbackFn fn, int vector);
uint32_t callback_stub_addr(int id);

void port_register(Machine *m, uint16_t first, uint16_t last, PortInFn in, PortOutFn out);
uint32_t port_in(Machine *m, uint16_t port, int size);
void port_out(Machine *m, uint16_t port, uint32_t value, int size);

/* IRQ line raised by a device; the PIC turns it into a vector when unmasked. */
void pic_raise_irq(Machine *m, int irq);
void pic_lower_irq(Machine *m, int irq);
/* Highest-priority deliverable vector, or -1. Acknowledges it. */
int pic_acknowledge(Machine *m);

/* VGA window access, implemented by the VGA device. */
uint8_t vga_mem_read(Machine *m, uint32_t linear);
void vga_mem_write(Machine *m, uint32_t linear, uint8_t v);

/* ---- machine ---- */

Machine *machine_create(void);
void machine_destroy(Machine *m);
/* Called every scheduler slice (TICK_SLICE_US of emulated time) after the CPU ran. */
void machine_add_ticker(Machine *m, TickFn fn);
/* Advances emulated time by us microseconds: CPU slices interleaved with device ticks. */
void machine_run_for(Machine *m, uint64_t us);
/* Runs the CPU until the current slice is used up or the CPU stops; ends the slice if used up. */
void machine_step(Machine *m);

enum { TICK_SLICE_US = 100 };

/* Emulated time in ns, refined inside a slice by the instructions already run, so devices that
 * are polled in tight loops (OPL timers) and sample-accurate audio see time pass. */
uint64_t machine_now_ns(const Machine *m);

void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
