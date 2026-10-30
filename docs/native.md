# M9: the web-native INN client

How the INN client (TSNEXEC, LSCITV, its DLLs and drivers) runs as C with no CPU interpretation and
no emulated hardware, while the games it starts keep running on the interpreter. This page is the
design; the work packages that implement it are private (`private/lifted/M9-PACKAGES.md`) because
they name addresses in Sierra's binaries.

## 1. Scope

Decided 2026-10-15:

- **Native INN.** Lifted functions call each other directly in C. Video goes to a host framebuffer and
  palette and then to the canvas; music goes to the existing OPL synthesiser and mixer, driven by
  function calls instead of port writes, and on to WebAudio; the PIT becomes a host timer; keyboard and
  mouse come from a host event queue; the modem link is the WebSocket (TCP natively); DOS services are
  runtime C calls; EMS and XMS report none. Sound Blaster digital audio is not played. The music driver
  has to be `adl.drv`; the browser page rewrites any other `music` line in its copy of `LSCI.CFG`.
- **Data stays where DOS put it.** SCI objects, heaps, resources, driver tables and the TSNEXEC link
  rings keep their addresses in the flat 1 MB guest memory image. Nothing is converted to host
  pointers or structs.
- **The interpreter stays** for shadow verification, for the games (Yserbius, Twinion, Red Baron, Golf,
  which are not lifted far enough to run without it), and as a development fallback.
- **Mixed mode.** When TSNEXEC starts a game, the machine switches to the emulator with emulated
  devices and the memory layout the game expects (TSNEXEC resident below it), and back when the game
  exits.

Not in scope: rewriting lifted code into C-native data structures, lifting the games, removing the
1 MB image.

## 2. What the prototype measured

A runtime-only prototype (branch `m9-proto`, section 13) made `guest_call_far`, `guest_call_near` and
`guest_int` call lifted entries and host-service stubs directly instead of running them through the
CPU loop. Logon scenario (click Play at 11 s, town map at 60 s, 65 s of emulated time, headless, 20 MIPS
guest clock):

| Build | Guest instructions interpreted | Wall time |
|---|---|---|
| hybrid today | 1.10 G | 19.2 s |
| direct service calls only | 0.82 G | 18.1 s |
| direct lifted calls and service calls | 0.39 G | 8.3 s |

With direct calls, 47.5 M lifted calls and 15.4 M service calls went straight to C, none fell back.
Shadow verification of the logon (95 s, 20 calls per function) checked 3367 calls with the one known
residue (kDrawPic), the same as without direct calls (3366 calls, same residue): direct calls do not
disturb verification.

Where the remaining 0.39 G instructions run:

| Share | Code | What removes it |
|---|---|---|
| 97.3 % | LSCITV's VM kernel-call sequence: the few dozen original instructions between the region lift's exit at a kernel call and the next opcode handler. About 46 M kernel calls in 65 s (700 k/s): the scripts poll in a loop with no Wait | native VM driver (package 3) |
| 1.4 % | the jump at a driver's start that leads to its lifted entry, and one bare `RET` a modem wait loop calls to burn time | dispatcher follows jump thunks and trivial returns (package 2) |
| 1.3 % | service stubs not taken directly: an interrupt was due, or the service blocked (`CB_RETRY`) | native interrupt delivery and waits (package 11) |
| < 0.3 % | VM start-up and InvokeMethod; TSNEXEC start-up and TSN.PRG interpreter; a little code loaded at run time outside every announced module | packages 3 and 2 |

Two failures on the way shaped the design (sections 5 and 6):

1. **Time must pass with work.** Direct calls took no emulated time. A modem wait that polls INT 1Ah for
   the BIOS tick never saw it change, because the timer interrupt is delivered only at an instruction
   boundary and none was executed: a hang. Once interrupts were taken at call boundaries, a script that
   times a loop divided by the ticks it took, which were 0: a VM divide-by-zero fatal. Charging 1 µs per
   direct call fixed both.
2. **Interrupts need safe points.** With no CPU there are no instruction boundaries; something else has
   to decide where an ISR may run.

## 3. Execution model

```
 frontend (SDL2 natively, SDL2 + JSPI in the browser): HostPlatform
   now, sleep/suspend, events, present, audio pump
        |
 native core: clock (work charge, waits, pacing) - safe points - IRQ model - native programs
        |
 lifted C, called directly            host APIs: video, audio, timer, input, link
   TSNEXEC (native main, TSN.PRG)        |
   LSCITV  (native main, VM driver)      +-- canvas / WebAudio / WebSocket
   GRAPH256, NLNULL, drivers
        |
 guest memory image (1 MB + HMA), register file, IVT, BIOS data area, DOS (runtime C)
        |
 interpreter + emulated devices: only for games (mixed mode), verification, development fallback
```

The machine (`Machine`) keeps one register file and one memory image for every tier, as today. In
native mode the register file is the calling convention between lifted functions (section 7), not the
state of a CPU.

`Machine` gains a mode: **native** while INN code runs, **emulated** while a game runs. In native mode
the device tickers do not run, the CPU loop is not entered except by an explicit development fallback,
port I/O is an error, and the VGA window is plain memory read by the host video.

## 4. Who drives the main loop

**The program drives.** `main()` boots the machine, loads `TSNEXEC.EXE` through the DOS loader, and
calls TSNEXEC's native entry. That runs TSN.PRG, whose program lines start `LSCITV.EXE`, whose native
entry runs the VM driver until the script quits. Control returns to the frontend only through waits.
There is no `emscripten_set_main_loop` and no per-frame `machine_run_for` in native mode.

Every place where the original waits becomes a call into the native core:

| Original wait | Native |
|---|---|
| `STI; HLT` until the next interrupt (`lift_idle`, used by `lsci_kWait`) | `native_idle`: deliver due interrupts; if none, advance the clock to the next timer tick (or input or link event) and wait for it |
| polling the BIOS tick (INT 1Ah) or a counter an ISR increments | every service call is a safe point (section 6) that charges time and delivers due interrupts, so the counter moves |
| waiting for vertical retrace (VGA status port) | `host_video_wait_retrace`: present if a frame is due, wait for the next 70 Hz boundary of the clock |
| a blocking BIOS read (INT 16h function 0 with an empty buffer) | the service returns `CB_RETRY`; the dispatcher waits for input with `native_idle` and calls it again |
| a script that polls GetEvent in a tight loop | safe points charge time; the clock is paced against the wall clock, so the loop sleeps the excess (section 5) |

**In the browser**, a wait that has to give the event loop a turn suspends the whole C stack with JSPI
(`emscripten_sleep`, or an `EM_ASYNC_JS` promise for a zero-delay yield through a `MessageChannel`, so
the 4 ms clamp of nested `setTimeout` is avoided). While suspended, WebSocket messages, input events,
`requestAnimationFrame` and WebAudio callbacks run. JSPI needs no instrumentation, so the deep native C
stacks (VM, kernel, driver, nested InvokeMethod) and the indirect calls through lifted-entry tables are
fine. The native web build requires JSPI (Chromium-based browsers today). Asyncify with
`ASYNCIFY_IGNORE_INDIRECT` cannot work here (lifted code is entered through function pointers);
full-instrumentation Asyncify, linked as a second variant from the same objects, is the fallback for
other browsers ([asyncify.md](asyncify.md)).

`setjmp`/`longjmp` (process exit unwinds the native C stack to the EXEC that started the program, and
the VM's Restart kernel) must use WebAssembly exception handling (`-fwasm-exceptions
-sSUPPORT_LONGJMP=wasm`): Emscripten's JavaScript-based longjmp routes calls through JS `invoke_*`
frames, and JSPI cannot suspend across JS frames.
Proved 2026-10-15: `test_native` built with `-sJSPI -fwasm-exceptions -sSUPPORT_LONGJMP=wasm` suspends
100 times with `emscripten_sleep(1)` in a lifted entry reached through the dispatcher, inside a `setjmp`
scope, then longjmps out and dispatches again (Node 24 with `--experimental-wasm-jspi`; the same
pattern passes in headless Chromium 153).

The native web build requires JSPI, so it runs in Chromium-based browsers (Chrome, Edge) and not in
browsers that only have Asyncify, until the Asyncify variant is built. Its measurements, flags and
page-level choice are in [asyncify.md](asyncify.md).

**Natively (SDL2)** the same waits call `SDL_Delay` and pump SDL events. **Headless** runs use a
deterministic virtual clock (section 5) and no platform at all: waits jump the clock, the input
script is applied at safe points as its times pass, screenshots come from the host video.

## 5. The native clock

`m->virtual_us` stays the one clock every module reads (`machine_now_ns`, the audio mixer, the BIOS
tick, the TSNEXEC tick counter maintained by the client). In native mode it advances by:

- **Work charges.** Each dispatched call (lifted entry, service stub, trivial stub) and each host-API
  call that stands for a port access charges `call_cost_ns` (default 1000 ns, the same as one emulated
  ISA bus cycle today). This keeps loops that measure themselves meaningful (the speed test above) and
  makes busy-polling loops consume time.
- **Waits.** `native_idle` and `native_wait_until` move the clock to the wake-up time.
- **Pacing (real-time mode).** At safe points the clock is compared with the wall clock. If it is ahead
  by more than 10 ms the platform sleeps the difference (in the browser, a JSPI suspension), so a busy
  script costs CPU in proportion to its work, not 100 %. If it falls behind by more than 50 ms (slow host,
  background tab) it jumps forward and the lost time is dropped, as the frame clock does today.
- **Deterministic mode (headless).** No wall clock: charges and waits only. A run is then independent of
  host speed; only the moments the server's bytes arrive (polled at safe points) tie it to wall time, as
  in today's headless hybrid runs. Headless hybrid and native runs start that clock at `INN_EPOCH`
  (default 750000000, 8 Oct 1993 UTC). An empty `INN_EPOCH` uses the host clock. The same epoch makes
  two runs of one build repeat. Hybrid and native still start about 0.4 s apart, because they charge
  work differently: a periodic host message can fall on opposite sides of a scripted event. Move the
  event off that boundary.

Timer interrupts are due by this clock: IRQ 0 every `divisor / 1193182` s of the PIT divisor the client
programmed (section 9). Several overdue ticks are delivered one per safe point, at most 5 in a row;
older ones are dropped.

## 6. Interrupts without a CPU

**Safe points** are the only places an ISR may run in native mode:

1. every dispatched call (`guest_call_far`, `guest_call_near`, `guest_int`, and the static direct calls
   of section 7), before the callee runs, exactly where the CPU would have taken the interrupt at the
   callee's first instruction;
2. every host-API call that replaces a port access (the original could be interrupted there too);
3. every wait.

A safe point does three cheap things: charge the work time; if `IF` is set in `m->cpu.flags`, deliver
the highest-priority due interrupt; every 4 ms of wall time (counted every 256 safe points), yield to the
platform so it can present a frame, pump events and audio, and in the browser suspend for a turn.

Lifted code already models `CLI`/`STI` by clearing and setting `F_IF`; the safe point respects it. An
ISR delivered at a safe point runs on the guest stack at the current `SS:SP`, as the hardware would; the
lifted C caller's locals are on the host stack and unaffected. This is the rule device replay already
uses for interrupts raised by lifted C with `IF` set.

**The interrupt model** (`native_irq.c`) keeps the 8259 pair's state as plain data: request, in-service
and mask bits for 16 lines, fully nested priority, non-specific and specific EOI. The client reaches it
through the host-API seam (section 9: `native_pic_eoi`, `native_pic_set_mask`), not through ports.
Sources:

| Line | Raised by |
|---|---|
| IRQ 0 timer | the clock, at the programmed rate |
| IRQ 1 keyboard | `host_input`: one scancode byte at a time, latched for the ISR's data read |
| IRQ 7 Sound Blaster | `host_audio`, at the end of a digital block (only if a driver starts one): checked at every safe point, and waits wake for it |
| mouse | not an IRQ: `mouse.c` events are handed to the client's INT 33h handler at a safe point |
| COM1 | none: the link is pumped at safe points (section 9, `host_link`) |

**Delivery** is `native_int(m, vector)`: push `FLAGS`, a return sentinel `CS:IP`, clear `IF` and `TF`,
and resolve the IVT entry with the dispatcher. The vectors are whatever the programs installed: the
lifted timer, keyboard and mouse handlers of LSCITV and its drivers, TSNEXEC's INT 14h handler, and
the runtime's BIOS handlers. A BIOS handler that is more than a service stub (the INT 08h ROM routine
calls INT 1Ch and sends an EOI; every IRQ routine sends an EOI) gets a C equivalent registered at its ROM
address when the BIOS emits it (`native_rom_lift`, our own code). A vector that resolves to nothing native is
fatal in strict mode, with its address and owning module.

**DOS and BIOS calls made by lifted code** (`guest_int(m, 0x21)` and friends) go through the same
dispatcher: the IVT points at a service stub, the stub's callback runs directly, and the stub's `IRET`
is applied in C. A callback that returns `CB_RETRY` makes the dispatcher wait (`native_idle`) and call
again.

## 7. Calls between lifted functions

**Decision: the emulated register file and the guest stack remain the calling convention.** A lifted
entry stays `void fn(Machine *m)`, entered with its arguments and a return address on the guest stack,
leaving results in `AX`/`DX` (and flags or other registers where its binary's convention says so).

Why:

- **Verification keeps working unchanged.** Shadow verification compares the original and the lifted
  function at the entry boundary: memory, registers outside the scratch set, the stack. A C-argument ABI
  would need a second copy of every function or a wrapper per function.
- **Indirect calls dominate the interfaces.** Kernel tables, driver function tables, timer callback
  tables, TSNEXEC's export table, client callbacks and the IVT hold far pointers in guest memory. Every
  callee reached through them needs one uniform signature and the guest-stack convention anyway.
- **Register-exact drivers.** MODEM, NOBRK, VGA320, IBMKBD and ADL are lifted register-exact; their
  callers read whatever registers and flags they leave.
- **It is cheap.** The prototype's dispatch is a bitmap test and a binary search over about 1000 active
  entries; the logon ran 2.3 times faster with no lifted code changed.

**The dispatcher** (package 2) decides, for a call to `seg:off`:

| Target | Action |
|---|---|
| active lifted entry with matching signature bytes | run the C function now (through shadow verification when it is on) |
| `JMP` (near, short or far) whose destination is resolvable | follow it (driver entry thunks) |
| bare `RET`, `RETF`, `RET n`, `RETF n`, `IRET` | apply the return in C |
| service stub (`FE 38 id CF/CB`) | run the callback, apply the stub's `IRET`/`RETF` |
| registered ROM lift | run its C |
| anything else | hybrid and mixed mode: interpret as today; native strict: fatal; native non-strict (development): interpret with the devices suspended, counted per module |

After the C callee returns, the dispatcher checks that `CS:IP` is the return sentinel (far) or the
caller's entry (near). If not (a region lift that leaves at another instruction, a longjmp-style
return), it falls back to the nested run until the sentinel is reached, which in native mode means the
fallback rules above.

**Static direct calls** (package 2, second half): call sites that name a constant target which is a
lifted entry (`lsci_call(m, SEG, OFF)`, `TSN_CALL_NEAR`, `guest_call_near(m, OFF)`) are rewritten by a
tool to name the lifted function, through a generated id table. They still push arguments and call
through the dispatcher's entry path (verification, synthetic-call triggers, signature check), so they are
equivalent at run time; the gain is a compile-time call graph, compile errors when a callee is renamed,
and a list of the calls that still reach unlifted code.

**Migration to C arguments** is optional and per function, by the pattern lifted code already uses: a
plain C helper (`mem_far_add`, `c_strlen`) holds the body; the hooked entry is a thin wrapper that reads
its arguments from the guest stack, calls the helper and sets `AX`/`DX`. Lifted callers call the helper.
The wrapper stays the verified entry, so the helper is verified through it. Nothing in M9 depends on it.

## 8. Program control

Four pieces of the client were not lifted by decision because they run or end other programs: TSNEXEC's
start-up and main, its TSN.PRG interpreter and EXEC wrapper, LSCITV's start-up and main with the VM
start-up and InvokeMethod, and both programs' exit. In native mode they are written as native C
(package 3):

- **Native programs.** A program's native start is a lifted entry at its start address
  (`LIFT_FAR_NOSHADOW`). The program-control lifts live in their own lifted directories whose
  `module.txt` carries a third line, `native`: the generator marks those entries and the runtime arms them
  only in native builds or when `INN_NATIVE_CONTROL=1`, so the hybrid build (and its browser build) is
  unchanged by them.
  Starting a program (from the frontend or from EXEC) always loads its image through the DOS loader, so
  its data, PSP, environment and memory blocks are exactly where the hybrid build has them; then control
  goes to the start address, where the dispatcher finds the native start. A program without one (a game)
  runs on the emulator (section 11). Because the hook is an ordinary lifted entry, the native program
  control also runs inside the hybrid build (`INN_NATIVE_CONTROL=1`, headless or SDL), with emulated
  devices and every other lift shadow-verified around it: that is how it is developed and checked before
  the rest of the native build exists.
- **InvokeMethod becomes a verified lift** (in a native-only table, since in the hybrid build the original
  start-up's call to it would pull the whole session into C). Its body runs the native VM driver until the
  method returns. The calls kernels make (Animate and others) return, so they are shadow-verified like any
  function, which checks the C kernel-call epilogue against the original on every scenario. Only the
  VM start-up's call, which runs the play loop for the whole session, uses the same body without the hook.
- **Start-up equivalence.** The native start-up code must leave guest memory and DOS state as the
  original start-up does (DGROUP variables, argument vectors, the stack fill pattern, memory block sizes).
  It is checked by dumping both builds at the same point (the first EXEC; LSCITV's first kernel call) and
  comparing (section 12).
- **The VM driver.** LSCITV's bytecode interpreter is already a region lift that leaves at every kernel
  call and at returns it does not model. The native driver loops: run the region lift; at a kernel call,
  perform the call through the kernel table (dispatcher) and the original's kernel-call epilogue in C,
  then enter the next opcode handler; at an InvokeMethod return, return from the native InvokeMethod.
  Kernels that send messages (InvokeMethod from Animate and others) recurse on the host stack.
- **Exit.** A program's exit runs its exit handlers (guest far pointers, through the dispatcher), then
  the DOS terminate service, which frees the process and unwinds the host stack (wasm longjmp) to the
  native EXEC that started it, returning the exit code. The VM's Restart kernel unwinds to the native
  LSCITV main in the same way.

## 9. Host APIs

Lifted driver code reaches devices only through a seam: the ~40 port-access sites in the INN drivers
(VGA320's DAC and retrace, the OPL write helper, the PIT programming and EOIs in LSCITV's timer, IBMKBD's
data read, acknowledge and EOI, the UART helpers of MODEM and NOBRK, the SB DSP and DMA) become calls of
small semantic helpers in the private headers, compiled one of two ways:

- `INN_NATIVE_HW=0` (hybrid): the helper performs the original port sequence, byte for byte. Shadow
  verification and every scenario stay as they are.
- `INN_NATIVE_HW=1`: the helper calls the host API below.

The host APIs are ours and public (`runtime/src/native/`). Each call is a safe point. Each has two
backends: the **host** backend (native mode) and the **device** backend (hybrid-hw builds, section 10),
which drives the emulated device directly in C and records a semantic event for verification.

### native.h (package 11)

```c
typedef struct NativeConfig {
    bool deterministic;        /* headless: clock advanced only by charges and waits */
    bool strict;               /* fatal when guest code would be interpreted in native mode */
    uint32_t call_cost_ns;     /* work charge per dispatched call or host-API call (1000) */
    uint32_t yield_every_us;   /* wall time between cooperative yields (4000) */
} NativeConfig;

void native_init(Machine *m, const NativeConfig *cfg);
bool native_active(const Machine *m);              /* INN runs natively (not a game on the emulator) */
void native_set_active(Machine *m, bool on);       /* mixed-mode switch (package 10b) */
void native_safepoint(Machine *m);                 /* charge, deliver a due IRQ, maybe yield */
void native_charge(Machine *m, uint32_t ns);
void native_idle(Machine *m);                      /* STI; HLT: run until one interrupt was delivered */
void native_wait_until(Machine *m, uint64_t virtual_us); /* deliver IRQs while waiting */
void native_yield(Machine *m);                     /* present/pump/suspend once */

void native_irq_raise(Machine *m, int irq);        /* 0-15, edge */
void native_irq_lower(Machine *m, int irq);
void native_pic_eoi(Machine *m, int chip, int irq); /* chip 0 master, 1 slave; irq < 0: non-specific */
void native_pic_set_mask(Machine *m, int chip, uint8_t mask);
uint8_t native_pic_mask(const Machine *m, int chip);
void native_int(Machine *m, uint8_t vector);       /* INT n without a CPU */

/* ROM routines of our own BIOS that are more than a service stub (an IRQ handler's callback, EOI and
 * IRET; the timer's INT 1Ch call) are registered with a C equivalent for the native dispatcher. */
void native_rom_lift(Machine *m, uint16_t seg, uint16_t off, LiftedFn fn, const char *name);
```

### native_exec.h (package 10a)

```c
/* Loads and runs a program as DOS EXEC function 0 would and returns its exit code (AH=4Dh), or -1 with
 * *dos_error set. Native start: called directly; otherwise the child runs on the emulator until it
 * terminates. The child's termination unwinds to here (setjmp; wasm longjmp in the browser). */
int native_exec(Machine *m, const char *dos_path, const uint8_t *tail /* length byte, text, CR */,
                uint16_t env_seg, uint16_t *dos_error);
```

### host_platform.h (package 11; the frontend implements it)

```c
typedef struct HostPlatform {
    void *ctx;
    uint64_t (*now_us)(void *ctx);                    /* monotonic wall clock */
    void (*sleep_us)(void *ctx, uint64_t us);         /* 0: yield once; browser: JSPI suspension */
    void (*poll_events)(void *ctx, Machine *m);       /* feeds host_input_* */
    void (*present)(void *ctx, const uint32_t *argb, int w, int h);
    void (*audio_pump)(void *ctx, Machine *m);
    bool (*quit_requested)(void *ctx);
} HostPlatform;

void native_set_platform(Machine *m, const HostPlatform *p); /* NULL: headless, deterministic */
```

### host_video.h (package 4)

In native mode `A0000`-`BFFFF` are plain memory: mode 13h pixels at `A0000` (linear, as chain-4 maps
them), text cells at `B8000`. The host video owns the mode, the 256-entry DAC (6-bit values), the CRTC
start address and the presentation.

```c
enum { HOST_VIDEO_TEXT80 = 0x03, HOST_VIDEO_320X200X8 = 0x13 };

void host_video_init(Machine *m);
void host_video_set_mode(Machine *m, uint8_t mode, bool clear);
uint8_t host_video_mode(const Machine *m);
void host_video_set_dac(Machine *m, uint8_t first, int count, const uint8_t *rgb6); /* 3 bytes each */
void host_video_get_dac(Machine *m, uint8_t first, int count, uint8_t *rgb6);
void host_video_set_start(Machine *m, uint16_t start);   /* CRTC start address (shake, scroll) */
void host_video_wait_retrace(Machine *m);                /* present if due, wait for the 70 Hz boundary */
bool host_video_in_retrace(Machine *m);                  /* for code that polls the status bit */
void host_video_render(Machine *m, uint32_t *argb, int *w, int *h); /* VGA_MAX_W * VGA_MAX_H buffer */
int host_video_write_ppm(Machine *m, const char *path);
void host_video_to_vga(Machine *m);      /* mixed mode: hand mode, DAC and pixels to the VGA device */
void host_video_from_vga(Machine *m);    /* and take them back */
```

### host_timer.h (package 11)

```c
void host_timer_set_divisor(Machine *m, uint16_t divisor); /* PIT channel 0, mode 3; 0 = 65536 */
uint16_t host_timer_divisor(const Machine *m);
uint64_t host_timer_next_due_us(const Machine *m);         /* for native_idle */
void host_timer_speaker(Machine *m, uint16_t divisor, bool on); /* channel 2 square wave and port 61h gates */
```

### host_audio.h (package 5)

The OPL2 core, mixer and sinks are the existing ones (`opl.c`, `audio.c`, SDL audio, WebAudio through
SDL); only the register access changes from ports to calls, at the native clock's time.

```c
void host_audio_opl_write(Machine *m, uint8_t reg, uint8_t value);
uint8_t host_audio_opl_status(Machine *m);                 /* timer flags, for the detection probe */
/* Sound Blaster digital output, 8-bit mono from guest memory; raises IRQ 7 at each block end. */
int host_audio_pcm_start(Machine *m, uint32_t linear, uint16_t bytes, uint32_t rate_hz, bool auto_init);
void host_audio_pcm_stop(Machine *m);
/* What SNDBLAST's init and play paths need besides (one DSP or DMA port sequence each). */
uint8_t host_audio_sb_reset(Machine *m);                   /* AAh */
uint8_t host_audio_sb_identify(Machine *m, uint8_t v);     /* E0h: ~v */
uint16_t host_audio_sb_version(Machine *m);                /* E1h: 0201h */
void host_audio_sb_speaker(Machine *m, bool on);           /* D1h / D3h */
void host_audio_sb_time_constant(Machine *m, uint8_t tc);  /* 40h; pcm_start with rate_hz 0 uses it */
uint32_t host_audio_sb_rate(uint8_t tc);                   /* 1000000 / (256 - tc) */
void host_audio_pcm_mask(Machine *m);                      /* channel 1 masked alone: the block stalls */
uint8_t host_audio_sb_ack(Machine *m);                     /* 22Eh read */
/* One byte to the MPU-401's MIDI OUT (MT32.DRV's 330h data write); see runtime.md, "MPU-401". */
void host_audio_midi_write(Machine *m, uint8_t byte);
```

**Sound Blaster output.** In native mode a mixer source in `audio.c` (the host PCM) plays 8-bit unsigned
mono from guest memory with the DSP's model (`sb.c`): byte i of a block is fetched i + 1 periods after the
start, the output ramps to each byte over one period and is AC-coupled. The period is the time constant's
(256 - tc µs; `rate_hz` is rounded to a time constant, so native and emulated play at the same rate). The
buffer is read as DMA channel 1 would: wrapping within its 64K page, restarting at its end for auto-init.
Each block end raises IRQ 7 in the native IRQ model at the first safe point at or after it
(`host_audio_poll`); `native_idle` and `native_wait_until` wake for it. Single-cycle blocks stop at their
end, auto-init blocks repeat, `host_audio_pcm_stop` (D0h, then the mask) and `host_audio_pcm_mask` end the
transfer with no further IRQ. The host PCM's state lives in the machine's mixer and is part of the
snapshots shadow and synthetic runs take (`sound_snapshot_*`, with `sb.c` and `dma.c`). In emulated mode
(hybrid-hw builds, games in mixed mode) each call performs the port sequence the original drivers use
(`host_audio.h` lists it), so the emulated DSP and DMA controller play it and a shadow run canonicalises
it into semantic events (docs/lifting.md, "Semantic port comparison").

### host_input.h (package 6)

```c
/* Platform side: the frontend and the input script call these in every mode. */
void host_input_key(Machine *m, uint16_t set1_scancode, bool down);  /* 0xE0xx = E0-prefixed */
void host_input_mouse_motion(Machine *m, int x, int y, int frame_w, int frame_h);
void host_input_mouse_button(Machine *m, int button, bool down);
/* Guest side, native mode: the byte delivered with the current IRQ 1 (port 60h equivalent). */
uint8_t host_input_kbd_data(Machine *m);
void host_input_kbd_ack(Machine *m);                       /* port 61h strobe: nothing to do */
/* Native core, at every safe point: latch the next key byte and raise IRQ 1, or hand a mouse event to
 * the INT 33h handler. */
void host_input_poll(Machine *m);
```

In emulated mode the platform calls go to the 8042 and INT 33h models as today; in native mode keys are
queued as scancode bytes and delivered one per IRQ 1, and mouse events are handed to the client's INT 33h
event handler (registers as the INT 33h trampoline sets them) at a safe point.

### host_link.h (package 7)

The link is the existing `SerialLink` (TCP natively, WebSocket in the browser, including the JSON
control channel for game saves). The client keeps its whole link stack: TSNEXEC's framing, acknowledgement
and queues, and the MODEM driver's dial logic (Hayes commands, PAD log-on), so `innkeeperd` sees the same
byte stream as from the hybrid build. Only the UART goes: the driver's transmit and receive rings in guest
memory are filled and drained by a pump instead of the UART interrupt.

```c
int host_link_open(Machine *m, const char *spec);      /* host:port or ws:// / wss:// URL */
bool host_link_carrier(Machine *m);                    /* connected: DCD, DSR and CTS */
int host_link_send(Machine *m, const uint8_t *buf, int len); /* bytes taken; fewer while connecting */
int host_link_recv(Machine *m, uint8_t *buf, int cap);
void host_link_set_rate(Machine *m, uint32_t baud);    /* the driver's line rate */
void host_link_set_dtr(Machine *m, bool on);
void host_link_break(Machine *m);                      /* sends the CR stand-in innkeeperd expects */
/* The driver's ring pump (the UART interrupt's work, done without a UART) lives with the lifted driver
 * and registers itself here; host_link_pump runs it at safe points in native mode and as a machine
 * ticker in emulated mode (games call TSNEXEC meanwhile). */
void host_link_set_pump(Machine *m, void (*pump)(Machine *m));
void host_link_pump(Machine *m);
```

`host_link_send` and `host_link_recv` pace the link at the line rate the driver programmed: one byte per
byte time (10 bits at that rate) in each direction, as the emulated UART does, so the client's timing and
the games' timer-seeded behaviour stay close to the hybrid build. Pacing is the default. `INN_LINK_PACING=0`
makes the link unpaced.

### Files

Unchanged: the DOS layer (`dos_*.c`) already maps the C: drive to host files (stdio, MEMFS/IDBFS in the
browser), and `dos_host_file_closed` already feeds the save sync. Package 8 adds C entry points for
EXEC and terminate so native code need not go through INT 21h for them, and confirms that EMS (INT 67h)
and XMS (INT 2Fh 43h) report none in every path. The hybrid build's opt-in EMS and XMS (`--ems-kb`,
`--xms-kb`, [runtime.md](runtime.md) "Conventional memory") are ignored by native builds, so the client's
probes, which run as the same lifted C in both builds, find neither.

## 10. Build configurations

| Configuration | CMake | INN code | Devices | Use |
|---|---|---|---|---|
| hybrid (default) | none | lifted C + interpreter glue; direct calls (once package 2 passes) | emulated | today's build; must keep passing every scenario with the same verification results |
| hybrid-hw | `-DINN_NATIVE_HW=ON` | as hybrid, but drivers call the host APIs, device backend | emulated, driven in C | shadow-verifies each converted driver function with semantic port comparison (section 12) |
| native | `-DINN_NATIVE=ON` (implies `INN_NATIVE_HW`) | native programs, no interpretation | host APIs; emulated devices only in mixed mode | the M9 product |
| web hybrid | `emcmake`, `INN_WEB_SUSPEND=ASYNCIFY` or `JSPI` | as hybrid | emulated | today's browser build |
| web native | `emcmake -DINN_NATIVE=ON` (requires `JSPI`) | as native | as native | the M9 browser build |

`INN_NATIVE` adds `runtime/src/native/*.c`, defines `INN_NATIVE=1` and `INN_NATIVE_HW=1` for the runtime
and the lifted tree, links with `-fwasm-exceptions -sSUPPORT_LONGJMP=wasm` in the browser, and makes the
frontend boot through the native path. Runtime switches: `INN_NATIVE_STRICT=0` allows the interpreter
fallback for development (with a per-module count of interpreted instructions at exit),
`INN_NATIVE_DETERMINISTIC=1` forces the virtual clock with a window, `INN_NATIVE_CALL_NS` sets the
charge. The public build without the private tree still builds every configuration (no lifted code: the
native boot reports that TSNEXEC has no native entry and runs it on the emulator).

## 11. Mixed mode: games on the emulator

TSNEXEC starts every program through EXEC. In native mode its native EXEC (`native_exec`, packages 10a and 10b):

1. **Loads** the program with the DOS loader into the block DOS gives it. Native code never takes guest
   DOS memory for its own use (host state is on the host heap), and the native TSNEXEC and LSCITV make the
   same DOS memory calls as the originals, so at a game's EXEC the arena holds exactly what it holds in the
   hybrid build: DOS data, TSNEXEC's PSP and image shrunk to its resident size, the MODEM driver's block,
   and nothing of LSCITV (it has exited). The game loads at the same segment.
2. **Native program?** Call its entry (LSCITV). Otherwise switch to emulated mode:
   - video: `host_video_to_vga` (mode, DAC, pixels into the planes) so the screen does not flash;
   - timer: the PIT device gets the native divisor; the PIC device gets the native masks;
   - input: platform input now goes to the 8042 and INT 33h models;
   - link: `host_link_pump` is registered as a ticker, so the rings keep moving while the game runs;
   - the child's INT 22h return address is a service stub that marks the child finished.
   The frontend's frame loop then runs inside `native_exec` (`machine_run_for` per frame, `vga_render`,
   audio pump, wait), as today's hybrid frontend does, until the child has terminated.
3. **The INT 14h trap needs no new mechanism.** TSNEXEC installed its lifted INT 14h handler in the IVT,
   and its image, export table and hooks stay armed while it is resident. A game's `INT 14h` and its far
   calls through the export table reach the hooked addresses and run TSNEXEC's lifted C from the
   interpreter, as in the hybrid build; that C calls the driver functions, which use the host link, and the
   game's own callbacks through `guest_call_far`, which the interpreter runs.
4. **Back.** `host_video_from_vga`, the native timer takes the PIT's divisor and the native IRQ model the
   PIC's masks and pending lines, the ticker is removed, and `native_exec` returns the exit code to
   TSNEXEC, which runs the next TSN.PRG program (LSCITV again) natively. A vector the game left pointing
   into its freed memory is reported, not called.

Lifted game code (CGENN, DARKSTRT, TWGENN, FATES, RB) runs in mixed mode exactly as in the hybrid build,
through hooks.

**The hand-off, as implemented** (`native_handoff_to_emulator` / `native_handoff_from_emulator` in
`native_exec.c`, `native_irq_to_pic` / `native_irq_from_pic` in `native_irq.c`; only when INN runs
natively, so a child started by the emulated machine changes nothing). To the emulator, in this order:

1. Sound Blaster (`host_audio_to_devices`): the host PCM's block becomes DMA channel 1's
   registers (base, current address and count, page, mode 49h or 59h; masked at terminal count) and the
   DSP's voice (bytes left in the block, time of the next byte, period, auto-init, speaker, the output's
   ramp and filter), so the sound goes on without a seam. A block end the native model has not raised yet
   is raised there and goes to the PIC with step 5. The host source is left idle and silent. OPL music
   needs nothing (one chip, one mixer).
2. `m->native` and `m->native_video` off; `host_video_to_vga`: mode, DAC and the 320x200 pixels or text cells.
3. PIT channel 0 gets the native divisor and keeps the native timer's phase (its next IRQ 0 falls when the
   native one was due). Native ticks counted but not yet raised are dropped.
4. Port 61h: bits 0 and 1 (speaker gates) are written through the port.
5. PIC masks, requests and in-service bits of both chips go to the device; the native copy of requests and
   in-service bits is cleared, so nothing is delivered twice.
6. Keyboard scancode bytes the native queue had not delivered (and a latched byte the ISR had not read) go
   to the 8042; the native queue is emptied.
7. Mouse events still queued raise IRQ 12 for the INT 33h trampoline. Position, buttons and handler stay in
   the one mouse device.
8. `host_link_pump` is registered as a machine ticker. The IVT is snapshotted.

Back: the ticker is removed; the VGA device's mode is taken by `host_video_from_vga` (a mode the host video
does not carry, such as 12h, is replaced by text mode and logged); the native timer takes the PIT's divisor
(a program that reprograms the timer and exits leaves it so, as on hardware); port 61h's gate bits come
from the PIT device; the native masks come from the PIC while the PIC's requests and in-service bits are
dropped (a bit left in service would block its line for good); a block the DSP is playing comes back
to the host PCM with its DMA position (`host_audio_from_devices`; the DSP is left idle with its IRQ line
low and channel 1 masked; a channel set to count down stops the block and is logged; a block end the game
had not taken is dropped with the PIC's requests); the 8042 is emptied; each IVT entry the
game changed that now points into free DOS memory is set back to its value at hand-off and logged as
`native: vector %02X left at %04X:%04X by the game`; an INT 33h handler in free memory is dropped and
logged. Not carried: emulated keyboard bytes the game left unread, and the PIT's other channels.

While the game runs, `native_exec` steps the machine and calls the nested yield the frontend installed:
a headless run applies the input script and ends at `--run-ms`; a windowed run keeps emulated time with
the wall clock, polls events, pumps audio and presents the VGA device's frame. `test_mixed` covers a
child that reprograms the timer, hooks vector 1Ch and exits without restoring it.

## 12. Verification

| Layer | Build | What | Pass |
|---|---|---|---|
| shadow, live | hybrid with direct calls | every lifted function as today (`tools/verify_lifted.sh`, all INN, Yserbius, Twinion and Red Baron scenarios, synthetic call lists) | per-function checked counts at least the baseline's, no mismatch the baseline does not have |
| shadow, semantic | hybrid-hw | each driver function whose port sites became host-API calls: the original's recorded port I/O and the lifted run's host-API events are both turned into semantic events (OPL register writes, DAC writes, PIT divisors, EOIs and masks, keyboard data reads; OPL status reads used as delays dropped) and compared, with memory, registers and VGA state compared as usual | 0 mismatches on every scenario that reaches the function |
| start-up dumps | hybrid and native | memory below `A0000`, the MCB chain, DOS handles and the IVT at TSNEXEC's first EXEC and at LSCITV's first kernel call (`INN_DUMP_AT`) | identical outside a listed set (BIOS tick, dead stack below SP) |
| differential, end to end | native vs hybrid, same scenario script, headless | (1) the server's view: the ordered list of messages each side sent (`innkeeperd` logs every message), (2) screens: at checkpoints on still screens exact; on animated screens the same palette and at most 3 % of pixels different, (3) files the client wrote (player, mail, `LANDADDR`), (4) EXEC sequence (program, tail, directory) and exit codes | all four match on every INN scenario; the game scenarios also reach their final checkpoint |
| no interpretation | native strict | the interpreter is never entered while `native_active` | the run completes; an attempt is fatal with the address and module |
| browser | web native in headless Chromium (Playwright, as `docs/web.md`) | log-on to the town map; a game round trip; frame pacing with `perf=1` | map reached; speed 1.000; no long frames over 100 ms outside loading |

The scenario scripts are shared: their times are in emulated milliseconds in both builds. Native
headless runs use the deterministic clock, so a mismatch normally reproduces.

## 13. The prototype

Branch `m9-proto` (local, not pushed) has the measured runtime change: direct dispatch of lifted entries
and service stubs from `guest_call_far`, `guest_call_near` and `guest_int` (`INN_DIRECT_CALLS=1`,
`2` lifted only, `3` stubs only), the fallback to the CPU path when an interrupt is due or a service
blocks, the 1 µs charge per direct call, and `INN_INTERP_PROFILE=<file>`, which counts interpreted
instructions per 16-byte paragraph and writes them with the modules' load segments at exit. Package 2
starts from it.

## 14. Risks

| Risk | Where | Mitigation |
|---|---|---|
| Native program control misses a side effect of the original start-up or exit (a DGROUP variable, a vector, a block size), which shows up much later | package 3 | start-up dumps compared byte for byte; EXEC sequence compared on every scenario |
| The VM kernel-call epilogue and the opcode re-entry are hand-written from disassembly | package 3 | the native VM driver is the body of the lifted InvokeMethod, which is shadow-verified on every scenario in the hybrid build with `INN_NATIVE_CONTROL=1`; the region lift keeps its own verification |
| Interrupt latency differs: ISRs run only at safe points | package 11 | safe points at every call boundary are denser than the original's interrupt windows matter for; device replay already accepts this rule; differential runs catch timing-dependent behaviour |
| Work charge too small or too large changes the scripts' speed measurement and animation detail | package 11 | charge configurable; differential screens on animated scenes; compare the speed-dependent script results in both builds |
| JSPI unavailable (non-Chromium browsers), or wasm exceptions with JSPI misbehave | package 11 | prove suspension inside a setjmp scope early (unit test under Node with JSPI); keep the hybrid web build as the fallback product; without JSPI, the Asyncify variant of [asyncify.md](asyncify.md) |
| The link without UART pacing: bursts where the original trickled at 2400 baud | package 7 | the dial's own delays stay; `innkeeperd` takes any rate; traffic compared message by message |
| Mixed-mode hand-off leaves device state inconsistent (PIT rate, masks, video mode, a vector into freed memory) | package 10b | explicit hand-off lists both ways; game scenarios end to end; vectors checked on return |
| Hidden port I/O or interpretation left in a rarely used path (Shake, Scroll, error paths) | packages 4, 9 | strict mode makes it fatal with the address and module; VGA320's unlifted Shake and Scroll, which no INN script calls, are reported if ever reached |

## 15. Work packages

Detailed briefs are private. Order and groups (packages in one step touch disjoint files):

| Step | Packages |
|---|---|
| 0 | **9a** harness tools: scenario suite runner, verification diff, comparison tools, baselines |
| 1 | **2a** direct dispatch in the runtime (hybrid, default on once verified); **8** DOS C entry points, EMS/XMS checks |
| 2 | **11** native core, build configurations, platform; **10a** `native_exec` and native-only lift tables; **9b** semantic port verification; **2b** static call-site conversion |
| 3 | **3** program control; **4** video; **5** sound; **6** input; **7** link |
| 4 | **10b** mixed mode: device hand-off, games from the native build |
| 5 | **9c** final verification campaign, browser checks, documentation |

## Status

Measured 2026-10-16. The INN client runs native. Games run on the emulator in mixed mode. Still open:
Sound Blaster digital audio on the native path, and an Asyncify fallback for browsers without JSPI.

| Section | Where it stands |
|---|---|
| 1 Scope | In place for the INN client. Games are not lifted onto the host APIs. |
| 2 Prototype | Historical. Direct calls are the hybrid default. |
| 3 Execution model | Native programs run as C; the interpreter is the strict-mode fatal path and the game path. |
| 4 Main loop | SDL and JSPI both drive the native loop. The native web build is JSPI only. |
| 5 Clock | Work charges, waits and pacing are in. `INN_EPOCH` fixes the headless clock. Hybrid and native differ by about 0.4 s at start-up. |
| 6 Interrupts | IRQ 0 and the other lines are delivered at safe points. |
| 7 Direct calls | On by default in the hybrid build. |
| 8 Program control | TSNEXEC and LSCITV start and exit as C. A native run no longer falls back to the emulator for them. |
| 9 Host APIs | Video, OPL music, the timer, input and the link are host calls. The link is paced unless `INN_LINK_PACING=0`. Digital Sound Blaster audio is not. |
| 10 Builds | Hybrid, native and web native (`-DINN_WEB_SUSPEND=JSPI -DINN_NATIVE=ON`) all build. |
| 11 Mixed mode | Yserbius and Twinion scenarios reach their checkpoints from the native build and return. |
| 12 Verification | Campaign below. |
| 13 Prototype | Unchanged, on `m9-proto`. |
| 14 Risks | The start-up dumps are not byte-identical (below). Timing-dependent scenarios are listed below. JSPI is in; Asyncify is not. |
| 15 Packages | 9a through 10b are merged. 9c is this measurement. |

**Strict native.** All 30 scenarios in `M9-NATIVE.txt` finish with no fatal. Strict mode is the default (`INN_NATIVE_STRICT=0` is the fallback).

**Hybrid against native** (`tools/m9_compare.sh`, 30 scenarios). 26 pass. The failures, not patched in other packages' code:

| Scenario | What differs |
|---|---|
| `inn_casino` | Compared in part (`private/lifted/scenarios/inn_casino.compare`). `Random` is seeded once from the TSNEXEC tick counter and then drawn once per animation cycle (about 3300 draws hybrid, 3900 native in one run), so a seed one tick apart or a different draw count changes the roulette result (`02 .. 68 00` message, bytes 8-9) and the blackjack cards, and the scripted blackjack clicks then play different hands. Re-timing cannot fix it: the seed tick differs by one in every build pair, and the draw counts differ by 20%. The sidecar masks the roulette result, stops the traffic comparison before the first blackjack message and compares the five screens taken before blackjack; blackjack and the exit are checked only by the hybrid suite. |
| `tw_combat` | Traffic and three screens. The encounter varies between runs of one build. Known flaky. |
| `ys_create` | Passes. The old `final.png` difference (3.19 %, box 49,0-219,141) was animation phase, not a mixed-mode fault. The Yserbius road scene animates lava and smoke on wall-clock time (a 500 ms shot series from one build shows about 25 distinct frames in 26 s, drawn from a small pool), so two builds that run a few hundred ms apart sometimes hold different frames; a run of the old scenario gave three different final frames across six runs. The scenario now ends in the tavern, which is still, so `final.png` is exact between the builds. The road scene is still visited, and its traffic compared. No runtime change. `tw_create` had the same road-scene animation phase on `final.png` (one run in three) and now also ends in the tavern. |
| `inn_sierraland` | One run diverged on the way out of 3-D golf (extra leave and occupancy messages, and `sl_end.png`). A second run passed. |

`ev_input` used to race one land-occupancy poll. The scripted walk now starts 8 s later, and hybrid and native traffic match. `mem_swap` (`--conv-kb 520`) passes.

**Files.** After `mem_mail` and after `inn_casino`, `INN/PLAYER.DIR`, `INN/LANDADDR`, `INN/LANDADDR.TIM` and the persona `.DTA` files are byte-identical between the two builds.

**Start-up dumps** (`INN_DUMP_AT=exec:1,kernel:1` on logon). Registers, the MCB chain and the DOS handle list match. In the kernel dump, INT 22h is the native terminate stub. Ignoring the BIOS tick, the unused stack immediately below SP, and memory from `A0000` up, a few ranges in the DOS file image and in DGROUP still differ. Left as they are.

**Hybrid suite** with direct calls, 20 calls per function, against the baseline (`--allow` from `work/m9/baseline/ALLOW`): 31 scenarios ok, diff 0 FAIL. Warnings are new keys and, on `inn_casino`, the file and swap entries that scenario no longer reaches. Those entries are checked on `mem_swap` (live) and by `private/lifted/synth/lscitv_file.txt` (synthetic), each with 0 mismatches.

**Browser** (native JSPI, headless Chromium, `?com1=ws://…&perf=1`). The land directory arrives at 25 s and the town map is up at the 50 s shot. The Yserbius gallery is up at 90 s and the town map again at 135 s. `perf:` speed stays between 0.998 and 1.003 (most windows 1.000). No `long frame` line; the longest busy slice in those windows is 17 ms. `executed` counts guest instructions, so it is about 0 while INN is native and about 15 MIPS while Yserbius runs on the emulator. `--perf` on the native loop is taken at each platform sleep, which is the same report `frame_clock_run` prints for the emulated loop.
