/* Device replay for shadow runs (docs/lifting.md, "Device replay"). The original runs with emulated
 * time and interrupts flowing; its port I/O, completed host callbacks (BIOS and DOS services, INT 1Ah
 * among them), HLTs and the interrupts it took are logged in order. Interrupts reach it only at the
 * first instruction boundary with IF set after one of those events, so each one has an unambiguous
 * place in the log. The lifted run then sees the same port reads, its writes and callbacks are
 * compared against the log, and each logged interrupt is delivered as soon as the lifted run has
 * passed the event before it: at the next guest instruction boundary with IF set, or at once when
 * lifted C made the event with IF set. Devices never see the lifted run. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lift_internal.h"

enum {
    REPLAY_LATE_INSNS = 100000, /* an interrupt pending this long without an event is taken anyway */
    REPLAY_MAX_EVENTS = 1 << 24,
};

typedef enum EventKind { EV_IN, EV_OUT, EV_CALLBACK, EV_HALT, EV_IRQ } EventKind;

/* Consecutive identical events (a poll loop) share one entry with a repeat count. */
typedef struct ReplayEvent {
    uint8_t kind, size;
    uint16_t port; /* port, callback id or interrupt vector */
    uint32_t value;
    uint32_t count;
} ReplayEvent;

typedef enum Phase { PHASE_OFF, PHASE_RECORD, PHASE_PLAY } Phase;

typedef struct Replay {
    Phase phase;
    ReplayEvent *log;
    size_t len, cap;
    size_t pos;     /* play: entry being consumed */
    uint32_t used;  /* play: repeats of log[pos] already consumed */
    uint64_t index; /* play: events consumed, for reports */
    bool gate_open; /* record: an event happened since the last boundary with IF set */
    uint64_t last_event_insn; /* record and, with late_gap, play: instructions at the last event */
    bool late_gap; /* play: a late interrupt waits until as many instructions passed without an event */
    int irqs, late_irqs;
    bool diverged;
    char why[160];
    jmp_buf *abort; /* play: where a divergence unwinds the lifted run to */
} Replay;

static Replay *replay(Machine *m)
{
    LiftState *s = lift_state(m);
    if (!s->replay) {
        s->replay = calloc(1, sizeof(Replay));
        if (!s->replay)
            fatal("out of memory");
    }
    return s->replay;
}

bool lift_replay_active(const Machine *m)
{
    const LiftState *s = m->lift;
    return s && s->replay && s->replay->phase != PHASE_OFF;
}

/* ---- recording ---- */

static void append(Replay *r, EventKind kind, uint16_t port, int size, uint32_t value)
{
    if (r->len) {
        ReplayEvent *last = &r->log[r->len - 1];
        if (kind != EV_IRQ && last->kind == kind && last->port == port && last->size == size && last->value == value) {
            last->count++;
            return;
        }
    }
    if (r->len == r->cap) {
        if (r->cap >= REPLAY_MAX_EVENTS) {
            if (!r->diverged)
                snprintf(r->why, sizeof r->why, "device replay log full (%d entries)", REPLAY_MAX_EVENTS);
            r->diverged = true;
            return;
        }
        size_t cap = r->cap ? r->cap * 2 : 4096;
        ReplayEvent *log = realloc(r->log, cap * sizeof *log);
        if (!log)
            fatal("out of memory");
        r->log = log;
        r->cap = cap;
    }
    r->log[r->len++] = (ReplayEvent){ (uint8_t)kind, (uint8_t)size, port, value, 1 };
}

static void record_event(Machine *m, EventKind kind, uint16_t port, int size, uint32_t value)
{
    Replay *r = replay(m);
    append(r, kind, port, size, value);
    r->gate_open = true;
    r->last_event_insn = m->cpu.instructions;
}

/* ---- playing ---- */

static const char *describe(char *buf, size_t cap, EventKind kind, uint16_t port, int size, uint32_t value)
{
    switch (kind) {
    case EV_IN: snprintf(buf, cap, "in %04X/%d", port, size); break;
    case EV_OUT: snprintf(buf, cap, "out %04X/%d <- %X", port, size, value); break;
    case EV_CALLBACK: snprintf(buf, cap, "callback %u", port); break;
    case EV_HALT: snprintf(buf, cap, "hlt"); break;
    case EV_IRQ: snprintf(buf, cap, "interrupt %02X", port); break;
    }
    return buf;
}

static void diverge(Machine *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void diverge(Machine *m, const char *fmt, ...)
{
    Replay *r = replay(m);
    if (!r->diverged) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(r->why, sizeof r->why, fmt, ap);
        va_end(ap);
        r->diverged = true;
    }
    if (r->abort)
        longjmp(*r->abort, 1);
}

static const ReplayEvent *head(Replay *r)
{
    return r->pos < r->len ? &r->log[r->pos] : NULL;
}

/* Instructions run so far, charged ones (cpu.stall, not yet folded in by lifted C) included. */
static uint64_t insns_now(const Machine *m)
{
    return m->cpu.instructions + m->cpu.stall;
}

static void advance(Replay *r)
{
    r->index++;
    if (++r->used >= r->log[r->pos].count) {
        r->pos++;
        r->used = 0;
    }
}

/* Delivers the logged interrupt at the head. In lifted C there is no instruction boundary, so it runs
 * as a nested INT from here; in guest code the CPU takes it before the next instruction. */
static void deliver_head(Machine *m, bool from_c)
{
    Replay *r = replay(m);
    uint8_t vector = (uint8_t)r->log[r->pos].port;
    advance(r);
    if (from_c)
        guest_int(m, vector);
    else
        cpu_interrupt(m, vector);
}

/* After the lifted function returned: the interrupt frame holds the caller's CS:IP, as the original's. */
static void deliver_at_return(Machine *m)
{
    uint32_t at = m->cpu.seg_base[S_CS] + (m->cpu.eip & 0xFFFF);
    uint16_t sp = m->cpu.r[R_SP].w;
    bool was_hooked = lift_hooked(m, at);
    lift_set_hook_bit(m, at, true);
    deliver_head(m, false);
    lift_run_nested(m, at, sp, false);
    if (!was_hooked)
        lift_set_hook_bit(m, at, false);
}

/* A late interrupt (taken after REPLAY_LATE_INSNS without an event) is never due right after an event
 * when the gap is kept. */
static bool irq_due_now(Machine *m, const Replay *r, const ReplayEvent *e)
{
    return !r->late_gap || !e->value || m->cpu.halted || insns_now(m) - r->last_event_insn >= REPLAY_LATE_INSNS;
}

static void deliver_due_in_c(Machine *m)
{
    Replay *r = replay(m);
    while (lift_in_c(m) && (m->cpu.flags & F_IF) && head(r) && head(r)->kind == EV_IRQ && irq_due_now(m, r, head(r)))
        deliver_head(m, true);
}

/* The next logged event must be this one; interrupts still due before it are taken first when lifted
 * C makes the event (IF was clear at the last chance), and are a divergence in guest code. */
static const ReplayEvent *consume(Machine *m, EventKind kind, uint16_t port, int size, uint32_t value)
{
    Replay *r = replay(m);
    char got[48], want[48];
    while (head(r) && head(r)->kind == EV_IRQ && kind != EV_IRQ) {
        if (!lift_in_c(m))
            diverge(m, "event %llu: lifted %s, original took interrupt %02X first",
                    (unsigned long long)r->index, describe(got, sizeof got, kind, port, size, value),
                    head(r)->port);
        r->late_irqs++;
        deliver_head(m, true);
    }
    const ReplayEvent *e = head(r);
    if (!e)
        diverge(m, "event %llu: lifted %s after the original's last event", (unsigned long long)r->index,
                describe(got, sizeof got, kind, port, size, value));
    bool same = e && e->kind == kind && e->port == port && e->size == size &&
                (kind == EV_IN || e->value == value);
    if (!same && e)
        diverge(m, "event %llu: lifted %s, original %s", (unsigned long long)r->index,
                describe(got, sizeof got, kind, port, size, value),
                describe(want, sizeof want, (EventKind)e->kind, e->port, e->size, e->value));
    if (!e || !same)
        return NULL; /* web builds: no unwinding, the comparison reports it */
    advance(r);
    r->last_event_insn = insns_now(m);
    return e;
}

/* ---- the taps ---- */

void lift_replay_io_record(Machine *m, uint16_t port, int size, uint32_t value, bool is_out)
{
    record_event(m, is_out ? EV_OUT : EV_IN, port, size, value);
}

uint32_t lift_replay_io_in(Machine *m, uint16_t port, int size)
{
    const ReplayEvent *e = consume(m, EV_IN, port, size, 0);
    uint32_t v = e ? e->value : 0xFFFFFFFFu;
    deliver_due_in_c(m);
    return v;
}

void lift_replay_io_out(Machine *m, uint16_t port, uint32_t value, int size)
{
    consume(m, EV_OUT, port, size, value);
    deliver_due_in_c(m);
}

void lift_io_callback(Machine *m, int id)
{
    LiftState *s = lift_state(m);
    if (!lift_replay_active(m) || id == s->return_callback)
        return;
    if (s->replay->phase == PHASE_RECORD)
        record_event(m, EV_CALLBACK, (uint16_t)id, 0, 0);
    else
        consume(m, EV_CALLBACK, (uint16_t)id, 0, 0);
}

void lift_io_halt(Machine *m)
{
    if (!lift_replay_active(m))
        return;
    if (replay(m)->phase == PHASE_RECORD)
        record_event(m, EV_HALT, 0, 0, 0);
    else
        consume(m, EV_HALT, 0, 0, 0);
}

void lift_irq_boundary(Machine *m)
{
    Replay *r = replay(m);
    Cpu *c = &m->cpu;
    if (r->phase == PHASE_RECORD) {
        bool late = c->instructions - r->last_event_insn >= REPLAY_LATE_INSNS;
        if (!m->irq_pending) {
            if (!c->halted)
                r->gate_open = false;
            return;
        }
        if (!r->gate_open && !c->halted && !late)
            return;
        r->gate_open = false;
        int vector = pic_acknowledge(m);
        if (vector < 0)
            return;
        append(r, EV_IRQ, (uint16_t)vector, 0, late && !c->halted);
        r->irqs++;
        r->late_irqs += late && !c->halted;
        cpu_interrupt(m, (uint8_t)vector);
        return;
    }
    if (r->phase != PHASE_PLAY)
        return;
    if (head(r) && head(r)->kind == EV_IRQ) {
        if (!irq_due_now(m, r, head(r)))
            return;
        deliver_head(m, false);
    } else if (c->halted) {
        c->halted = false;
        diverge(m, "event %llu: lifted run halted where the original took no interrupt",
                (unsigned long long)r->index);
    }
}

/* ---- the two phases ---- */

void lift_replay_record_begin(Machine *m)
{
    Replay *r = replay(m);
    r->phase = PHASE_RECORD;
    r->len = r->pos = 0;
    r->used = 0;
    r->index = 0;
    r->irqs = r->late_irqs = 0;
    r->diverged = false;
    r->why[0] = 0;
    r->gate_open = false;
    r->last_event_insn = m->cpu.instructions;
    m->irq_hold = false;
    m->irq_gate = true;
}

void lift_replay_record_end(Machine *m)
{
    replay(m)->phase = PHASE_OFF;
    m->irq_gate = false;
    m->irq_hold = true;
}

bool lift_replay_play(Machine *m, LiftedFn fn)
{
    LiftState *s = lift_state(m);
    Replay *r = replay(m);
    bool recorded_ok = !r->diverged;
    r->phase = PHASE_PLAY;
    r->pos = 0;
    r->used = 0;
    r->index = 0;
    r->last_event_insn = insns_now(m);
    r->late_gap = s->shadow_of && s->replay_late && lift_listed(s->replay_late, s->shadow_of->entry->name);
    m->irq_gate = true;
    m->ticks_frozen = true;
    int nesting = s->nesting, c_depth = s->c_depth;
#ifndef __EMSCRIPTEN__
    jmp_buf abort;
    if (setjmp(abort) == 0) {
        r->abort = &abort;
#endif
        if (recorded_ok) {
            lift_call_c(m, fn);
            /* The original took these at its return, before the caller's next instruction. */
            while (head(r) && head(r)->kind == EV_IRQ && (m->cpu.flags & F_IF))
                deliver_at_return(m);
            if (head(r)) {
                char want[48];
                const ReplayEvent *e = head(r);
                diverge(m, "event %llu: the lifted run returned, the original went on with %s",
                        (unsigned long long)r->index,
                        describe(want, sizeof want, (EventKind)e->kind, e->port, e->size, e->value));
            }
        }
#ifndef __EMSCRIPTEN__
    } else {
        s->nesting = nesting; /* unwound out of nested guest calls */
        s->c_depth = c_depth;
        s->nested_abort = false;
    }
    r->abort = NULL;
#endif
    (void)nesting, (void)c_depth;
    r->phase = PHASE_OFF;
    m->irq_gate = false;
    m->ticks_frozen = false;
    return !r->diverged;
}

const char *lift_replay_why(Machine *m)
{
    return replay(m)->why;
}

void lift_replay_counts(Machine *m, int *irqs, int *late_irqs, size_t *entries)
{
    Replay *r = replay(m);
    *irqs = r->irqs;
    *late_irqs = r->late_irqs;
    *entries = r->len;
}

void lift_replay_free(Machine *m)
{
    LiftState *s = lift_state(m);
    if (s->replay)
        free(s->replay->log);
    free(s->replay);
    s->replay = NULL;
}
