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
each slice. `HLT` and `CB_RETRY` end a slice early.

A repeated string instruction (`REP MOVS`, `STOS`, `SCAS`, ...) costs one instruction slot per
element, as it costs a real CPU clocks per element, and is interruptible like on hardware: when the
slice's slots run out it stops with CX, SI and DI current and EIP back on its first prefix, and resumes
in the next slice or after an interrupt. Before 2026-10-16 a whole `REP` cost one slot, so a frame loop
built on block copies (the Yserbius tavern copies its 64000-byte frame to the VGA window about 450 times
per emulated second that way) ran far faster than any 386 and starved the host. 16-bit `MOVS`, `STOS`,
`SCAS` and `CMPS` over conventional memory (and `MOVS`/`STOS` into the VGA window) take a fast path that
does the same element-by-element work without per-byte address decoding (`runtime/tests/test_string.c`). The frontend's `FrameClock`
(`runtime/frontend/frame_clock.c`) calls `machine_run_for` once per frame with the wall-clock time owed,
capped at 50 ms, from `frame_step`, which Emscripten's main loop drives ([web.md](web.md)).

## Idle polls

Waiting screens spin: the INN map asks INT 16h for the shift state about 6300 times per second, the Yserbius
gallery, town and dialogs alternate INT 21h AH=0Bh and INT 33h AX=3 about 17000 times per second. Each
of those services reports a poll that found what the previous one found (`machine_idle_poll`: no key, the
same shift state, no console input, the same mouse position and buttons). After `IDLE_STREAK` (8) such polls
in a row, each within `IDLE_GAP_SLOTS` (5000) instruction slots of the previous one and with nothing else in
between (no port I/O except reads that only tell the time: OPL status, VGA input status, the empty game port;
no VGA memory write; no other service, including interrupt handlers' callbacks), the slice ends after the
service returns and further slices pass without instructions until an interrupt is pending or
`IDLE_MAX_US` (1 ms) has passed. Devices tick as usual meanwhile, so the timer, the UART and the keyboard
raise their interrupts on time and the guest takes them as soon as the skip ends.

To the guest this is a slower CPU while it waits: the polls it would have repeated find the same answer later.
A key arrives at once (IRQ 1 ends the skip), a mouse move within 1 ms; loops that do any work between polls
(the tavern redraws its frame between them) are not affected. Shadow verification, lifted C calling a
service and native mode never skip. `--no-idle-skip` (page: `?idle=0`) turns it off;
`runtime/tests/test_idle.c` covers the key, mouse and timer wake-ups and the busy loops that must not skip.

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

`fpu.c` is a 387: every D8-DF escape and FWAIT, with the state (`Fpu`: eight registers, control,
status and tag words, last instruction and operand pointers) inside `Cpu`, so snapshots and shadow
verification carry it. `Machine.fpu_present` (default on; `--no-fpu` turns it off) decides whether
escapes execute or only decode their operand as before, and sets bit 1 of the BIOS equipment word.
CR0 already reports ET (387 present).

| Topic | Choice |
|---|---|
| Precision | Registers are host `long double`s rounded to a 64-bit significand after each operation: exact x87 extended precision on x86 hosts (where `long double` is the 80-bit format), and binary128 rounded to 64 bits under Emscripten. 80-bit memory operands are packed and unpacked bit by bit, so `FLD`/`FSTP m80` round-trip on both |
| Rounding | Precision control (24, 53, 64 bits) applies to `+ - * /` and `FSQRT` as on a 387. Rounding control applies to integer and BCD stores, `FRNDINT`, real stores and precision-reduced results; extended-precision arithmetic itself always rounds to nearest |
| Transcendentals | `FSIN`, `FCOS`, `FSINCOS`, `FPTAN`, `FPATAN`, `F2XM1`, `FYL2X`, `FYL2XP1` use libm (may differ from a 387 in the last bit); operands of 2^63 or more set C2 and are left alone, as on the 387 |
| Exceptions | `FNINIT` gives CW 037Fh (all masked), the DOS default. IE (invalid, stack overflow/underflow with SF and C1), ZE, OE and UE are detected; PE and DE are not reported. Masked responses store the indefinite, infinity or integer indefinite. An unmasked exception sets ES and B, skips the store, and the next waiting instruction (or `FWAIT`) requests IRQ 13; the BIOS default handler only EOIs |
| Not modelled | NaN payloads and signalling NaNs (every NaN becomes the quiet indefinite with its sign), unsupported 80-bit encodings (read as NaN), the 8087's FENI/FDISI (no-ops as on a 387), P6 instructions (FCOMI, FCMOV) |

Detection: `FNINIT; FNSTSW` stores 0 and `FNSTCW` stores 037Fh, so the Intel/Borland probes find a
coprocessor; with `--no-fpu` nothing is stored and they find none.

Unit tests: `runtime/tests/test_fpu.c` runs instruction bytes through the interpreter (probes, loads and
stores in every format, rounding and precision control, compares, `FXAM`, transcendentals, stack
faults, masked and unmasked exceptions, `FSTENV`/`FSAVE`/`FRSTOR` layouts); it also passes when built
with Emscripten and run under Node.

Lifted C uses the same unit through `fpu.h`: `fpu_mem(m, opcode, reg, seg, off)` and `fpu_reg(m, opcode, modrm)`
run one escape instruction exactly as the CPU would (FWAIT first), and named wrappers spell out the forms
(`fpu_fld_m32/m64/m80`, `fpu_fild_m16/m32/m64`, `fpu_fstp_m64`, `fpu_fistp_m16`, `fpu_fadd_m64`,
`fpu_fsubr_i16`, `fpu_fcompp`, `fpu_fsqrt`, `fpu_fsin`, `fpu_frndint`, `fpu_fstsw_ax`, ...). `fpu_st`, `fpu_sw`
and `fpu_top` read the state, `fpu_push`/`fpu_pop` move host values, and `fpu_state_matches` is the comparison
shadow verification makes (docs/lifting.md, "Floating point"). `runtime/tests/test_lift_fpu.c` runs one long
sequence under eight control words both ways and compares the results bit for bit; it also passes under
Emscripten and Node.

Users: Red Baron has no floating point at all (its math is fixed point, `s\util\amath.c`). GOLF links
Borland's emulator: its code holds `INT 34h`-`3Dh` sequences (2581 in the image) that the emulator's
handlers either interpret in software (no FPU) or patch in place into `FWAIT` + x87 opcodes (FPU present).
Both run under dosrt: with the FPU on, GOLF reaches the first tee executing real x87 instructions
(D8-DF with memory operands of every size, `FCOMPP`, `FSTSW`, the arithmetic-and-pop forms). Verification
compares `Cpu.fpu` and accepts code the original patched where lifted C did not (docs/lifting.md, "Floating
point"). Lifted x87 code assumes the coprocessor: with `--no-fpu` the emulator keeps its own software state,
which the lifted C does not model.

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
| PC speaker (`speaker.c`) | 42h, 43h, 61h | square wave at PIT channel 2's rate; held high while the gate is off, so bit-1 toggling is heard; DC-blocked. Native code sets channel 2 and the gates through `host_timer_speaker` (`pit_set_speaker`) |
| Sound Blaster DSP (`sb.c`) | 226h, 22Ah, 22Ch, 22Eh; DMA 1, IRQ 7 | DSP 2.01: 8-bit mono single-cycle (14h, 91h) and auto-init (1Ch, 90h, DAh) DMA, direct DAC (10h), time constant, block size, pause/continue, speaker on/off, E0h/E1h/E4h/E8h/F2h (the IRQ test raises IRQ 7 30 us after the command, as drivers clear their flag after sending it and then wait: GOLF.EXE's detection); output ramps linearly between samples and is AC-coupled like the line out, so a held DAC level fades. ADPCM, recording and the SB Pro mixer are not emulated (their commands are parsed and ignored) |

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
| SDL2 device | queue mode from the main loop, no audio thread touching the machine. `audio_read_paced` keeps about `AUDIO_TARGET_MS` (50 ms) queued, plus SDL's 1024-frame device buffer: drift is absorbed by stretching at most 0.5%, an underrun gets silence, a lasting excess (smoothed error over 50%) is skipped, and more than 4x the target is dropped. `--perf` counts underruns and discarded audio |
| AudioWorklet (browser) | the same pacing with a 100 ms target (frames are posted from the busy main thread); the frames go to `runtime/web/audio-worklet.js` by `postMessage` and play on the audio thread ([web.md](web.md), "Sound") |
| `--wav out.wav` | every rendered frame, unpaced; with `--headless` this is the test path |

### MPU-401

`mpu401.c` is a Roland MPU-401 at 330h (data) and 331h (status, commands), as far as a UART-mode driver
(`MT32.DRV`, chosen with `music = mt32.drv` in `LSCI.CFG`) uses it. Status bit 6 (DRR) is always clear:
the card always takes a byte; bit 7 (DSR) is clear while an answer waits at 330h. Every command is
answered with FEh, reset (FFh) included; 3Fh enters UART mode, where every command but reset is ignored
(so `MT32.DRV`'s second and third 3Fh time out after 65535 polls each, as on a real card). In UART mode
data bytes go to MIDI OUT; intelligent mode is not emulated (data is dropped, commands only
acknowledged). An empty queue reads FEh, as DOSBox's MPU does. No IRQ: the driver polls.

Nothing is synthesised (MT-32 ROMs are not distributable). `--midi-out out.mid` writes MIDI OUT as a
type 0 Standard MIDI File, one tick per millisecond of emulated time: running status is expanded,
system exclusive messages are written whole, real-time bytes, system common messages and stray data as
F7 escapes, each event stamped at the byte that completes it. The file is finished when the machine is
shut down. Native builds reach the same sink through `host_audio_midi_write` (docs/native.md).

The lifted `MT32.DRV` charges the original's instructions with exact time (`lift_time.h`, see
[lifting.md](lifting.md), "Waiting in lifted code") and takes the timer interrupts that fall inside its
4 s init where the original does, so `INN_LIFT_ONLY=none` and `INN_LIFT_ONLY=mt32_entry` write the same
file, timestamps included, wherever the original repeats itself. Timestamps of the same driver calls
differ where emulated time does:

- Shadow verification stops emulated time during a verified call's original run, so a verified init
  writes its whole patch upload at one millisecond, and later music moves by that much.
- Native mode skips the driver's handshakes and delays (a host sink needs neither), so init takes no
  time there.
- Runs that talk to a live server are not repeatable past the point where its timing matters, as
  innkeeperd's answers arrive by the host's clock: on `ev_input` (2026-10-26) six runs of the original
  agree for the first 871 events and then start the land's music between 86 510 and 86 524 ms, each
  giving a different file, and `inn_hub` varies when ten runs share the machine.

## Conventional memory

`--conv-kb N` leaves programs only the first N KB of conventional memory; the rest becomes a DOS
system block, as resident software would take it. Use it to drive LSCITV into swapping (it needs about
500 KB to start). EMS and XMS are off unless asked for, so every run without the options below is
unchanged.

`--ems-kb N` turns on LIM EMS 4.0 with N KB (16 KB pages; native builds ignore it, their client answers
its probes with "none"): INT 67h functions 40h-48h, 4Bh-4Dh, 57h (move and exchange) and 58h; the
`EMMXXXX0` device opens, IOCTL 4400h reports a character device and 4407h ready. The page frame is
E000h, four 16 KB slots of plain RAM: mapping a page copies the slot's old page back to its store and
the new page in, so a logical page mapped into two slots at once is copied, not aliased. `--xms-kb N`
turns on an XMS 3.0 driver with N KB: INT 2Fh AX=4300h answers 80h, AX=4310h gives the entry in
ES:BX (a far-called service stub), which implements 00h, 08h, 09h, 0Ah, 0Bh (move, handle 0 being a
real-mode seg:off), 0Ch/0Dh (lock returns the linear address) and 0Eh; 01h answers that there is no HMA
(synthetic calls keep their buffers there) and other functions BL=80h.

EMS pages, XMS blocks and both handle tables live in `Machine.mem` above 1 MB + 64 KB, where real-mode
code cannot reach, and `Machine.snapshot_bytes` grows to cover them: shadow verification and synthetic
calls snapshot `[0, snapshot_bytes)`, so a verified call that maps, allocates or moves is undone like any
memory write. Moves read and write the frame through the current mapping. `runtime/tests/test_emsxms.c`
covers both, alone and together.

## Program load and exit

`dos_exec_load` loads a child as EXEC function 0 does and leaves the CPU registers unchanged. The
environment is copied from `env_seg`, or from the parent when that is 0. The command tail is a length
byte, text and CR; the two FCBs are parsed from the text. The child becomes the current process, and
the EXEC frame recorded for it makes terminate restore the parent. The child's terminate address, in
its PSP and in vector 22h, is the value vector 22h already holds, so a caller can install its own
handler first. `dos_exec_enter` then sets the registers, segments, SS:SP and CS:IP as DOS does when it
starts the program.

`dos_last_exit_status` reads the word AH=4Dh returns (AL the exit code, AH the termination type) and
leaves it stored. `dos_set_terminate_hook` runs once terminate has freed the child and made the parent
current, and also when the top program sets `exit_requested`.

With `INN_EXEC_LOG=1`, each load logs `exec: <path> tail=<text> cwd=<directory>` and each terminate
logs `exit: <psp> code=<al> type=<ah>`. With `INN_DOS_AUDIT=1`, INT 21h calls are counted per AH (and
per AL for 44h, 4Bh, 57h, 58h and 5Ah-5Dh) and logged at process exit.

Without `--ems-kb`, INT 67h returns AH=84h for every function and opening `EMMXXXX0` fails with file not
found; without `--xms-kb`, INT 2Fh AX=4300h returns AL=0 ("Conventional memory").

## Input scripts

`inn --headless --script FILE` replays timed input. One event per line, times in emulated ms:

```
10000 shot work/e2e/select.ppm   # screenshot
11000 click 285 59               # move, press and release the left button (frame pixels)
12000 move 100 50
12500 mdown                       # hold the left button at the pointer (a drag: move, then mup)
12900 mup
13000 key 1C                     # set-1 make code, pressed and released
13500 down 48                    # held until the matching "up" (flight controls)
13900 up 48
14000 type guybrush\n            # US layout, \n is Enter
15000 signal ready               # creates $INN_SYNC_DIR/ready
16000 await other 60000          # holds later events until $INN_SYNC_DIR/other exists
17000 end                        # ends the run, closing the line, as --run-ms does
```

The headless loop runs the machine in 10 ms chunks and applies the events due at each chunk's end.
The chunk ends keep a fixed grid from the start of the run, so a lifted call that outlasts a chunk (one
machine step can run `MT32.DRV`'s 4 s init) does not move the later ones, and an event lands at the
same emulated time with or without lifted code. A nested wait (a guest call from lifted C) applies due
events when it passes a chunk end of the grid.
`signal` and `await` synchronize several clients (next section). `await NAME [timeout_ms]` is the same
synchronization as `wait`: it holds the emulated clock until the file exists (default timeout 120000 ms),
so later events keep their spacing. A timeout or a missing `INN_SYNC_DIR` fails the run. Names have no
`/`. `end` stops the run at that time, so a client can hang up.

`tools/ppm2png.py` converts screenshots. Example end-to-end run against the sibling server:

```
../TSN-INN-Decomp/target/debug/innkeeperd --bind 127.0.0.1:2314 --no-int14h &
build/inn --headless --run-ms 95000 --script play.txt --com1 127.0.0.1:2314 work/inn-c 'INN\TSNEXEC.EXE' DEFAULT
```

The C: drive is a copy of the installed client with the program under `\INN`, as `LSCI.CFG`
expects. The frontend starts each program in its own directory.

Two clients on one server keep in step through files in a sync directory (`--sync-dir DIR`):

```
105000 signal a_room           # creates DIR/a_room
105000 wait b_room             # holds here until DIR/b_room exists (at most 120 s of wall time)
117000 wait a_said 30000       # the same with a 30 s limit
```

A `wait` holds the emulated clock at its moment, so the guest sees no time pass; meanwhile the link's
socket is read into the link's own queue (`serial_link_drain`), and the guest receives those bytes once it
runs on. A wait that runs out of wall time, or a `signal`/`wait` without `--sync-dir`, logs `script: ...`
and ends the run with exit status 1 (the summary still prints). Lines at the same time apply in file order,
so a signal written before a wait is made first. Names are plain file names (no `/`, no leading `.`).
Scripts without these commands behave as before. `tools/verify_two.sh <build> <port> <run_ms> <calls>
<scriptA> <scriptB>` runs such a pair, both shadow-verified, against one `innkeeperd`: A on `INN_DRIVE`
(default `work/inn-c`), B on `INN_DRIVE_B` (default `work/inn-c-p2`, account 100002 with its own persona),
with outputs in `work/verify/<port>/a` and `/b` and the sync directory `work/verify/<port>/sync`. Unit test:
`runtime/tests/test_input_script.c`.

## Link settling

A headless run's emulated clock does not read the wall clock (`INN_EPOCH` fixes the start), but the
server's bytes arrive at a wall time, and the guest sees them at whatever emulated time its next receive
poll falls on. Headless runs go about six times faster than real time, and that rate follows the host's
load, so a reply could land a few emulated milliseconds earlier or later from one run to the next.
Scenarios that walk into a random encounter (`tw_combat`) took different paths that way.

`INN_LINK_SETTLE_MS=N` (`runtime/src/link_settle.c`) wraps the TCP link in a settling layer, in both the
UART of the hybrid build (`uart_attach_tcp`) and the native host link (`host_link_open`):

- Once the guest has sent a byte, what the server sends next is held. It is released at the first
  receive poll at or after `INN_LINK_SETTLE_WINDOW_MS` (default 3) of emulated time after the guest's
  last sent byte. Each send moves that point, and 250 ms after the first send it is due regardless.
- At that poll the client waits, in wall time, until the server has been quiet for N ms (or for twice
  the slowest reply seen so far, up to 4 N), then hands over everything received. No more than a 5 s
  wait per poll.
- The first carrier check waits for the TCP connect to finish (up to 5 s).
- An input script's `wait` (`serial_link_drain`) reads the server into the settling queue: a held
  reply stays held until its poll, other bytes are the guest's at once.
- Bytes the server sends while nothing is pending go through at once. Their emulated time still follows
  the wall clock; the first 20 are logged as `link-settle: N bytes arrived unprompted at T ms`.

Every receive poll falls at an emulated time fixed by the run itself, so replies reach the guest at the
same emulated times in every run, whatever the host's load. `tools/verify_lifted.sh` and
`tools/m9_suite.sh` set `INN_LINK_SETTLE_MS=40` unless it is already set; `INN_LINK_SETTLE_MS=0` turns
it off. The run's summary, `link-settle: S settles, W ms waited, slowest reply R ms, ...`, is logged
when the link closes or the process exits.

The 3 ms window is about the median reply time of an unsettled run (1 to 4 emulated ms after the last
byte, measured on `tw_combat`). It keeps the recorded scenarios on the paths they were recorded on: with
it the hybrid `tw_combat` gives the unsettled baseline's traffic, and the M9 suite's verification counts
match an unsettled run of the same build except on `inn_casino`. Other windows (2, 5, 10 or 20 ms) send
`tw_combat` down other paths. The window is shorter than a 2400-baud byte, so the UART's
byte-at-a-time sends settle about once per byte: `tw_combat` waits about 49 s of wall time in 1212
settles. The slowest reply measured under a load average of 40 was 23 ms. The window is a timing model,
not a measurement of the 1993 network.

Settling makes runs of one build repeat. Between the hybrid and the native build it removes the wall
clock, but the two still differ wherever they reach a moment at different emulated times, because
they charge work differently ([native.md](native.md) section 5). That matters for the Yserbius engine:
its random-number table index is seeded from the hundredths of a DOS get-time (INT 21h 2Ch) made
shortly after the player enters the game, so a few milliseconds' difference picks another encounter
(`tw_combat`, `ys_combat`). Most of the difference came from the line: the native host link sent each
byte when it was offered and at the exact baud rate, the UART one byte time later and at 4200 us a
byte (its line is checked once per 100 us slice). `host_link` now models the same shift register and
byte time ([native.md](native.md) section 9), which brings the two builds to within about 3 ms of
each other at that seed (it was 22 ms).
## Several clients against one server

`tools/multi_client.sh <build_dir> <port> <manifest> [run_ms_default]` starts one innkeeperd
(`$INNKEEPERD`, with `--bind 127.0.0.1:<port> --no-int14h --data-dir OUT/data --capture-dir OUT/captures`)
and one `inn --headless` per client, then checks the logs. `OUT` is `work/verify/<port>` (locked
like `verify_lifted.sh`); each client gets `OUT/<tag>/client.log`, its screenshots as ppm and png, and
the server writes `OUT/server.log`. Each client runs on an rsync copy of its drive at
`work/verify-drive/<port>-<tag>` (default `work/srv-games-c`) whose `INN\LSCI.CFG` `id =` line is the
client's account. Scripts see `@OUT@` as `OUT/<tag>` and `@SYNC@` as `OUT/sync`, and the clients get
`INN_SYNC_DIR=OUT/sync` for `signal` and `await`. The environment passes through;
`INN_VERIFY_LIFTED` (50 calls when unset) applies only to the tags in `INN_VERIFY_TAGS` (comma or space
separated, or `all`), so the lift families can shadow-verify multiplayer paths.

Manifest lines (`#` starts a comment; relative paths are relative to the manifest's directory):

```
server <extra innkeeperd args>
client <tag> <account> <script> [start_delay_ms] [drive] [run_ms]   # '-' for the default drive
command <tag> <shell command>      # started after the server, e.g. a Playwright driver;
                                   # @OUT@, @SYNC@ and @PORT@ are substituted
expect <tag|server> <ERE>          # matched in client.log or server.log after the run
reject <tag|server> <ERE>          # (ANSI colour stripped)
```

The summary lists each process's exit status, the count of `not decoded` lines in `server.log` and every
expect and reject result; the exit status is non-zero when any check fails. Example, two players in the
SierraLand Waiting Room, the second hanging up first:

```
client ann 100001 ann.txt 0 - 190000
client bob 100002 bob.txt 3000 - 190000
expect server step=host GroupLeft
reject server not decoded
```

Ann's script ends with `signal annin`, `await bobin` and a screenshot; Bob's mirrors it and finishes
with `await annshot` and `end`. Run: `INNKEEPERD=... tools/multi_client.sh build 41020
private/lifted/scenarios/srv/waiting2/manifest.txt` (the scenario is in the private repo).

## TSN host stand-in

`exec_stub.c` replaces TSNEXEC for running one DOS game client alone: INT 14h returns a 17-entry export
table of callbacks (each stub `FE 38 id CB` ends in `RETF`, since the exports are far cdecl calls).
Messages travel whole, without the link layer: Send dereferences the client's handle through its own
callbacks (nested guest calls, as TSNEXEC makes them) and hands the bytes to a hook; Receive allocates and
fills a handle for the oldest queued host message. GetStatus reports connected (`8003h`), GetLineRate
2400, the tick-counter pointer is `0000:04F0`, the shared block (256 bytes) and the previous-program name
live in the ROM area.

The stand-in's own state (the queued host messages, the handles waiting for the next Poll to free them) is part of
the shadow-verification snapshot (`dos_snapshot.c`): a lifted function that calls Send, Poll or Receive runs against
the same queue as the original did, a Send is answered once, and the queue is compared like memory.

The frontend's `--tsn-host rules.txt` (`runtime/frontend/host_rules.c`) answers from a rule file:

```
shared 80 02 00 01 03 05            # shared block bytes at a hex offset ("text" also allowed)
previous SierraLand                 # GetPreviousProgram
reply 11 00 .. .. .. .. 09 00 => 0d 00 01 04 00 00 09 00 01 02
reply 24 02 => 24 02 T              # T: host time (year-1900, month, day, hour, minute, second)
reply 1c => 02 00 @1 @2 @1 @2 @9-   # @N: request byte N; @N-: the request from byte N on
```

Every rule whose pattern (hex, `..` any byte) prefixes a client message queues its reply. More forms:

```
reply once 1c => ...          # answers the first matching message only
reply nth 3 1c => ...         # answers only the 3rd matching message
reply 1c => 02 00 @1 @2 @9w+10 @11w-2 @13d+1    # @Nw+K / @Nw-K: the little-endian word at request offset N
                                                  # plus or minus the hex K, as 2 bytes; @Nd+K likewise, 4 bytes
at 20000 => 02 00 ...         # queued once when emulated time passes 20000 ms
```

Sums wrap at 16 (32) bits; bytes past the request read as 0. An `at` rule has no request, so `@` items in it
read 0; it is checked at each stand-in Send, Receive and Poll callback (`exec_stub_set_poll_hook`), so it
fires as soon as the client next talks to the host, in queue order after earlier replies. Time counts from
emulated power-on (`virtual_us`), which is within a second of the program start. `tests/test_host_rules.c`
covers the forms.
 Run the game
itself, not `TSNEXEC.EXE`: `inn --tsn-host rules.txt <drive> 'INN\BARON\RB.EXE' '-C..\LSCI.CFG'`
([redbaron.md](redbaron.md)).

## Devices

- **Mouse (INT 33h, HLE).** User event handlers (AX=0Ch/14h) run from IRQ12: host events queue,
  IRQ12 enters a ROM trampoline that saves registers, far-calls the handler with the event in
  AX/BX/CX/DX/SI/DI, restores, EOIs and IRETs. One queued event per call.
- **MPU-401 (330h/331h).** UART mode, no IRQ; see "MPU-401" under Sound. Shadow verification treats it
  as every non-VGA device: only the original run's bytes reach MIDI OUT.
- **Game port (201h).** Nothing attached: reads return FFh (no one-shot ever fires, no button),
  writes are ignored. Red Baron polls it every frame.
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
