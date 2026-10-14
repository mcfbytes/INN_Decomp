/* Lifted-function registry state shared by dispatch (lift.c) and verification (lift_verify.c). */
#ifndef DOSRT_LIFT_INTERNAL_H
#define DOSRT_LIFT_INTERNAL_H

#include "lift.h"

enum { MAX_TABLES = 64, MAX_ACTIVE = 8192, MAX_NESTING = 64, HOOK_BITMAP_BYTES = MEM_ONE_MEG / 8 };

typedef struct ActiveHook {
    uint32_t linear;
    const LiftedEntry *entry;
    bool verified, rejected; /* entry bytes compared on first use */
    int shadow_calls, shadow_mismatches;
} ActiveHook;

/* A pending return from guest code called by lifted code. */
typedef struct NestedCall {
    uint32_t return_linear;
    uint16_t return_sp;
    bool sp_above; /* match any SP above return_sp: the callee may pop its own arguments */
    bool returned;
} NestedCall;

typedef struct LiftState {
    struct { const LiftedEntry *entries; int count; } tables[MAX_TABLES];
    int table_count;
    ActiveHook active[MAX_ACTIVE];
    int active_count;
    NestedCall nested[MAX_NESTING];
    int nesting;
    int return_callback;
    uint32_t bypass_linear; /* the next dispatch here runs the original (shadow run) */
    bool in_shadow;
    struct VerifyState *verify;
    struct { char name[16]; uint16_t load_seg; } modules[16];
    int module_count;
} LiftState;

struct VerifyState;

static inline LiftState *lift_state(Machine *m) { return m->lift; }

void lift_set_hook_bit(Machine *m, uint32_t linear, bool on);
/* Runs guest code until the pending nested call returns (see NestedCall). */
void lift_run_nested(Machine *m, uint32_t return_linear, uint16_t return_sp, bool sp_above);
/* Shadow-verifies one call of h; returns false when verification does not apply. */
bool lift_verify_call(Machine *m, ActiveHook *h);

#endif
