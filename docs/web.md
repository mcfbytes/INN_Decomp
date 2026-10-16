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
scripts `page.js`, `saves.js` and `zip.js` copied from `runtime/web/`. The default build type is `Release` (`-O3`).

The native client (INN as C, games still on the emulator) is the same page with two cache variables.
JSPI is required; Asyncify does not link this configuration:

```
source /mnt/source/tools/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-web-native -G Ninja -DINN_WEB_SUSPEND=JSPI -DINN_NATIVE=ON
cmake --build build-web-native
```

Serve `build-web-native/` the same way as `build-web/`. Chromium-based browsers run it. A browser
without JSPI does not; use the hybrid build above for that. When `private/lifted` is present it is
picked up (`INN_LIFTED_DIR`). Without lifted code the binary still builds, and the native boot reports
that TSNEXEC has no native entry and runs it on the emulator.

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
| `scene=<id>` | Landing backdrop: `town` (default, the flat pixel-art valley in `shell.html`), `valley`, `diner`, `tower` or `harbor`. An unknown id falls back to `town`. |
| `scenes` | Any value: shows a small picker at the bottom left to flip between the backdrops live (it also updates `scene=` in the address bar). |
| `perf=1` | Pass `--perf`: a timing line in the log every 5 s (see Performance). |
| `ips=N` | Guest instruction rate (`--ips`), default 20000000. |

## Landing scenes

`runtime/web/scene.js` is the engine and `runtime/web/scenes/<id>.js` one painting each. A scene module is
`{ id, title, focus, paint(ctx, kit), anims, hotspots }` registered with `InnScenes.register`. `paint` draws a 320x200
picture with Canvas 2D (soft gradients, wobbly brush shapes, dabs and grain); the engine adds a paper-and-wash texture,
cuts a 256-colour palette from the result (median cut) and maps it with an 8x8 ordered dither, so the load takes about
30 ms. The canvas is drawn with `image-rendering: pixelated` and covers the window, with the scene's `focus` kept beside
the setup card (on a narrow portrait screen it hangs from the bottom edge under the card).

Every scene has a day palette and a dusk palette; the dusk one is used when the browser is in dark mode. Small things move by
re-compositing only their own rectangle (`anims`: clouds, glints, blinking lights, and gags that replay every 20 s or so),
so the picture is never repainted per frame. All of it stops under `prefers-reduced-motion`. Hotspots are rectangles in
scene pixels; clicking one, or resting the mouse on it for a second, shows a short narrator line in a bordered text box.
Adding a scene: write `scenes/<id>.js`, add the id to `IDS` in `scene.js` and the file to the copy list in `CMakeLists.txt`.
The artwork is original code; nothing is traced or copied from any game.

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
serial bytes both ways (text frames are the save channel, next section), received bytes queue in a growing inbox until the UART paces them in, sends wait
(return 0) while connecting, and DCD/DSR/CTS follow the socket state as with the TCP link. `--com1` picks
it for `ws://` and `wss://` specs; the native build returns an error for those.

## Game saves on the server

Characters, automaps and the INN persona files normally live only in this browser's IndexedDB. Over the
same WebSocket, `innkeeperd` can keep them per account and put them back when the account signs on from
another browser. The host side (protocol, store, rules) is `docs/server/saves.md` in the TSN-INN-Decomp
repository; this is the browser side.

- **Channel.** Binary frames stay serial bytes. Text frames are JSON control messages: `uart_link_ws.c`
  hands each one to `Module.onControlText(text)` and never to the UART, and the exported
  `Module.sendControl(text)` sends one (a 1 MiB file fits; the text is staged in the heap, not on the
  stack). `runtime/web/saves.js` (`InnSaves`) is the only user.
- **Which files.** `INN\YSERBIUS\MYCHARS.DAT`, `AUTOMAPS.DAT`, `INN\TWINION\TWINCHAR.DAT`, `AUTOMAPS.DAT`,
  `INN\PLAYER.DIR` and `INN\*.DTA` except `PASS_SET.DTA`, which never leaves the browser; the same two in
  `INN\LL` and `INN\SL` (CasinoLand and SierraLand personas) and the CasinoLand bankroll `INN\LL$$<n>.DAT`
  (`LLSYSTEM.DAT`, CasinoLand's access password, stays local). The host
  enforces the same list. Names are matched without regard to case against the drive's own spelling.
- **When.** The host sends a manifest as soon as the line has logged on (`LoginAccepted`); before that it
  answers `not_logged_on`. For each path, comparing the local SHA-256 with the hash and version last
  synced (kept in `localStorage` per account):

  | Case | Action |
  |---|---|
  | only the host has it, or the host changed and the local file did not | download and write it (with the host's modification time) |
  | only the page has it, or the local file changed and the host's did not | upload |
  | both changed | upload anyway; the host keeps the later modification as current and the other as a previous version, and the page downloads if it lost |

  The manifest arrives about when the logon completes, before the player can pick a land, and CGENN and
  TWGENN read the character files after that.
- **Uploads.** `dos_file.c` and `dos_handle.c` call `dos_host_file_closed(dos_path)` when the program
  closes a file it wrote or renames onto a name; in the browser build that calls `Module.onFileClosed`,
  which uploads an allowlisted path 1.5 s later (debounced). Every changed file is also pushed when the tab
  is hidden, on `pagehide`, and when the client exits. Native builds do nothing there.
- **UI.** The settings drawer shows the sync state under Storage. A save that lost to a newer copy on the
  host raises a toast. "Forget saved installation" clears only this browser; the host's copies stay.
- **Failure.** If the host is unreachable or the account has not logged on, nothing is lost: the files stay
  in the drive and the next manifest reconciles them.

Verified 2026-10-14 in headless Chromium against a scratch `innkeeperd --data-dir`: the first install
uploaded its six save files after the logon, an edit closed through the hook became version 2, and a fresh
browser profile with an install that lacked the character files received all four, version 2 included.

## The page

`shell.html` and `page.js` are a single-page app around the canvas; the DOS screens themselves are
unchanged. The page has three views (`body[data-view]`): `setup`, `dialing` and `play`.

**Setup.** A full-viewport scene of original pixel art (inline SVG on a 320x200 grid, `crispEdges`, a
limited palette with dither seams): a town, autumn trees, a winding road, a telephone line and a volcano.
It follows the system colour scheme (day palette in light, dusk with lit windows in dark) through the
`--c-*` tokens at the top of the style block. Clouds, volcano smoke and the modem light animate unless
`prefers-reduced-motion` is set. On it sits one card: drop the INN folder or a zip anywhere on the page
(dropped folders are walked with `webkitGetAsEntry`) or use the pickers, then Connect. The server address
and "Remember on this device" are under Advanced. `SITE_NAME` at the top of `page.js` is the one place
for a name: empty (the default) shows no wordmark and the title "Online world"; a value is used for both.

**Providers.** The page fetches `<server http base>/auth/providers` (the server address with `ws` turned
into `http`, same host and port); a JSON array of `{id, name, url}` shows a "Sign in with ..." button per
entry. Any error, a 404 or an empty list keeps the section hidden, so there are no buttons without
providers.

**Dialing.** Connect starts the client and replaces the card with a modem panel: `ATZ`, `OK`,
`ATDT <server>`, then `CONNECT` when the runtime logs `uart: websocket open`, and the game view opens
1.4 s later. `uart: websocket closed|error` before that shows `NO CARRIER` and a Try again button.

**Play.** There is no app bar. The game fills the viewport at 4:3 on a dark letterbox. Controls are a
translucent strip that slides in when the pointer is within 8 px of the top edge (it stays while hovered
or focused and hides 1.5 s after the pointer leaves): COM1 state, save-sync state, mute (suspends SDL's
`AudioContext`), display mode (cycles Sharp, Smooth, CRT), full screen and the settings drawer (display
mode, server, storage, log, Forget). Toasts stay over the game. When the client exits, an overlay offers
to start again.

Native music is `adl.drv`. If `LSCI.CFG` names another music driver, the page shows a notice and
rewrites that line to `adl.drv` in the browser's copy of the file. Sound Blaster digital audio is
not played.

## Keyboard and screen

SDL listens for keys on `#canvas` only (`SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT`), so the page's own fields
keep working. The canvas takes focus at start and on mouse down, and a hint shows while it does not have
focus. Ctrl+Alt+F toggles full screen; the page takes it in a capturing `keydown` listener on `window`
before SDL sees it (the game does not use it). Esc and every other key go to the game. Full screen keeps
the same layout.

The canvas buffer is 960x720. `page.js` sizes its frame as the largest 4:3 box that fits the window
(`layoutFrame`), so mouse coordinates map at any size. Display modes: Sharp (nearest-neighbour; snaps to a
whole multiple of 960 when that costs under 4% of the size), Smooth (browser filtering) and CRT (filtering
plus a 200-line scanline overlay); the choice is kept in `localStorage`. Right-click is the guest's right
button, so the context menu is suppressed.

The system cursor is hidden over the canvas (CSS `cursor: none`, and `SDL_ShowCursor(SDL_DISABLE)` in the
Emscripten branch of `frontend_sdl.c`, since SDL writes the canvas cursor style); only the game's own
cursor shows. It is back over the letterbox, the strip and the drawer.

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

The native loop does not go through that frame clock. `--perf` there is the same line, taken at each
platform sleep: host time since the previous sleep returned is the busy slice, and a slice over 100 ms
logs `long frame`. `executed` counts guest instructions, so it stays near 0 while INN itself is native
and rises while a game runs on the emulator.

Measured 2026-10-14 in headless Chromium (Playwright build 1243, SwiftShader GL) on a Ryzen 9 9950X3D,
boot, dial, logon and town map, guest clock 20 MIPS: speed 1.000 throughout, host busy 22 to 44%,
capacity 45 to 91 MIPS across runs (2x to 4x headroom; the lower end with OPL synthesis running).
Raising the guest clock: 50 and 70 MIPS stay at 1.000; 90 MIPS falls to 0.84 to 0.99 with no runaway.

## Verifying headlessly

Playwright drives Chromium against the build: pick the zip with `setInputFiles('#pick-zip', ...)` or
open `?autostart=1` on a preload build, click Connect, wait for `body[data-view=play]`, then click Play at frame pixel (285, 59) of
the 320x200 frame scaled to the canvas box. The server log shows the Hayes dial, the PAD call, the Login
and the land directory; the canvas reaches the town map at about 40 s.

A click while a game is on the emulator needs a delay between button down and button up (about 200 ms).
A click that is released inside one frame is missed. At real speed the headless scenario's emulated
times are wall times, so the same script can drive the page.

Checked 2026-10-16 against the native JSPI build in headless Chromium: the land directory at 25 s, the
town map at the 50 s shot, the Yserbius gallery at 90 s and the map again at 135 s. After logon, `perf:`
speed stayed between 0.998 and 1.003, and no `long frame` line was logged.
