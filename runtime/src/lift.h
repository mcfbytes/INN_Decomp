/* Lifted functions: hand-decompiled C that replaces original code at its entry address.
 * Contract, registration and verification: docs/lifting.md. */
#ifndef DOSRT_LIFT_H
#define DOSRT_LIFT_H

#include "machine.h"

/* Entered with CS:IP at the function entry and the caller's return address on the stack;
 * leaves the machine exactly as the original's return would. */
typedef void (*LiftedFn)(Machine *m);

enum { LIFT_SIGNATURE_LEN = 12 };

/* Register bits for LiftedEntry.scratch_regs: general registers by index, then segments. */
enum {
    LIFT_REG_AX = 1 << R_AX, LIFT_REG_CX = 1 << R_CX, LIFT_REG_DX = 1 << R_DX, LIFT_REG_BX = 1 << R_BX,
    LIFT_REG_SI = 1 << R_SI, LIFT_REG_DI = 1 << R_DI,
    LIFT_REG_ES = 1 << (8 + S_ES), LIFT_REG_FLAGS = 1 << 15,
};

/* One lifted function of one module build. The signature guards against other builds. */
typedef struct LiftedEntry {
    const char *module;     /* module name as registered by its loader, e.g. "LSCITV.EXE" */
    uint16_t seg, off;      /* entry, segment relative to the module's load segment */
    LiftedFn fn;
    const char *name;
    bool near_return;       /* returns with RET rather than RETF */
    uint16_t scratch_regs;  /* LIFT_REG_* bits the calling convention lets differ on return */
    uint8_t signature[LIFT_SIGNATURE_LEN]; /* original bytes at the entry, as in the file */
    uint16_t reloc_mask; /* bit i: a load-segment-relocated word starts at signature byte i */
    bool region;    /* LIFT_REGION: may leave at any instruction; see lift_verify.c */
    bool no_shadow; /* LIFT_NOSHADOW: hooked, never shadow-verified (device I/O, process exit) */
} LiftedEntry;

/* reloc_mask bit 15: byte 0 is the high half of a relocated word starting before the entry. */
enum { LIFT_RELOC_SPLIT_HEAD = 1 << 15 };

/* Marks a lifted function in a lifted source file; tools/gen_lift_table.py builds the
 * registration table from these markers and the original binary. */
enum { LIFT_FAR = 0, LIFT_NEAR = 1, LIFT_REGION = 2 };
/* A far function hooked but never shadow-verified: its original would repeat device I/O (dialling,
 * UART handshakes) or end the process. Checked functionally only; use sparingly. */
enum { LIFT_FAR_NOSHADOW = 3, LIFT_NEAR_NOSHADOW = 4 };
#define LIFTED(seg, off, fn, kind, scratch) void fn(Machine *m);

/* Registers every lifted table built into this binary; a no-op without the private tree. */
void lifted_register_all(Machine *m);

void lift_init(Machine *m);
void lift_shutdown(Machine *m);
/* Adds a table of lifted functions; tables are static data owned by the caller. */
void lift_add_table(Machine *m, const LiftedEntry *entries, int count);

/* A loader announces where a module image now lives; matching entries become active. */
void lift_module_loaded(Machine *m, const char *module, uint16_t load_seg, uint32_t image_bytes);
/* Load segment of a module announced by lift_module_loaded, or 0 if it is not loaded. */
uint16_t lift_module_base(Machine *m, const char *module);
/* Deactivates every hook inside [load_seg, load_seg + paragraphs). */
void lift_module_unloaded(Machine *m, uint16_t load_seg, uint32_t paragraphs);

/* Whether the code running now is lifted C rather than interpreted guest code. */
bool lift_in_c(const Machine *m);

/* Called by the CPU before an instruction whose linear address has a hook bit set. */
bool lift_dispatch(Machine *m, uint32_t linear);

static inline bool lift_hooked(const Machine *m, uint32_t linear)
{
    return m->exec_hooks && (m->exec_hooks[linear >> 3] >> (linear & 7) & 1);
}

/* Shadow verification (docs/lifting.md): each lifted entry is checked against the original for
 * its first calls_per_entry calls. Returns the number of mismatches seen so far. */
void lift_verify_enable(Machine *m, int calls_per_entry);
int lift_verify_mismatches(const Machine *m);

/* Port I/O taps used while verifying; machine.c calls them according to m->io_mode. */
void lift_io_record(Machine *m, uint16_t port, int size, uint32_t value, bool is_out);
uint32_t lift_io_replay_in(Machine *m, uint16_t port, int size);
void lift_io_replay_out(Machine *m, uint16_t port, uint32_t value, int size);

/* ---- helpers for lifted code ---- */

/* Called between slices while lifted code waits in a nested guest call, so a frontend can present
 * frames, take input and pace against the wall clock; never during shadow verification. */
void lift_set_nested_yield(Machine *m, void (*fn)(Machine *m));

/* Runs original code at seg:off as a far call and returns when it returns. */
void guest_call_far(Machine *m, uint16_t seg, uint16_t off);
/* Waits for the next interrupt as STI; HLT would, with the caller's flags restored: emulated time
 * moves to the next timer tick or device IRQ and its handler runs. The way for lifted code to wait
 * for ticks: a busy loop of short guest calls advances time by a few instructions per pass. */
void lift_idle(Machine *m);

/* Runs original code at CS:off as a near call. */
void guest_call_near(Machine *m, uint16_t off);

static inline void lift_ret_far(Machine *m, uint16_t arg_bytes)
{
    uint16_t ip = cpu_pop16(m), cs = cpu_pop16(m);
    cpu_set_seg(&m->cpu, S_CS, cs);
    m->cpu.eip = ip;
    m->cpu.r[R_SP].w += arg_bytes;
}

static inline void lift_ret_near(Machine *m, uint16_t arg_bytes)
{
    m->cpu.eip = cpu_pop16(m);
    m->cpu.r[R_SP].w += arg_bytes;
}

/* Runs INT n through the current interrupt vector, as the instruction does. */
static inline void guest_int(Machine *m, uint8_t vector)
{
    cpu_push16(m, (uint16_t)m->cpu.flags);
    m->cpu.flags &= ~(uint32_t)(F_IF | F_TF);
    guest_call_far(m, mem_rd16(m, vector * 4u + 2), mem_rd16(m, vector * 4u));
}

/* Returns from a lifted interrupt handler as IRET does in real mode. */
static inline void lift_ret_iret(Machine *m)
{
    uint16_t ip = cpu_pop16(m), cs = cpu_pop16(m), flags = cpu_pop16(m);
    cpu_set_seg(&m->cpu, S_CS, cs);
    m->cpu.eip = ip;
    m->cpu.flags = (m->cpu.flags & 0xFFFF0000u) | (flags & 0x7FD5u) | 2u;
}

/* Word at SS:SP+offset, e.g. the first argument of a far cdecl call is at +4. */
static inline uint16_t stack_arg16(Machine *m, uint16_t offset)
{
    return mem_rd16(m, m->cpu.seg_base[S_SS] + (uint16_t)(m->cpu.r[R_SP].w + offset));
}

#endif
