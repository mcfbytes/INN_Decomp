# Compilers that built each binary

Evidence from `tools/omf_sigs.py` (runtime-library signatures from Borland C++ 3.1's `LIB/*.LIB`,
fixup bytes masked) and a byte comparison of each program's entry code with the `C0x.OBJ`
startup module. Borland C++ 3.1 lives in `/mnt/source/tools/borlandcpp/BCPP31.ZIP`, extracted to
`work/bc31/` (git-ignored).

| Binary | Compiler | Model | Evidence |
|---|---|---|---|
| GOLF.EXE | Borland C++ 3.1 (CONFIRMED startup) | large | entry matches `C0L.OBJ` 309/310 bytes; 93 `CL.LIB` modules |
| SHOPADV.EXE | Borland C++ 3.1 (CONFIRMED startup) | large | entry matches `C0L.OBJ` 309/310; 88 `CL.LIB` modules |
| RB.EXE (Red Baron) | Borland C++ 3.1 (CONFIRMED startup) | medium | entry matches `C0M.OBJ` 315/316; 93 `CM.LIB` modules |
| CGENN, DARKSTRT, TWGENN, FATES, Cawdor's TWGENN, RUINS | **Borland C++ 3.0 (1991)** | large | 65–81 `CL.LIB` modules; shared C0 object matches Borland's DOS startup but not 3.1's (pre-3.1 `__envLng` form, no 3.1 `__nfile` block); `Borland C++ - Copyright 1991 Borland Intl.` and VROOMM/FBOV overlays; not Microsoft C (private `COMPILER-FORENSICS.md`) |
| LSCITV.EXE | C, INFERRED Microsoft C; no compiler RTL linked | small/medium code, near-heavy | 310 C prologues; register saves after the prologue `push di/push si` 21x vs `push si/push di` 15x; VM core is hand-written assembly |
| TSNEXEC.EXE | C, INFERRED Microsoft C, own minimal runtime | small/medium | 98 C prologues in 14.8 KB; saves `push di/push si` 22x vs 3x; no `__chkstk`; custom start-up at 0000:0010; own DOS error-message table; printf-style formats |

The Ybarra startup is a BC++ 3.0 `C0L.OBJ`; the +38 divergence from 3.1 is not an overlay flag but a
different C0 version. BC++ 3.1 reproduces individual lifted functions byte-identically (two test
functions in `private/lifted/COMPILER-FORENSICS.md`), but startup/link bytes need BC++ 3.0 media, which
is not on this machine. Microsoft C 6.0a/6.00ax/7.0 is not used by these programs.

Register-save order is a heuristic: Microsoft C saves DI before SI after `push bp/mov bp,sp`, Borland
saves SI before DI. Neither binary contains Microsoft's runtime strings (`R6000`, `_C_FILE_INFO`).

## Use

- Runtime-library functions found by signature are not decompiled: the lifted build maps them
  to the runtime's libc equivalents.
- Matching decompilation is possible for the Borland titles: compile a lifted function with BCC 3.0
  (3.1 for a function-level check) under DOSBox-X or dosrt and compare bytes. This is a per-function
  check; the full link needs the BC++ 3.0 libraries.
