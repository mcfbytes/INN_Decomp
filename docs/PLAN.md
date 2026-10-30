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
| BARON | direct VGA/PIT/PIC programming, code overlays loaded from its volume archive (no x87, despite first guesses) |

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

The end state is "lifted everywhere", with the interpreter kept only as a debugging oracle, and
then a web-native port (M9): lifted code calls canvas, WebAudio and WebSocket through thin
host APIs, not emulated VGA, OPL and UART registers.

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
- [x] **M5** Lift the INN client (2026-10-14): TSNEXEC (118 hooks, INT 14h handler and driver glue),
      LSCITV (VM, memory and swapping, resources, events and the sound sequencer, graphics: about 587
      of 764 functions), GRAPH256.DLL, NLNULL.DLL and the VGA320, MODEM, NOBRK, IBMKBD, ADL and
      SNDBLAST drivers. The combined build verifies about 43,000 calls on the logon path with one
      documented residue (kDrawPic). Not lifted: EMS/XMS transfers, MT32, dead and non-returning code.
- [ ] **M6** Sound: OPL2 (AdLib), then Sound Blaster digital audio. OPL2, PC speaker, mixer,
      SDL2 output and `--wav` done (2026-10-14), browser audio confirmed in Edge. Sound Blaster 2.0 DSP
      and 8237 DMA done (2026-10-14): `SNDBLAST.DRV` initialises, plays FM and, called directly, plays a
      sound's digital track. LSCITV never asks the driver for digital tracks, so in INN only Golf's
      `SOUND.SFX` remains to hear (needs the M8 game launch); browser check of SB output open.
- [ ] **M7** Yserbius, Twinion and Cawdor (lift the shared engine; overlays need per-overlay module registration).
      Status 2026-10-29: all three run; the 1995 Cawdor client reaches the Fall Map, the Cawdor gallery and its
      ruins gate. The lift campaign on `lift-integration` has 1,647 registered entries across CGENN, DARKSTRT,
      FATES and TWGENN, plus Cawdor's TWGENN and RUINS. Cawdor's RUINS lifts are staged pending a byte-level
      recheck; its gallery lifts are shadow-verified. Remaining FATES package work is listed in the private
      `YSERBIUS-PLAN.md`; the interpreter still runs the un-lifted functions ([yserbius.md](yserbius.md),
      private `CAWDOR-PLAN.md`). Per-overlay registration done ([lifting.md](lifting.md), "Overlays").
- [ ] **M8** Golf, Red Baron, Shoppers Advantage. Status 2026-10-14: the interpreter has a 387
      ([runtime.md](runtime.md), "FPU"; Red Baron turned out to have no floating point, GOLF's Borland
      emulator patches itself into x87 code and runs either way). `--tsn-host` runs a game client without
      INN against a scripted host: Red Baron boots and flies, GOLF reaches the first tee. From INN, Red
      Baron is reached through SierraLand's Waiting Room with 2-4 players; `innkeeperd` has no Red Baron
      service or queue yet ([redbaron.md](redbaron.md)). Red Baron census and lifting plan done (private
      `BARON-PLAN.md`).
- [x] **M9** Web-native port (2026-10-16). The INN client is native: video, AdLib music, the timer,
      input and the link call the host, and lifted functions call each other directly. Games stay on
      the emulator in mixed mode. The interpreter remains for shadow verification and for those games.
      The original data files are still read at run time. Still open: Sound Blaster digital audio on
      the native path, and an Asyncify fallback for browsers without JSPI. Details in [native.md](native.md).

Hosting (ledgered 2026-10-16 in innkeeper's `docs/PLAN.md` Phase 7): the public hybrid web build on Azure
Static Web Apps, `innkeeperd` on Azure Container Apps scaled 0 to 1, and an optional OIDC sign-in that
provisions the drive's account and binds the WebSocket. The lifted and native builds are not published.

Later (noted 2026-10-15): optionally run the older TSN 2.1 media as well. Its binaries differ from INN's,
so lifts do not hook and it runs on the interpreter. It needs an installed TSN folder (the sets are
install media), a page that finds the TSN program, a TSN land catalog in `innkeeperd`, and per-land
version ranges where TSN and INN scripts differ. INN may be largely a retheme of TSN, which a
comparison of the two sets' scripts would show.

## 5. Verification

- Boot traces: interpreter vs DOSBox-X (`../TSN-INN-Decomp/tools/dosbox/`) on the same inputs.
- Shadow verification of every lifted function against the original during scripted runs
  (`tools/verify_lifted.sh`).
- Screenshots of key screens compared against DOSBox-X captures.
