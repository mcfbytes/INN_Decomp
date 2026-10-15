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
at their load segment. Drivers (`*.DRV`, raw images) and LSCI DLLs (`*.DLL`, image after the
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

Functions whose original would repeat device I/O during a shadow run (dialling, UART handshakes
through a modem driver) or end the process are marked `LIFT_FAR_NOSHADOW` / `LIFT_NEAR_NOSHADOW`:
they are hooked but only ever checked functionally, and the summary lists them as not shadowed.
During the lifted run, VGA port accesses also reach the device (its state is snapshotted), so mode
sets and palette writes are compared. Interrupts are held and emulated time stands still during
shadow runs, so the VGA status register toggles vertical retrace by read count instead; code that
waits for a timer tick still spins there; a shadow
run still going after 200 million instructions is logged with the hook's name and CS:IP.
`INN_LIFT_ONLY=name,name,...` hooks only the named functions, which checks functions that are
otherwise only reached from other lifted code. `INN_VERIFY_SKIP=name,...` keeps the named functions
hooked but never shadows them, for a call that only works with interrupts flowing (a driver init
that finds its IRQ by waiting for one).

Enable it with `INN_VERIFY_LIFTED=<calls per function>`; `tools/verify_lifted.sh` runs the logon
scenario against a local server and prints the per-function summary. Snapshots cover guest
memory, the VGA device (registers, DAC, planes) and host-side DOS state (open files, positions,
current directory). Host file deletions, renames, creations and growth are journaled and undone
between the two runs. Known limits: bytes written to host files are written by both runs and not restored; devices other than VGA (PIT, UART, OPL) see only the original run's I/O, which is the
intended behaviour.

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

