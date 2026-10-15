/* Native core (docs/native.md sections 4-6): the clock, safe points and the interrupt model that let
 * lifted INN code run as C with no CPU and no emulated devices. */
#ifndef DOSRT_NATIVE_H
#define DOSRT_NATIVE_H

#include <stdbool.h>
#include <stdint.h>

#include "lift.h"
#include "machine.h"

typedef struct NativeConfig {
    bool deterministic;        /* headless: clock advanced only by charges and waits */
    bool strict;               /* fatal when guest code would be interpreted in native mode */
    uint32_t call_cost_ns;     /* work charge per dispatched call or host-API call (1000) */
    uint32_t yield_every_us;   /* wall time between cooperative yields (4000) */
} NativeConfig;

/* cfg NULL: defaults with the environment applied (INN_NATIVE_STRICT, INN_NATIVE_DETERMINISTIC,
 * INN_NATIVE_CALL_NS). machine_create calls it; calling it again changes only the configuration. */
void native_init(Machine *m, const NativeConfig *cfg);
void native_shutdown(Machine *m);
bool native_active(const Machine *m);              /* INN runs natively (not a game on the emulator) */
void native_set_active(Machine *m, bool on);       /* mixed-mode switch (package 10b) */
void native_safepoint(Machine *m);                 /* charge, deliver a due IRQ, maybe yield */
void native_charge(Machine *m, uint32_t ns);
void native_idle(Machine *m);                      /* STI; HLT: run until one interrupt was delivered */
void native_wait_until(Machine *m, uint64_t virtual_us); /* deliver IRQs while waiting */
void native_yield(Machine *m);                     /* present/pump/suspend once */
/* Input or link data arrived (host_input, host_link): a wait in progress returns to its caller. */
void native_wake(Machine *m);

void native_irq_raise(Machine *m, int irq);        /* 0-15, edge */
void native_irq_lower(Machine *m, int irq);
void native_pic_eoi(Machine *m, int chip, int irq); /* chip 0 master, 1 slave; irq < 0: non-specific */
void native_pic_set_mask(Machine *m, int chip, uint8_t mask);
uint8_t native_pic_mask(const Machine *m, int chip);
void native_int(Machine *m, uint8_t vector);       /* INT n without a CPU */
/* Delivers the highest-priority deliverable request (IF is the caller's concern); false when none. */
bool native_deliver_irq(Machine *m);

/* ROM routines of our own BIOS that are more than a service stub (an IRQ handler's callback, EOI and
 * IRET; the timer's INT 1Ch call) are registered with a C equivalent for the native dispatcher. */
void native_rom_lift(Machine *m, uint16_t seg, uint16_t off, LiftedFn fn, const char *name);

/* Port 61h as native code sees it (include/hw.h): the last value written, 30h after reset. */
uint8_t native_port61(const Machine *m);
void native_set_port61(Machine *m, uint8_t v);

/* Headless runs (no platform): fn runs at every yield and as a wait passes emulated time, to apply
 * the input script and end the run once its time is up. */
void native_set_headless_hook(Machine *m, void (*fn)(Machine *m));

/* ---- for the dispatcher (lift.c), the CPU and the machine ---- */

/* A dispatched target nothing native resolves: a ROM lift runs it (true); strict mode is fatal; the
 * non-strict fallback returns false and the interpreter runs it. */
bool native_unresolved(Machine *m, uint32_t linear);
/* cpu_run entered while native at a target no hook covers: fatal in strict mode. */
void native_cpu_entry(Machine *m);
_Noreturn void native_port_fatal(Machine *m, bool out, uint16_t port);
/* Time the interpreter used while native (non-strict fallback): charged, devices stay suspended. */
void native_interp_time(Machine *m, uint32_t us);

#endif
