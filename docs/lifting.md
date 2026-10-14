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

## Registration

Each lifted source file contributes rows to a static `LiftedEntry` table: module name, entry
`seg:off` relative to the load segment, function, name, and the first 12 original bytes.
`lift_module_loaded` arms a table when a loader announces the module (DOS EXEC does this for
programs; drivers and DLLs loaded by the programs themselves need their loader hooked as well,
still to do). On first execution the bytes at the entry are compared with the signature; a
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

## Shadow verification (planned)

With `--verify-lifted`, each call to a lifted function snapshots the machine, runs the lifted C,
records the resulting state, restores the snapshot, runs the original under the interpreter until
it returns (interrupts held), and compares the two end states: memory, the registers the
convention preserves or returns, and the port I/O log. Port reads made by the original are
replayed to the lifted run so device timing cannot cause false differences. The first difference
is reported with the function, the call count and a byte-level diff.

## Browser constraint

The browser build has one thread and no blocking. If lifted code calls original code that waits
for input (`CB_RETRY` inside a nested call), the nested loop would never return to the browser's
event loop. Such call paths either get lifted as non-blocking state machines or the web build
enables Emscripten's ASYNCIFY for them. Track any such path when it appears.
