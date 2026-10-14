# INN to source, source to Wasm

Goal: the complete ImagiNation Network client (TSNEXEC, the LSCI interpreter and its lands, Shadow
of Yserbius, Fates of Twinion, Red Baron, 3-D Golf, Shoppers Advantage) as C source that builds
natively and to WebAssembly, and runs in a browser against `innkeeperd`.

Sibling project `../TSN-INN-Decomp` is reused, not copied: its extraction tools (`tools/`), formats
and protocol docs, Ghidra projects, and the Rust server (`crates/`). Its ScummVM route is dropped.

## 1. Why not decompile straight to hand-written C

Recon (2026-10-14) found code that a one-shot static decompile cannot cover:

| Binary | Obstacle |
|---|---|
| LSCITV | threaded opcode dispatch through a data table; code in runtime-loaded `GRAPH256.DLL`, `NLNULL.DLL` and `*.DRV` sound and modem drivers; EMS/XMS swapping (INT 67h, 2Fh) |
| TSNEXEC | TSR that hooks INT 14h and EXECs every other program; drives the UART directly |
| YSERBIUS, TWINION | Borland VROOMM overlays (INT 3Fh): code loaded into an overlay buffer at run time |
| GOLF | Borland FPU emulator (INT 34h..3Dh), which patches itself into x87 opcodes; 386 instructions |
| BARON | x87, direct VGA/PIT/PIC programming |

## 2. Architecture: one machine, three ways to run its code

```
 browser / SDL2 frontend          (canvas, WebAudio, keyboard, mouse, WebSocket)
          |
 dosrt: the machine               memory, CPU state, PIC/PIT, VGA, OPL, UART,
          |                       DOS/BIOS/EMS/XMS services in C
   +------+------------+
   | interp | lifted C  |   lifted entries are hooked; everything else is interpreted
   +--------+-----------+
```

1. **Interpreter.** A 386 real-mode interpreter, validated against the SingleStepTests suites.
   Runs anything on day one, including overlays, drivers and self-modifying code. It is the
   reference semantics.
2. **Lifted.** Hand-decompiled readable C that replaces one original function at its entry
   (`docs/lifting.md`). Shadow verification runs the original and the lifted C from one snapshot
   and compares memory, video state, registers and port I/O. Runtime-library functions found by
   signature map to the runtime's libc.

A mechanical recompiler tier was planned and dropped (2026-10-14): the interpreter runs the
client at 2-4x real speed even in the browser, and lifted C replaces functions directly, so a
recompiled middle step would add work without removing any.

The end state is "lifted everywhere", with the interpreter kept only as a debugging oracle.

Serial is the network seam: the browser build tunnels UART bytes over a WebSocket into
`innkeeperd`, which already speaks Hayes, the SprintNet PAD and the TSN link layer.

## 3. Copyright boundary

Committed to this repo: the runtime, tools and docs. Never committed: media, extracted
data, generated C, and lifted sources (they are derived from Sierra's code). Lifted sources live in
`private/`, its own git repository, which is ignored here. A build takes the user's own media and
produces the game.

## 4. Milestones

- [x] **M0** Repo, plan, toolchain (Emscripten 6.0.12 in `/mnt/source/tools/emsdk`).
- [x] **M1** `dosrt` with the interpreter: MZ loader, EXEC, DOS file I/O, VGA mode 13h, keyboard,
      mouse, PIT, UART. Native SDL2 build. TSNEXEC + LSCITV reach the dial screen.
- [x] **M2** UART over TCP to `innkeeperd`; the client logs on and reaches the map (2026-10-14).
- [x] **M3** Emscripten build; the same in a browser over WebSocket (adds WS ingress to `innkeeperd`);
      headless Chromium reaches the town map (2026-10-14, [web.md](web.md)).
- [x] **M4** Lifting infrastructure: entry hooks, nested guest calls, shadow verification, generated
      registration tables, Ghidra export (`tools/ghidra/`). First lifted function verified live.
- [ ] **M5** Lift LSCITV: VM, memory manager, resources, kernels, graphics, sound, TSN kernel.
      Status 2026-10-14: VM (207 functions plus the bytecode interpreter), memory/resources/files (144),
      events/input/time/sound sequencer (86 hooked) and TSNEXEC's C code (110) lifted and verified;
      graphics in progress. Open: TSNEXEC INT 14h handler and driver glue, the drivers and DLLs.
- [ ] **M6** Sound: OPL2 (AdLib), then Sound Blaster digital audio. OPL2, PC speaker, mixer,
      SDL2 output and `--wav` done (2026-10-14), browser audio confirmed in Edge. Sound Blaster 2.0 DSP
      and 8237 DMA done (2026-10-14): `SNDBLAST.DRV` initialises, plays FM and, called directly, plays a
      sound's digital track. LSCITV never asks the driver for digital tracks, so in INN only Golf's
      `SOUND.SFX` remains to hear (needs the M8 game launch); browser check of SB output open.
- [ ] **M7** Yserbius and Twinion (lift the shared engine; overlays need per-overlay module registration).
      Status 2026-10-14: from the INN map, `CGENN` and `DARKSTRT` run under the interpreter to the town and
      the dungeon with music, `TWGENN` and `FATES` to the town, given three host additions ([yserbius.md](yserbius.md)). Per-overlay
      registration done ([lifting.md](lifting.md), "Overlays").
- [ ] **M8** Golf, Red Baron (x87), Shoppers Advantage.

## 5. Verification

- Boot traces: interpreter vs DOSBox-X (`../TSN-INN-Decomp/tools/dosbox/`) on the same inputs.
- Shadow verification of every lifted function against the original during scripted runs
  (`tools/verify_lifted.sh`).
- Screenshots of key screens compared against DOSBox-X captures.
