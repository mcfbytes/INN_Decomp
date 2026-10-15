/* Lifted-function registry state shared by dispatch (lift.c) and verification (lift_verify.c). */
#ifndef DOSRT_LIFT_INTERNAL_H
#define DOSRT_LIFT_INTERNAL_H

#include "lift.h"

enum { SHADOW_STALL_INSNS = 200000000 };
enum { MAX_TABLES = 64, MAX_ACTIVE = 8192, MAX_NESTING = 64, HOOK_BITMAP_BYTES = MEM_ONE_MEG / 8 };

/* Shadow-verification counts of one entry, kept across loads (overlays come and go). */
typedef struct HookStats {
    bool armed_once;
    int shadow_calls, shadow_mismatches;
    int result_diffs; /* calls whose AX or DX differed although scratch: a possible wrong return value */
    int synth_calls, synth_mismatches, synth_result_diffs; /* the same for synthetic calls (lift_synth.c) */
    int replay_calls; /* shadow calls verified with device replay */
} HookStats;

typedef struct ActiveHook {
    uint32_t linear;
    uint16_t load_seg;
    uint16_t reloc_base; /* relocated words in the signature hold reloc_base + file value */
    const LiftedEntry *entry;
    bool verified, rejected; /* entry bytes compared on first use */
    bool overlay;        /* in an overlay buffer: entry bytes compared on every call */
    bool absent_logged;  /* an overlay entry was reached with other bytes there; logged once */
    HookStats *stats;
} ActiveHook;

/* A pending return from guest code called by lifted code. */
typedef struct NestedCall {
    uint32_t return_linear;
    uint16_t return_sp;
    bool sp_above; /* match any SP above return_sp: the callee may pop its own arguments */
    bool returned;
} NestedCall;

typedef struct LiftState {
    struct { const LiftedEntry *entries; HookStats *stats; int count; } tables[MAX_TABLES];
    int table_count;
    ActiveHook active[MAX_ACTIVE];
    int active_count;
    NestedCall nested[MAX_NESTING];
    int nesting;
    int return_callback;
    bool in_shadow;
    bool hooks_off; /* shadow runs of the original run with every lifted hook off */
    const ActiveHook *shadow_of; /* the hook being shadow-verified, for stall reports */
    void (*nested_yield)(Machine *m);
    uint16_t idle_off; /* BIOS-segment stub PUSHF; STI; HLT; POPF; RETF, emitted on first use */
    int c_depth; /* lifted C on the host stack and not inside a nested guest call */
    const char *verify_skip; /* INN_VERIFY_SKIP: hooked names whose calls are never shadowed */
    const char *verify_replay; /* INN_VERIFY_REPLAY: names shadowed with device replay (lift_replay.c) */
    struct Replay *replay;
    const char *only; /* INN_LIFT_ONLY: comma-separated names to hook, all when NULL */
    struct VerifyState *verify;
    struct { char name[16]; uint16_t load_seg; } modules[16];
    int module_count;
    struct Synth *synth;     /* INN_SYNTH: synthetic call lists (lift_synth.c), NULL when off */
    uint64_t nested_budget;  /* nonzero: nested runs stop once cpu.instructions reaches it */
    bool nested_abort;       /* set when the budget ran out; every nested run then unwinds */
    bool direct, direct_cb; /* INN_DIRECT_CALLS: lifted entries, thunks and returns / service stubs skip the CPU */
    uint64_t direct_calls, direct_fallbacks, direct_callbacks, direct_thunks, direct_returns;
    LiftUnresolvedFn unresolved; /* lift_set_unresolved */
    bool native_control; /* INN_NATIVE_CONTROL=1: native-only entries are armed in a hybrid build */
    bool native_seen;    /* m->native was set when a module was armed: native-only entries stay armed */
} LiftState;

struct VerifyState;

static inline LiftState *lift_state(Machine *m) { return m->lift; }

void lift_set_hook_bit(Machine *m, uint32_t linear, bool on);
/* Runs guest code until the pending nested call returns (see NestedCall). */
void lift_run_nested(Machine *m, uint32_t return_linear, uint16_t return_sp, bool sp_above);
/* Whether name is one of the comma-separated names in list. */
bool lift_listed(const char *list, const char *name);
/* Runs a lifted function, marking lifted C as running for port-time accounting. */
void lift_call_c(Machine *m, LiftedFn fn);
/* Shadow-verifies one call of h; returns false when verification does not apply. */
bool lift_verify_call(Machine *m, ActiveHook *h);
/* Whether the bytes at h's entry are the entry's signature (with relocations applied). */
bool lift_entry_bytes_match(Machine *m, const ActiveHook *h);

/* ---- device replay (lift_replay.c, docs/lifting.md "Device replay") ---- */
bool lift_replay_active(const Machine *m);
/* The original's run: interrupts flow, gated to the instruction after an event; events are logged. */
void lift_replay_record_begin(Machine *m);
void lift_replay_record_end(Machine *m);
/* Runs the lifted function against the log; false when it diverged (lift_replay_why says where). */
bool lift_replay_play(Machine *m, LiftedFn fn);
const char *lift_replay_why(Machine *m);
void lift_replay_counts(Machine *m, int *irqs, int *late_irqs, size_t *entries);
void lift_replay_free(Machine *m);
void lift_replay_io_record(Machine *m, uint16_t port, int size, uint32_t value, bool is_out);
uint32_t lift_replay_io_in(Machine *m, uint16_t port, int size);
void lift_replay_io_out(Machine *m, uint16_t port, uint32_t value, int size);

/* ---- synthetic calls (lift_synth.c, docs/lifting.md "Synthetic calls") ---- */
typedef struct SynthResult {
    bool returned, mismatch;
    uint16_t ax, dx;
    uint8_t at_result[24]; /* the original's bytes at DX:AX, when DX:AX points into the HMA buffers */
    bool has_bytes;
} SynthResult;
/* Verifies the call the machine is set up for (CS:IP at h's entry, return address pushed) and leaves
 * the machine in that state again; label names the call list line; budget caps the original run. */
SynthResult lift_verify_synthetic(Machine *m, ActiveHook *h, const char *label, uint64_t budget);
void lift_synth_module(Machine *m, const char *module, uint16_t load_seg);
void lift_synth_unloaded(Machine *m, uint32_t lo, uint32_t hi);
void lift_synth_reached(Machine *m, uint32_t linear);
void lift_synth_shutdown(Machine *m);

#endif
