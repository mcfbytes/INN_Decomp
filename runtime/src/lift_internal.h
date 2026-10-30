/* Lifted-function registry state shared by dispatch (lift.c) and verification (lift_verify.c). */
#ifndef DOSRT_LIFT_INTERNAL_H
#define DOSRT_LIFT_INTERNAL_H

#include "lift_semantic.h"
#include "lift.h"

enum { SHADOW_STALL_INSNS = 200000000 };
enum { MAX_TABLES = 64, MAX_ACTIVE = 8192, MAX_NESTING = 64, HOOK_BITMAP_BYTES = MEM_ONE_MEG / 8, MAX_IMAGES = 16,
       MAX_MODULE_META = 128 };

/* Shadow-verification counts of one entry, kept across loads (overlays come and go). */
typedef struct HookStats {
    bool armed_once;
    int shadow_calls, shadow_mismatches;
    int result_diffs; /* calls whose AX or DX differed although scratch: a possible wrong return value */
    int synth_calls, synth_mismatches, synth_result_diffs; /* the same for synthetic calls (lift_synth.c) */
    int replay_calls; /* shadow calls verified with device replay */
    int cost_diffs; /* INN_VERIFY_COST: calls whose lifted instruction count differs from the original's */
    uint64_t cost_original, cost_lifted;
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
    bool sp_above; /* match any SP above return_sp (modulo 64K, so a stack at the segment top may wrap): the callee may pop its own arguments */
    bool sp_any;   /* match any SP: the callee may move its caller's frame (LiftedEntry.lower_sp) */
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
    const char *replay_late; /* INN_REPLAY_LATE: replayed names whose late interrupts wait out the gap */
    const char *replay_exact; /* INN_REPLAY_EXACT: replayed names whose interrupts come when due, at their gap */
    struct Replay *replay;
    const char *only; /* INN_LIFT_ONLY: comma-separated names to hook, all when NULL */
    const char *except; /* INN_LIFT_EXCEPT: comma-separated names never to hook */
    struct VerifyState *verify;
    struct { char name[16]; uint16_t load_seg; bool build_matches; } modules[16];
    int module_count;
    const LiftedModule *module_meta[MAX_MODULE_META];
    int module_meta_count;
    struct Synth *synth;     /* INN_SYNTH: synthetic call lists (lift_synth.c), NULL when off */
    uint64_t nested_budget;  /* nonzero: nested runs stop once cpu.instructions reaches it */
    bool nested_abort;       /* set when the budget ran out; every nested run then unwinds */
    bool direct, direct_cb; /* INN_DIRECT_CALLS: lifted entries, thunks and returns / service stubs skip the CPU */
    uint64_t direct_calls, direct_fallbacks, direct_callbacks, direct_thunks, direct_returns;
    LiftUnresolvedFn unresolved; /* lift_set_unresolved */
    bool native_control; /* INN_NATIVE_CONTROL=1: native-only entries are armed in a hybrid build */
    bool native_seen;    /* m->native was set when a module was armed: native-only entries stay armed */
    /* Images the program loads itself (lift_add_image): where each is announced now, 0 when nowhere. */
    struct { const LiftedImage *image; uint16_t load_seg; } images[MAX_IMAGES];
    int image_count;
    bool dump_far_calls; /* INN_DUMP_AT names a module address point (native_dump_far_call) */
} LiftState;

struct VerifyState;

/* ---- semantic port comparison (lift_semantic.c): the lifted run's side, kept by lift_verify.c ---- */
/* The lifted run's trace while a shadow run is comparing semantically, else NULL. */
SemTrace *lift_verify_semantic_lifted(Machine *m);
/* The value of the next original read of this kind; false when the original made no more. */
bool lift_verify_semantic_original_read(Machine *m, SemKind kind, uint32_t *value);
void lift_verify_semantic_missing(Machine *m, SemKind kind);

static inline LiftState *lift_state(Machine *m) { return m->lift; }

void lift_set_hook_bit(Machine *m, uint32_t linear, bool on);
/* Runs guest code until the pending nested call returns (see NestedCall). */
void lift_run_nested(Machine *m, uint32_t return_linear, uint16_t return_sp, bool sp_above);
/* Sets m->far_call_probe from the images still waiting and the dump points. */
void lift_update_probe(Machine *m);
/* As lift_run_nested, ending at the first arrival at return_linear whatever SP is then. */
void lift_run_nested_any_sp(Machine *m, uint32_t return_linear);
/* Whether name is one of the comma-separated names in list. */
bool lift_listed(const char *list, const char *name);
/* Runs a lifted function, marking lifted C as running for port-time accounting. */
void lift_call_c(Machine *m, LiftedFn fn);
/* Shadow-verifies one call of h; returns false when verification does not apply. */
bool lift_verify_call(Machine *m, ActiveHook *h);
/* Called as a lifted entry starts inside a shadow run's lifted half: an entry on another stack than the
 * verified call's makes the memory just below its SP dead stack for the comparison. */
void lift_verify_nested_entry(Machine *m);
/* Whether the bytes at h's entry are the entry's signature (with relocations applied; either form of
 * an emulator sequence where LiftedEntry.emu_mask marks one). */
bool lift_entry_bytes_match(Machine *m, const ActiveHook *h);

/* ---- floating-point emulator sequences (tools/borland_emu_patch.py): an emulator with a coprocessor patches
 * CD 34+i into 9B D8+i, CD 3C b into 9B <prefix of b >> 6> b|C0, CD 3D into 90 9B ---- */
/* Length (2 or 3) of the INT-form sequence at b[0..n), or 0 when there is none or it is cut off. */
int lift_emu_int_len(const uint8_t *b, int n);
/* The INT form of the sequence at b[0..n) in either form, into out[3]; its length, or 0 for none. */
int lift_emu_to_int(const uint8_t *b, int n, uint8_t out[3]);
/* Whether patched[0..len) is what an emulator writes over the INT-form sequence intform[0..len). */
bool lift_emu_is_patch_of(const uint8_t *intform, const uint8_t *patched, int len);

/* ---- native core (runtime/src/native, docs/native.md) ---- */
/* The announced module with the highest load segment not above seg, or NULL; *load_seg is its base. */
const char *lift_module_owning(Machine *m, uint16_t seg, uint16_t *load_seg);
/* Whether any table has a native-only entry (a native program start) for the module. */
bool lift_module_has_native(Machine *m, const char *module);

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
 * the machine in that state again, or with keep_end in the original's end state when the call
 * returned and matched; label names the call list line; budget caps the original run. */
SynthResult lift_verify_synthetic(Machine *m, ActiveHook *h, const char *label, uint64_t budget, bool keep_end);
void lift_synth_module(Machine *m, const char *module, uint16_t load_seg);
void lift_synth_unloaded(Machine *m, uint32_t lo, uint32_t hi);
void lift_synth_reached(Machine *m, uint32_t linear);
void lift_synth_shutdown(Machine *m);

#endif
