# INN_Decomp

The ImagiNation Network (Sierra's 1993-94 online service client) as C that builds natively and
to WebAssembly, so it can run in a modern browser against the revival server `innkeeperd`.

| Path | What |
|---|---|
| `docs/PLAN.md` | Goal, architecture, milestones |
| `docs/runtime.md` | `dosrt`, the emulated PC every tier runs on |
| `docs/lifting.md` | How original functions are replaced by lifted C |
| `docs/compilers.md` | Which compiler built each original binary |
| `runtime/` | `dosrt` (CPU, DOS, BIOS, devices) and the SDL2 frontend |
| `tools/` | Analysis and conversion tools |

Build: `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build`.
Run: `build/inn [--com1 host:port] <c-drive-dir> 'INN\TSNEXEC.EXE' DEFAULT`.

Bring your own disks: no original media, extracted data or code derived from it is in this
repository. The build runs the game from your own installed copy.

## License

GPL-3.0-or-later (`LICENSE`), the same as the `innkeeperd` server. Third-party code must be
compatible with it (GPL-2.0-or-later, GPL-3.0, LGPL, MIT, BSD, zlib, Apache-2.0); each vendored
component lives under `third_party/<name>/` with its own license file and is listed in
`docs/third_party.md`.
