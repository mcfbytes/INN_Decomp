# The browser build

The same `inn` frontend compiled with Emscripten: SDL2 draws into a canvas, the C: drive lives in the
browser's file system, and COM1 is a WebSocket to `innkeeperd --ws-bind`. No Sierra file is part of the
build or of `runtime/web/`; the user supplies their own install.

## Building

```
source /mnt/source/tools/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-web -G Ninja
cmake --build build-web
```

Output in `build-web/`: `inn.html` (from `runtime/web/shell.html`), `inn.js`, `inn.wasm`, and the page
scripts `page.js` and `zip.js` copied from `runtime/web/`. The default build type is `Release` (`-O3`).

| CMake cache variable | Default | Effect |
|---|---|---|
| `INN_WEB_SUSPEND` | `ASYNCIFY` | How lifted code that waits inside a frame yields to the browser. `JSPI` (Chromium-based browsers: Edge, Chrome) suspends without instrumentation and leaves about 1.5x more headroom; `ASYNCIFY` works in every browser. |
| `INN_WEB_PRELOAD_DIR` | empty | A local C: drive (for example `work/inn-c`) packed into `inn.data` and mounted at `/c`. Development only: that file holds the user's media, so a build made with it must never be published. Changes in the directory need a relink (`touch runtime/web/shell.html`). |

Link settings worth knowing: `INVOKE_RUN=0` (the page calls `main` once the drive is ready),
`EXIT_RUNTIME=0`, `-lidbfs.js` and `-lwebsocket.js`, `FS` and `callMain` exported.

## Running

```
../TSN-INN-Decomp/target/debug/innkeeperd --bind 127.0.0.1:2314 --ws-bind 127.0.0.1:2316 --no-int14h &
python3 -m http.server -b 127.0.0.1 -d build-web 8080     # serves .wasm as application/wasm
# open http://127.0.0.1:8080/inn.html
```

The page asks for the install (unless `inn.data` provides one), then runs
`inn --com1 <server> /c INN\TSNEXEC.EXE DEFAULT` from `emscripten_set_main_loop`.

| Query parameter | Meaning |
|---|---|
| `com1=ws://host:port` | Server URL; default `ws://<page host>:2316` (`wss://` on an https page). The page also offers an editable field and remembers the last value. |
| `autostart=1` | Start as soon as an install is available (preloaded or saved), without the Start button. |
| `perf=1` | Pass `--perf`: a timing line in the log every 5 s (see Performance). |
| `ips=N` | Guest instruction rate (`--ips`), default 20000000. |

## The C: drive

| Source | How it gets to `/c` |
|---|---|
| preload build | `--preload-file DIR@/c`, in MEMFS before `main` |
| `.zip` | `zip.js` reads stored and deflated entries (no ZIP64, no encryption) with the browser's `DecompressionStream`; no third-party code |
| folder | `<input webkitdirectory>`, each file read with `File.arrayBuffer` |
| saved | IDBFS mounted at `/c` and loaded with `FS.syncfs(true)` |

The directory that holds `TSNEXEC.EXE` becomes `C:\INN`, whatever its name in the zip or folder, because
`LSCI.CFG` expects that path. Picking an install clears `/c` first. The client writes `VMF\SWAPFILE.*`,
`PLAYER.DIR`, `LANDADDR` and others; MEMFS takes the writes. With "Remember" ticked (the default), `/c` is
written back to IndexedDB after install, every 15 s, when the tab is hidden, and when the client exits,
so the next visit starts from the saved install with its player files. "Forget saved installation" clears
it. A preload build does not mount IDBFS.

## Serial over WebSocket

`runtime/src/uart_link_ws.c` is a `SerialLink` on `<emscripten/websocket.h>`: binary frames carry raw
serial bytes both ways, received bytes queue in a growing inbox until the UART paces them in, sends wait
(return 0) while connecting, and DCD/DSR/CTS follow the socket state as with the TCP link. `--com1` picks
it for `ws://` and `wss://` specs; the native build returns an error for those.

## The page

`shell.html` and `page.js` are a single-page app around the canvas; the DOS screens themselves are
unchanged. Setup is one card: drop the INN folder or a zip anywhere on the page (dropped folders are
walked with `webkitGetAsEntry`), or use the folder and zip pickers, then Connect. While running, the
app bar shows the COM1 state (taken from the runtime's `uart: websocket ...` log lines), mute (suspends
SDL's `AudioContext`), full screen, and a settings drawer with the display mode, storage, and the log.
When the client exits, an overlay offers to start again. Light and dark follow the system setting.

## Keyboard and screen

SDL listens for keys on `#canvas` only (`SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT`), so the page's own fields
keep working. The canvas takes focus at start and on mouse down, and a hint shows while it does not have
focus. The canvas is 960x720 and CSS scales it at 4:3 to fit the window (container query units); right-click
is the guest's right button, so the context menu is suppressed. Display modes: Sharp (nearest-neighbour),
Smooth (browser filtering) and CRT (filtering plus a 200-line scanline overlay); the choice is kept in
`localStorage`.

## Performance

`runtime/frontend/frame_clock.c` runs the machine once per animation frame for the wall time since the
previous frame. Emulated time owed is carried between frames, so slice rounding never drifts from real
time, and at most 50 ms is run per frame and owed in total, so a background tab or a slow host slows the
game down instead of making it race. `--perf` logs, every 5 s:

| Field | Meaning |
|---|---|
| speed | emulated time / wall time; 1.000 is real speed |
| host busy | share of wall time spent in `machine_run_for` |
| executed | guest instructions per wall second |
| capacity | guest instructions per second of `machine_run_for` time: the rate the host could sustain |
| dropped | wall time minus emulated time over the window (time the guest fell behind) |
| longest frame | longest single frame; audio runs dry if it exceeds the queue without yielding |

With `--perf`, a frame over 100 ms also logs `long frame`: its emulated time and how long nested
waits in lifted code slept. A frame that waits inside lifted code keeps pace with the wall clock and
keeps presenting and feeding audio; the frame clock does not owe that time back afterwards.

Measured 2026-10-14 in headless Chromium (Playwright build 1243, SwiftShader GL) on a Ryzen 9 9950X3D,
boot, dial, logon and town map, guest clock 20 MIPS: speed 1.000 throughout, host busy 22 to 44%,
capacity 45 to 91 MIPS across runs (2x to 4x headroom; the lower end with OPL synthesis running).
Raising the guest clock: 50 and 70 MIPS stay at 1.000; 90 MIPS falls to 0.84 to 0.99 with no runaway.

## Verifying headlessly

Playwright drives Chromium against the build: pick the zip with `setInputFiles('#pick-zip', ...)` or
open `?autostart=1` on a preload build, click Start, wait, then click Play at frame pixel (285, 59) of
the 320x200 frame scaled to the canvas box. The server log shows the Hayes dial, the PAD call, the Login
and the land directory; the canvas reaches the town map at about 40 s.
