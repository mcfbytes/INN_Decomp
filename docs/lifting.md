# Lifting: replacing original code with readable C

A *lifted* function is hand-decompiled C that replaces one original function. The runtime
(`runtime/src/lift.h`) runs it in place of the original whenever execution reaches the entry.
Lifting proceeds one function at a time; every step leaves a fully working game.

Lifted sources are derived from Sierra's code and live in `private/` (its own git repository,
never pushed to the public repo; see `docs/PLAN.md` section 3). Only the mechanism is public.

## Contract

- Signature `void fn(Machine *m)`. On entry CS:IP is the function entry and the caller's return
  address is on the stack, exactly as the original sees it.
- The function must leave every guest-visible effect the original leaves: memory, registers the
  caller relies on (by the calling convention of that binary), flags if a caller tests them, port
  I/O in order, and the return (`lift_ret_far` / `lift_ret_near`, with the callee-popped bytes
  for Pascal-convention functions).
- It may call original code that is not lifted yet with `guest_call_far` / `guest_call_near`;
  those run nested CPU slices and return when the callee returns. Lifted callees are called as
  plain C functions.
- Emulated time advances only for instructions actually interpreted, so a lifted function runs
  in zero emulated time. Programs that time themselves with busy loops still work because those
  loops read the PIT or BIOS tick count, not instruction counts.

## Region lifts

Some original code never returns in the C sense: LSCITV's bytecode interpreter runs the script
main loop for the whole session. A `LIFT_REGION` entry may run any amount of original behaviour in
C and hand control back at an instruction of its choosing (for the VM: every kernel call, and any
return address it does not model). Nothing nests, so the frontend keeps its frame loop and the
browser never blocks.

Region verification inverts the order: the lifted run goes first (I/O recorded), which fixes the
exit CS:IP and SP; the original then runs from the same snapshot, with recorded reads replayed,
until it first reaches that CS:IP with that SP. Memory below the exit SP is dead stack. The game
continues from the lifted end state.

## Guest stack use

Lifted code that needs scratch space in guest memory (a buffer passed to original code, a DOS
call's argument block) takes it by moving SP down, as the original does, never at a fixed depth
below SP: small stacks (TSNEXEC has 1 KB inside DGROUP) put static data right below them.

## Registration

Scratch sets must use macros from shared headers (`include/`): the generated table is a separate
translation unit and cannot see file-local definitions. Each lifted source file contributes rows to a static `LiftedEntry` table: module name, entry
`seg:off` relative to the load segment, function, name, and the first 12 original bytes.
`lift_module_loaded` arms a table when a loader announces the module. DOS EXEC announces programs
at their load segment. Drivers (`*.DRV` and Miles `*.ADV`, raw images) and LSCI DLLs (`*.DLL`, image after the
relocation list) are announced when a file read lands their image start on a paragraph
boundary (`runtime/src/dos_modules.c`); their `seg:off` are relative to the image start. Borland
overlays are announced per overlay (section "Overlays"). Freeing
a DOS memory block disarms every hook inside it. On first execution the bytes at the entry are compared with the signature; a
mismatch (another build, or code not yet unpacked) logs once and runs the original instead.

## Overlays

Borland's overlay manager (VROOMM, in `DARKSTRT.EXE` and `FATES.EXE`) keeps overlay code out of the load
image and reads it from the program's own EXE into a buffer when an INT 3Fh thunk in the overlay's stub
segment is called; it later reuses the buffer for other overlays. An overlay is named by its stub
segment, as Borland's map files and the far calls in the resident code name it.

- **Format** (observed, `runtime/src/borland_overlay.c`): right after the MZ load module, a 16-byte
  trailer `"FBOV"`, u32 overlay data bytes, u32 file offset of the segment table, u32 table rows; the
  overlay data follows. Table rows are 8 bytes (u16 segment, u16 max offset, u16 flags, u16 min
  offset); flags 3 marks an overlay stub. A stub starts with INT 3Fh, then u16, u32 code offset in the
  overlay data, u16 code bytes, u16 fixup bytes, u16 entry count; one 5-byte INT 3Fh thunk per entry
  follows at +20h. The fixup table follows the code: word offsets that get the program's load segment
  added. The buffer only keeps the code; the next overlay may be read over the fixups.
- **Markers.** An overlay function is marked with its stub segment and its offset in the overlay's code,
  `LIFTED(0x1E3E, 0x0042, ...)` in a directory whose `module.txt` names the EXE. `tools/gen_lift_table.py`
  sees that the segment is a stub, registers the entry under `DARKSTRT.EXE#1E3E`, takes the signature
  from the overlay's code, its relocations from the overlay's fixup table, and leaves bytes past the end
  of the code uncompared (`LiftedEntry.unknown_mask`).
- **Announcing.** A DOS read from a file whose name is a loaded program with overlays announces each
  overlay whose code the read holds whole and that lands on a paragraph (`dos_modules.c`):
  `lift_overlay_loaded(name#stub, segment, code bytes, program load segment)`. That disarms the same
  overlay's hooks wherever they were armed and any hook inside the new code's range (the overlay it
  replaced), then arms the overlay's entries.
- **Checked on every call.** Overlay entries compare their bytes, with the fixups applied, each time
  they are reached, not only the first time: a buffer may be reused without a read we see. A mismatch
  runs the original and is logged once per arming.
- **Verification.** Counts are kept per entry across loads, and the summary lists every entry that was
  ever armed. An overlay load during a shadow run re-arms hooks for the new code; restoring the
  snapshot does not undo that, which at worst leaves an overlay function running its original until
  the next load of it.

The manager may read several consecutive overlays in one read (`DARKSTRT` reads its first two
together); each is announced.

## Images loaded by the program

Some programs load code that no DOS read lands whole: Red Baron decompresses its video and sound drivers
from its data volumes into its own heap and far-calls them. Such a raw image is announced when it is
first called:

- **Marking.** The lifted directory's `module.txt` names the image (any name, e.g. `VMCODE2.OVL`), the
  path of the image as it sits in memory once loaded, and a line `announce entry`. Entries are marked
  with segment 0 and their offset in the image. `tools/gen_lift_table.py` adds a `LiftedImage` to the
  table: the image's size and its first 32 bytes, with relocated words (none in a raw image) and bytes
  past its end left uncompared.
- **Announcing.** While an image is not announced, every far call (CPU `CALLF`, direct and through a
  pointer, and `guest_call_far` from lifted code) is looked at once it lands: when the target is offset
  0 of a paragraph that is no announced module's load segment and the paragraph holds the image's head,
  `lift_module_loaded` announces the image there (`lift: image NAME found at SEG` in the log). Its entries
  are armed before the called instruction runs, coverage names its addresses (`IMAGE.OVL:0000:0100`),
  and synthetic call triggers can name it. A lifted loader that calls the image's start keeps the
  announcement, through `guest_call_far`; it may also call `lift_module_loaded` itself.
- **Cost.** A far call pays one more not-taken test; the target is looked at only while an image is
  waiting (or a dump point below is set), and once every image is announced that stops. Freeing the
  memory that holds an image disarms it and makes it wait for its next load, which may be elsewhere.
- **Getting the bytes.** The image file is cut from a memory dump taken at the first far call into it:
  `INN_DUMP_AT=PROG.EXE:SSSS:OOOO:1` (below) with the image's address relative to the program, then
  `tools/extract_image.py <dump.bin> <linear or seg:off> <bytes> <out>`. The size comes from the loader
  (or the DOS block that holds the image). Compare the image with a later dump to see which bytes are its
  own data; the head and the lifted entries must not be among them.

## Workflow per function

1. Ghidra (`/mnt/source/tools/ghidra/ghidra_12.1.4_PUBLIC`) with the unpacked binary, runtime
   library functions named by `tools/omf_sigs.py`, and names carried over from the sibling
   project's analyses.
2. Export the function's disassembly and Ghidra C to `private/` as the starting point.
3. Write the lifted C: real types, named fields, no raw offsets in the logic (DGROUP variables
   become named accessors), comments only for why.
4. Verify with shadow execution (below) over recorded play sessions; only then enable it by
   default.

## Shadow verification

Each verified call snapshots the machine, runs the original under the interpreter until it
returns (interrupts held, port I/O recorded and performed), saves that end state, restores the
snapshot, runs the lifted C with the recorded port reads replayed (its writes are compared, not
performed), and compares the two end states: memory except the dead stack below the entry SP,
the registers outside the entry's scratch set, flags unless scratch, and the port I/O sequence.
The game then continues from the original's end state, so a wrong lift cannot derail a run. All
lifted hooks are off during the original's shadow run, so lifted callees inside it run original
code and the whole call tree is compared. The
first difference is reported with the function, the call number and the differing value.
AX and DX are scratch for C functions but also carry their return value, so the scratch set alone
would hide a wrong result. When everything else matches but DX:AX differs, the call is counted and
the first one per entry is logged as `result DX:AX lifted ... original ...`, and the summary line
adds `DX:AX differs in N`. That is not a mismatch (a void function leaves whatever it last computed),
so a function that returns a value declares it with the `*_RET_AX` or `*_RET_DXAX` scratch macros,
which take AX (or DX and AX) out of the scratch set and make a wrong result a mismatch.

Functions that wait on the BIOS tick, a UART or another interrupt (dialling, UART handshakes through
a modem driver) are marked `LIFT_FAR_REPLAY` / `LIFT_NEAR_REPLAY` and verified with device replay
(below). Functions whose original ends the process, or whose effects no snapshot can undo, are marked
`LIFT_FAR_NOSHADOW` / `LIFT_NEAR_NOSHADOW`: they are hooked but only ever checked functionally, and the
summary lists them as not shadowed.
`LIFTED_PREFIX(seg, off, fn, kind, scratch, n)` is `LIFTED` with a signature of only the first `n` bytes, for an
entry whose code is followed by data the loader or the host fills in (the file does not hold those bytes).
During the lifted run, VGA port accesses also reach the device (its state is snapshotted), so mode
sets and palette writes are compared. Interrupts are held and emulated time stands still during
shadow runs (device replay aside), so the VGA status register toggles vertical retrace by read count instead; code that
waits for a timer tick still spins there; a shadow
run still going after 200 million instructions is logged with the hook's name and CS:IP.
`INN_LIFT_ONLY=name,name,...` hooks only the named functions, which checks functions that are
otherwise only reached from other lifted code. `INN_VERIFY_SKIP=name,...` keeps the named functions
hooked but never shadows them, for a call that only works with interrupts flowing (a driver init
that finds its IRQ by waiting for one).

Enable it with `INN_VERIFY_LIFTED=<calls per function>`; `tools/verify_lifted.sh` runs the logon
scenario against a local server and prints the per-function summary. With `INN_TSN_HOST=<rules>` it
runs without a server, the program named by `INN_PROGRAM` against dosrt's TSN host stand-in (Red Baron:
[redbaron.md](redbaron.md), "Verifying lifts"). Multiplayer paths use `tools/verify_two.sh`: two clients,
each with its own drive copy and output directory, against one server, their scripts kept in step with
`signal` and `wait` ([runtime.md](runtime.md), "Input scripts"). Snapshots cover guest
memory, the VGA device (registers, DAC, planes) and host-side DOS state (open files, positions,
current directory). Host file deletions, renames, creations and growth are journaled and undone
between the two runs. Known limits: bytes written to host files are written by both runs and not restored; devices other than VGA (PIT, UART, OPL) see only the original run's I/O, which is the
intended behaviour.

## Semantic port comparison

In a hybrid-hw build (`-DINN_NATIVE_HW=ON`) a converted driver function does not touch ports: its device
backend calls `lift_semantic_event` / `lift_semantic_read` (`runtime/src/lift_semantic.h`). Shadow
verification then compares by meaning (`INN_VERIFY_SEMANTIC=0` turns it off there; `=1` turns it on in a
hybrid build, where it must agree with the raw comparison). Both runs are reduced to the same events:

| Device | Port accesses | Event |
|---|---|---|
| OPL (388h, 228h, 220h, 222h) | index write, then the data write at the next port; status reads dropped | `SEM_OPL_WRITE` |
| VGA DAC | 3C8h write sets the write index, every third 3C9h write; 3C7h write sets the read index, every third 3C9h read | `SEM_DAC_SET`, `SEM_DAC_GET` |
| CRTC | 3D4h index 0Ch and 0Dh with their 3D5h data | one `SEM_CRTC_START` after both |
| PIT | 43h control for channel 0 (lobyte/hibyte), two 40h writes | `SEM_PIT_DIVISOR` |
| PIC | 20h/A0h write of 20h; 21h/A1h write (reads dropped) | `SEM_PIC_EOI`, `SEM_PIC_MASK` |
| keyboard | 60h read; 61h read, write with bit 7 set, write with it clear | `SEM_KBD_DATA`, `SEM_KBD_ACK` |
| DMA channel 1 | 0Ah write for channel 1 | `SEM_DMA_MASK` (a = 1, b = 1 masked) |
| DMA channel 1 | 0Ch flip-flop clear, 0Bh mode for channel 1, both 02h address and both 03h count bytes (the flip-flop says which), 83h page, in any order | one `SEM_DMA_PROGRAM` (a = mode << 24 \| page << 16 \| address, b = count) once all are in |
| SB DSP reset | 226h write of 1, then of 0 | `SEM_SB_RESET` |
| SB DSP data | 22Ah read | `SEM_SB_DATA` (value replayed) |
| SB DSP commands | 22Ch writes, a command byte with its argument bytes: E0h x, E1h; 40h tc; 48h length; 14h length; 1Ch and 90h (auto-init), 91h; D0h, DAh; D4h; D1h, D3h | `SEM_SB_IDENT`, `SEM_SB_TIME_CONSTANT`, `SEM_SB_BLOCK_SIZE`, `SEM_SB_PCM_START` (a = auto-init, b = 14h's length or the last 48h's), `SEM_SB_PCM_STOP` (a = 0 halt, 1 exit auto-init), `SEM_SB_PCM_CONTINUE`, `SEM_SB_SPEAKER` |

3DAh reads are dropped; every other access, and a lone 61h access, stays a raw port event. The
original's side is canonicalised from the recorded port log; the lifted side canonicalises the ports
it still performs and adds the events its host-API calls report. Raw events and semantic events are
each compared in order. A difference is logged as `semantic event K: lifted <...>, original <...>` (or
`port event K: ...`). Reads return the original's value: the n-th read of a kind, or the n-th read of a
port for raw accesses; a read the original did not make is a mismatch. An unconverted function produces
the same events on both sides, so it compares as before. Region-mode and device-replay entries keep
their raw comparison.

22Ch reads (write-ready polls) and 22Eh reads (data-ready polls and the IRQ acknowledge) are dropped, as
OPL status reads are. Other DSP commands, DMA accesses for other channels and a program, reset or command
left incomplete stay raw events. High-speed and normal starts are the same event, so a converted driver
may use either. The device backend of `host_audio.h` (`host_audio_pcm_start`, `host_audio_sb_reset` and
the rest) performs these port sequences in emulated mode, so in a shadow run's lifted half they are
canonicalised like the original's.

## Device replay

A plain shadow run holds interrupts and devices see only the original's I/O, so an original that waits
for a timer tick or a received byte never returns. Entries marked `LIFT_*_REPLAY`, and any named in
`INN_VERIFY_REPLAY=name,...` (also NOSHADOW ones, to try them), are verified differently
(`runtime/src/lift_replay.c`):

- **The original runs live, with time flowing.** Interrupts are delivered and devices tick (PIT, the BIOS
  tick at 0040:006C through INT 08h, UART transmit and receive on the real link), so the original waits,
  dials and logs on exactly as it would unverified. Everything it observes is logged in order as
  *events*: port reads (with the value read) and writes, every host callback that completes (INT 1Ah,
  DOS and BIOS services; a callback that blocks and retries counts once, the lifted-code return sentinel
  never), and HLT. Consecutive identical events (a poll loop) share one log entry with a repeat count.
- **Interrupts are gated to events.** In the original's run an interrupt is taken only at the first
  instruction boundary with IF set after an event (while halted, at any boundary), and logged there. Where
  an interrupt falls between two events is not observable by the code (any latency is valid on a real
  PC), but which events lie before and after it is, and that is what the lifted run can reproduce. An
  interrupt pending for 100000 instructions without an event is taken anyway and counted as unaligned.
- **The lifted run replays.** Devices are frozen (no tickers run, emulated time stands still, nothing
  reaches the link), port reads return the logged values, port writes, callbacks and HLTs must match the
  log in kind, port, size and value, and each logged interrupt is delivered as soon as the lifted run has
  made the event before it: at the next guest instruction boundary with IF set, or, when lifted C made the
  event with IF set, at once as a nested INT from inside the port access (C has no instruction
  boundaries). Interrupts the original took at its return are delivered after the lifted function
  returns. ISRs run as guest code (or their lifts) in both runs, so their port reads are replayed too.
- **Alignment is by events, not instruction counts.** Lifted C reaches the same waits through nested
  guest calls and runs far fewer instructions, so instruction counts or emulated time would not line up;
  the sequence of port accesses and service calls does when the lift is faithful. The first event that
  differs ends the lifted run (a longjmp back to the verifier) and is reported as
  `device replay: event N: lifted <event>, original <event>`, followed by the log's size and interrupt
  counts. Otherwise the end states are compared as for any shadow run: memory, registers, VGA and DOS
  state; the port I/O and serial output are already compared event by event.
- **The link sees every byte once**, from the original's run; the lifted run's UART writes are compared,
  never sent. The game continues from the original's end state, at the emulated time it reached.

Limits: an interrupt the original took during a stretch with IF clear in lifted C (C that emulates
CLI/STI) is delivered at the lifted run's next chance instead, before or after reads of memory the ISR
writes; code that polls memory for an ISR's result without any event in the loop is only taken by the
unaligned fallback. Both show up as event or memory mismatches, not as false matches. The log holds at
most 16M entries. Synthetic calls never use replay.

## Synthetic calls

A lifted function that no scenario reaches can still be shadow-verified: a call list names the entry
and the arguments of each call, and the runtime makes those calls itself on top of a realistic
machine state, verifies each one exactly as a live call (original first, then the lifted C from the
same snapshot, the same comparison, DX:AX notes and `*_RET_AX` rules), and puts the machine back.
`INN_SYNTH=<file>[,<file>...]` loads call lists (`runtime/src/lift_synth.c`); it turns verification
on with 0 live calls per entry when `INN_VERIFY_LIFTED` is not set. `tools/verify_lifted.sh` passes
the variables through.

```
dgroup LSCITV.EXE 2074                       # DGROUP of the module: image segment, load base added
after LSCITV.EXE lsci_kGetEvent 20            # fire the following calls at the 20th entry of it
call LSCITV.EXE lsci_kSqrt words(1, 144) DS=DGROUP
call LSCITV.EXE lsci_strncat buf(64,"abc") "xy" 3 DS=DGROUP
call TSNEXEC.EXE tsn_memcpy let a=buf(32,"abcdefgh") a+2 a 5
call TSNEXEC.EXE tsn_long_shift_left DX:AX=long(0x12345678) CX=4
```

- **Triggers.** `after <module> <entry> [n] [near|far]` takes the snapshot the n-th time (default
  1) live code reaches the entry: a lifted entry by name, or any address as `seg:off` relative to the
  module's load segment (overlays: `PROG.EXE#stub`). `near` says how `arg(n)` finds the trigger's own
  arguments at an address (named entries know). The calls after an `after` line belong to it. Entries
  reached inside shadow runs do not count. A trigger that never fires is reported at exit.
- **Calls.** `call <module> <entry> <items...>`; the entry is a name or `seg:off`. Items are separated
  by spaces; an argument item is pushed as the original's caller pushes cdecl arguments: the first item
  at the lowest address. A word is one item; a pointer pushes offset then segment, a `long()` low word
  then high word. Callee-popped (Pascal) functions take their arguments in the same layout.
  Registers: `AX= BX= CX= DX= SI= DI= BP= DS= ES=` and pairs `DX:AX= ES:DI= ES:SI= ES:BX= DS:SI= DS:DI=
  DS:DX= DS:BX= CX:BX=` (high:low, from a pointer or a long). Registers not named keep their trigger
  values; `DS=DGROUP` uses the module's `dgroup` line, or the trigger's DS when there is none.
- **Values.** Numbers (`12`, `-1`, `0x7FFF`, `'a'`); `long(n)`; `"text"` (a fresh NUL-terminated copy);
  `buf(size, "text", 1, 2, fill=0xCC)` (a fresh buffer, zero-filled unless `fill=`, initial bytes as
  listed, no implied NUL); `words(a, b, ...)`, `bytes(a, b, ...)` (fresh buffers holding them; a
  pointer in `words` takes two words); `far(SEG, off)`; `null`; `off(p)`, `seg(p)`; memory as it is at
  the trigger: `[SEG:off]` (word), `farat[SEG:off]`, `longat[SEG:off]`, where SEG is `DS ES SS CS
  DGROUP` or a number; the trigger's arguments `arg(n)`, `argptr(n)` (word n, and the far pointer at
  words n, n+1). `p+n` and `p-n` move a pointer (no spaces around the operator). `name=value` pushes a
  value and names it, `let name=value` only names it, for later items such as `name+2`.
- **Buffers** live in the HMA (FFFF:0010 to FFFF:FFFF, 16-byte aligned, 16 zero bytes after each), with
  A20 enabled for the call: no DOS program here uses the HMA and the runtime has no XMS, so the area is
  free and inside the snapshot. They are compared like any memory, so what a function writes into its
  buffers is checked, including writes past their end (into the gaps).
- **Restoring.** The trigger state is saved (CPU, the first 1 MB + 64 KB of memory, VGA, the Sound
  Blaster DSP and the DMA controller, DOS handles, hook bitmap, emulated time and pending interrupts, A20).
  Each call is set up on top of it, verified (host file deletions, renames, creations and growth
  journaled and undone after both runs, as for live calls), and the trigger state is loaded again before
  the next call and before the run continues. During the calls the time slice is stretched so no device
  ticker runs, interrupts are held and call coverage is off. `INN_SYNTH_CHECK=1` saves the state again
  after each restore and stops with an error if the CPU, memory, VGA, Sound Blaster or DMA state, DOS
  state, hook bitmap, time, interrupts, A20 or I/O mode differ. Calls that program the DSP and DMA
  channel 1 are therefore restorable (shadow runs save and load that state with the VGA state too).
  Not restorable, so not for synthetic calls: bytes written into host files, port I/O to other devices
  (PIC, PIT, OPL, keyboard, UARTs; an IRQ the DSP raises within the call, F2h's or a block end's, is PIC
  state), loading an overlay or module, ending the process. A call whose original does not return
  within 50 million instructions, or that ends the process, is reported as a mismatch and the lifted C
  is not run; avoid calls the lift turns into `fatal()` (a zero divisor) or loops forever on.
- **Results** are counted apart from live calls: mismatches are logged as
  `verify: MOD name synthetic call N (file:line): ...` and the summary adds
  `verify: MOD name: N calls checked, M mismatches (synthetic)` (with `DX:AX differs in N` as for live
  calls). Each trigger logs where it fired (`synth: file:line after ... fired at CS:IP SS:SP DS: N calls
  checked, M mismatched`). `INN_SYNTH_LOG=1` logs every call's original DX:AX, and the text there when
  it points into a buffer, for reviewing what the cases did.

`tools/synth_args.py MODULE ENTRY TEMPLATE --var NAME=SPEC ...` writes call lines from a template with
`{NAME}` placeholders, as the product of the variables' values (or `--zip`): edge sets (`edges16`,
`edges32`), ranges, seeded random numbers, longs and strings, string lists and files
(`tools/synth_args.py --help`). Call lists that spell out Sierra functions' arguments belong in
`private/lifted/synth/`.

## Coverage

`INN_COVERAGE=<file>` records every distinct guest address that is the target of a CALL, a far CALL,
an interrupt (INT n, IRQ, fault) or a guest call from lifted code, and writes them with their call
counts when the run ends. Each place is named relative to the innermost announced module that holds
it: `DARKSTRT.EXE:08C7:0032` (segment relative to the load segment, as the CALL named it),
`DARKSTRT.EXE#1E34:003C` (an overlay: stub segment and offset in its code). Targets outside every
announced module (BIOS stubs, code loaded by other means) are left out. Off, the CPU pays one
not-taken test per call instruction; the interpreter's instruction loop is untouched.

VROOMM enters an overlay function by a far jump from the stub's INT 3Fh thunk, so a call to an
overlay function is recorded at the thunk (`DARKSTRT.EXE:1E56:002F`); calls inside an overlay are
recorded at the overlay code.

`tools/verify_lifted.sh` passes it through: `INN_COVERAGE=1` writes `coverage.txt` into the run's
output directory (`work/verify/<port>/`). `tools/coverage_report.py` turns one or more coverage files
into a work list against a Ghidra export: each reached function with its size, call count, whether it
is a leaf (no callees), and whether it is lifted or staged in the given lifted trees. Thunk calls are
credited to the overlay function they load (the thunks are read from the EXE `module.txt` names).

```
INN_DRIVE=$PWD/work/yserbius-c INN_COVERAGE=1 tools/verify_lifted.sh build-x 2591 196000 20 \
    private/lifted/scenarios/ys_dungeon.txt
tools/coverage_report.py work/verify/2591/coverage.txt --ghidra private/ghidra/DARKSTRT.EXE \
    --lifted private/lifted --todo --leaf
```

`--todo` keeps functions neither lifted nor staged, `--leaf` only leaves, `--unknown` also lists
targets that are no Ghidra function (functions inside overlays Ghidra did not find, code past the
image such as loaded drivers), `--tsv` prints tab-separated columns. Several coverage files (one per
scenario) merge.

## Waiting in lifted code

Lifted C runs no instructions, so two things keep its waits working:

- Port accesses made from lifted C (outside shadow runs) charge their bus time to emulated time and
  end scheduler slices as they fill, so a C poll loop sees retrace, UART and timer state change.
- A nested guest call that waits (a dial, kWait) calls the frontend's yield hook between slices
  (`lift_set_nested_yield`): it keeps emulated time with the wall clock, presents frames, takes input
  and, in the browser, returns to the event loop through Emscripten's Asyncify, so WebSocket data
  arrives. The browser build therefore links with `-sASYNCIFY`; the functions that reach the sleep
  through an indirect call are listed in CMakeLists.txt. Headless runs have no hook and stay
  deterministic.


## Native-only tables and dumps

A lifted directory whose `module.txt` has a third line `native` holds code written for native program
control (docs/native.md section 8). The generator sets `LiftedEntry.native_only` on its entries and
`arm()` skips them unless `m->native` was set when a module was armed or `INN_NATIVE_CONTROL=1`, so a
plain hybrid build never runs them. `native_exec` (`runtime/src/native/native_exec.c`) loads a child
through the DOS loader and runs it as C when its start address is a lifted entry, otherwise on the
emulator; termination unwinds to it through the DOS terminate hook.

`INN_DUMP_AT=<point>:<n>[,...]` with `INN_DUMP_DIR=<dir>` writes `<point>-<n>.bin` (memory 0 to
10FFEFh) and `.txt` (registers, MCB chain, IVT, DOS handles, current directory, PSP) the n-th time the
point is reached; points are `exec` (AH=4Bh AL=0 or `native_exec`), `start` (a program is entered),
`kernel`, and module addresses `MODULE:SSSS:OOOO` (hex, segment relative to the module's load segment),
reached when a far call lands there (`RB.EXE:1234:0000:1` writes `RB.EXE:1234:0000-1.bin`).
`tools/m9_dump_diff.py` compares two dumps.

## Direct calls

`guest_call_far`, `guest_call_near` and `guest_int` run their target as a C call when it is one of:

- an active lifted entry whose signature bytes match (through shadow verification when it is on);
- a jump thunk (`JMP` near, short or far) whose destination is hooked or again a thunk or bare return,
  followed up to four hops (a driver's entry jumps to its body);
- a bare return (`RET`, `RETF`, `RET n`, `RETF n`, `IRET`), applied in C;
- a host service stub (`FE 38 id CF/CB`), whose callback runs and whose `IRET` or `RETF` is applied in C.

Anything else, and anything that does not return to the pushed return address, runs on the CPU as
before. Nothing is followed while a shadow run has the hooks off or at 0F0000h and above (the BIOS
ROM). `lift_set_unresolved` registers a hook that is offered a target resolved to nothing.

The CPU path costs at least a hooked instruction and the return stub; a direct call costs no
instructions, so each one (and each followed jump or applied return) charges `io_cost_ns` through
`machine_charge_c`. Without it a loop that polls INT 1Ah never sees the tick change, and a script that
times a loop divides by zero ticks.

A lifted entry is called directly only when no interrupt is due (`IF` set and a request pending), so
the ISR runs first, on the CPU, at the callee's first instruction boundary as before. A service stub
is called directly only in live I/O mode, outside device replay and tracing, and when the caller's
pushed `IF` and a pending request do not call for an interrupt. A callback that returns `CB_RETRY`
puts the stub back and the CPU retries it.

Environment: `INN_DIRECT_CALLS` unset or `1` enables everything, `0` nothing, `2` lifted entries,
thunks and returns only, `3` service stubs only. `INN_DIRECT_STATS=1` prints the counters at exit.
`INN_INTERP_PROFILE=<file>` counts the instructions the interpreter runs per 16-byte paragraph and
writes them with the modules' load segments and the line `direct <calls> fallbacks <n> callbacks <n>
thunks <n> returns <n>` (`tools/m9_profile.py` summarises it). A fallback is a direct call that did
not end at its return address.
