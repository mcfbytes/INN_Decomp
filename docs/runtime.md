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
each slice. `HLT` and `CB_RETRY` end a slice early. The frontend calls `machine_run_for` with the
wall-clock time since the last frame, capped, from a per-frame function so Emscripten's main loop
can drive it.

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
