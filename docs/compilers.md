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
| CGENN, DARKSTRT, TWGENN, FATES | Borland C++ (version INFERRED 3.x) | large | 65-80 `CL.LIB` modules; entry diverges from `C0L.OBJ` at +38 (INFERRED: overlay-enabled startup, they use VROOMM) |
| LSCITV.EXE | unknown, no compiler RTL | - | 1 incidental match; VM core is hand-written assembly |
| TSNEXEC.EXE | unknown, no compiler RTL | - | 1 incidental match |

The single differing startup byte (+338 large, +350 medium) is not yet explained; INFERRED a
link-time constant.

## Use

- Runtime-library functions found by signature are not decompiled: the lifted build maps them
  to the runtime's libc equivalents.
- Matching decompilation is possible for the Borland titles (compile a lifted function with BCC
  3.1 under DOSBox-X or dosrt and compare bytes), as an optional check on top of the
  differential tests.
