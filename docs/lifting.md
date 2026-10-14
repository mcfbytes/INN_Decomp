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
boundary (`runtime/src/dos_modules.c`); their `seg:off` are relative to the image start. Freeing
a DOS memory block disarms every hook inside it. On first execution the bytes at the entry are compared with the signature; a
mismatch (another build, or code not yet unpacked) logs once and runs the original instead.

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

Functions whose original would repeat device I/O during a shadow run (dialling, UART handshakes
through a modem driver) or end the process are marked `LIFT_FAR_NOSHADOW` / `LIFT_NEAR_NOSHADOW`:
they are hooked but only ever checked functionally, and the summary lists them as not shadowed.
During the lifted run, VGA port accesses also reach the device (its state is snapshotted), so mode
sets and palette writes are compared. Interrupts are held and emulated time stands still during
shadow runs, so the VGA status register toggles vertical retrace by read count instead; code that
waits for a timer tick still spins there; a shadow
run still going after 200 million instructions is logged with the hook's name and CS:IP.
`INN_LIFT_ONLY=name,name,...` hooks only the named functions, which checks functions that are
otherwise only reached from other lifted code.

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

