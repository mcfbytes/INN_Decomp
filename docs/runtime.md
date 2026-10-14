# dosrt: the runtime

`runtime/src` is an emulated real-mode PC that the interpreter, recompiled code and lifted code
share. It is ours (no Sierra code or data), builds natively with SDL2 and with Emscripten.

## Machine state

`Machine` (`machine.h`) owns everything a guest can observe: `Cpu` (386 register file, segment
bases cached, flags kept eagerly), 16 MB + 64 KB of memory (`a20` selects 1 MB wrap), the port
table, the callback table, the IRQ-pending mask and `virtual_us`, the emulated clock.

Because all three tiers read and write this one struct, a recompiled or lifted function can be
swapped for the interpreter at any call boundary, and a differential test is a struct comparison.

## Services are callbacks, not traps

Each BIOS/DOS service is a C function registered with `callback_register`. Its stub is
`FE 38 id CF` at `F000:1000 + 4*id` (FE /7 is undefined on real CPUs), and the IVT points at it.
Guest code that hooks a vector and chains to the old one reaches the stub like real code would
reach the BIOS. A service that has to wait (keyboard read) returns `CB_RETRY`: the CPU rewinds to
the stub and ends the slice, so no host thread ever blocks. That is required for the browser build.

## Time

`machine_run_for(m, us)` runs the CPU in `TICK_SLICE_US` slices of `ips * slice` instructions,
advancing `virtual_us` and calling every registered ticker (PIT, UART pacing, VGA retrace) after
each slice. `HLT` and `CB_RETRY` end a slice early. The frontend's `FrameClock`
(`runtime/frontend/frame_clock.c`) calls `machine_run_for` once per frame with the wall-clock time owed,
capped at 50 ms, from `frame_step`, which Emscripten's main loop drives ([web.md](web.md)).

## Memory map

| Range | Owner |
|---|---|
| `0000:0000`-`03FF` | IVT |
| `0040:0000`-`00FF` | BIOS data area |
| `0050`-`00FF` segments | DOS kernel data (SYSVARS, SFT, InDOS, default DTA) |
| `0100`-`9FFF` segments | DOS arena (real MCB chain) |
| `A000`-`BFFF` | VGA window, routed through `vga_mem_read`/`vga_mem_write` |
| `C000`-`EFFF` | read-only, open bus |
| `F000:0000`-`0FFF`, `F000:E000`+ | BIOS tables (font, config) |
| `F000:1000`-`1FFF` | callback stubs |

## FPU

The interpreter has no x87: escape opcodes decode their operand and do nothing, so `FNINIT` /
`FNSTSW` probes report no coprocessor and Borland programs fall back to their linked emulator.
Red Baron is the first title that may need real x87 support (milestone M8).

## Port I/O costs bus time

Every `IN`/`OUT` adds `io_cost_ns` (default 1000 ns, an ISA bus cycle) of emulated time through
`Cpu.stall`. Drivers poll with fixed loop counts: `MODEM.DRV` waits at most 8696 reads of the
UART line status before writing anyway, which on real hardware outlasts one 2400-baud byte only
because each read is slow. Without the charge it overran the transmit holding register.

`machine_now_ns` refines `virtual_us` inside a slice by the instructions (and stalls) already run.
Devices polled in tight loops read it: the AdLib probe starts OPL timer 1 (80 us) and polls the
status about 100 times, all inside one 100 us slice.

## Sound

`audio.c` mixes every source into a ring of 16-bit stereo frames at one output rate (48 kHz unless
the sink asks for another). Output frame n belongs to emulated time `base + n / rate`, so sound
follows the emulated clock. Sources render lazily: `audio_sync` runs before any register write that
changes a sound (OPL data, PIT channel 2, port 61h, DSP ports), before a sink reads, and from a ticker once
5 ms are pending. With no sink the mixer only advances its clock and costs nothing.

| Source | Ports | Notes |
|---|---|---|
| OPL2 (`opl.c`) | 388h/389h, mirrors 228h/229h and 220h-223h | Nuked-OPL3 core (docs/third_party.md) at 49716 Hz, linear resampling; status reads `06h` in the low bits (OPL2, not OPL3); timers evaluated lazily from `machine_now_ns`; no IRQ, as on an AdLib |
| PC speaker (`speaker.c`) | 42h, 43h, 61h | square wave at PIT channel 2's rate; held high while the gate is off, so bit-1 toggling is heard; DC-blocked |
| Sound Blaster DSP (`sb.c`) | 226h, 22Ah, 22Ch, 22Eh; DMA 1, IRQ 7 | DSP 2.01: 8-bit mono single-cycle (14h, 91h) and auto-init (1Ch, 90h, DAh) DMA, direct DAC (10h), time constant, block size, pause/continue, speaker on/off, E0h/E1h/E4h/E8h/F2h; output ramps linearly between samples and is AC-coupled like the line out, so a held DAC level fades. ADPCM, recording and the SB Pro mixer are not emulated (their commands are parsed and ignored) |

The DSP keeps its own clock: byte k of a block is fetched from the DMA controller at block start +
(k + 1) sample periods of emulated time, whether or not a sink is rendering. Rendering calls the same
fetch for each frame's time (`audio_frame_ns`), so samples and block ends agree; with no sink a ticker
fetches the rest of a block when it is due, so the block-end IRQ and the DMA counters keep their real
timing. Port access to the DSP or to channel 1's DMA registers first brings the DSP up to now. A masked
DMA channel stalls the DSP, as the DREQ would. The DOS environment carries `BLASTER=A220 I7 D1 T3`.
`SNDBLAST.DRV` ignores it (fixed base 220h from the interpreter, DMA 1, IRQ found by probing 2/3/5/7
with a one-byte transfer); `GOLF.EXE` names the variable (probably reads it).

Sinks, in `runtime/frontend/audio_sdl.c`:

| Sink | How it reads |
|---|---|
| SDL2 device | queue mode from the main loop, no audio thread (works in the browser). `audio_read_paced` keeps about `AUDIO_TARGET_MS` (50 ms; 100 ms in the browser, whose output and frames share the main thread) queued, plus SDL's 1024-frame device buffer: drift is absorbed by stretching at most 0.5%, an underrun gets silence, a lasting excess (smoothed error over 50%) is skipped, and more than 4x the target is dropped. `--perf` counts underruns and discarded audio |
| `--wav out.wav` | every rendered frame, unpaced; with `--headless` this is the test path |

## Conventional memory

`--conv-kb N` leaves programs only the first N KB of conventional memory; the rest becomes a DOS
system block, as resident software would take it. Use it to drive LSCITV into swapping (it needs about
500 KB to start). There is no EMS or XMS.

## Input scripts

`inn --headless --script FILE` replays timed input. One event per line, times in emulated ms:

```
10000 shot work/e2e/select.ppm   # screenshot
11000 click 285 59               # move, press and release the left button (frame pixels)
12000 move 100 50
13000 key 1C                     # set-1 make code, pressed and released
14000 type guybrush\n            # US layout, \n is Enter
```

`tools/ppm2png.py` converts screenshots. Example end-to-end run against the sibling server:

```
../TSN-INN-Decomp/target/debug/innkeeperd --bind 127.0.0.1:2314 --no-int14h &
build/inn --headless --run-ms 95000 --script play.txt --com1 127.0.0.1:2314 work/inn-c 'INN\TSNEXEC.EXE' DEFAULT
```

The C: drive is a copy of the installed client with the program under `\INN`, as `LSCI.CFG`
expects. The frontend starts each program in its own directory.

## Devices

- **Mouse (INT 33h, HLE).** User event handlers (AX=0Ch/14h) run from IRQ12: host events queue,
  IRQ12 enters a ROM trampoline that saves registers, far-calls the handler with the event in
  AX/BX/CX/DX/SI/DI, restores, EOIs and IRETs. One queued event per call.
- **VGA.** B8000h is the text buffer; the A0000h window is routed through `vga_mem_read/write`.
- **8237 DMA (`dma.c`).** Channels 0-3: address/count with the flip-flop, mode, single and all-channel
  masks, status (TC bits, cleared on read), master clear, page registers 87h/83h/81h/82h. Devices pull
  memory-to-device bytes with `dma_read`; terminal count reloads in auto-init mode and masks the channel
  otherwise. Reads never touch the VGA window (they would latch planes). Before the guest reads or
  reprograms a channel, the owning device's sync hook runs.
- **Shadow verification and sound devices.** SB and DMA ports follow the `io_mode` rules like every
  non-VGA device: the original run drives the devices and records, the lifted run replays. DMA only
  reads guest memory, so it cannot disturb the snapshots, and emulated time running backwards when the
  CPU is restored is ignored (fetches only move forward). IRQs raised meanwhile wait in the PIC until
  `irq_hold` clears.
