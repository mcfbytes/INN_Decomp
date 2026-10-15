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
    const char *only; /* INN_LIFT_ONLY: comma-separated names to hook, all when NULL */
    struct VerifyState *verify;
    struct { char name[16]; uint16_t load_seg; } modules[16];
    int module_count;
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

#endif
