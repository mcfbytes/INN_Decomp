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
scripts `page.js`, `saves.js`, `auth.js`, `adv.js`, `scene.js` (and `scenes/`) and `zip.js` copied from `runtime/web/`. The default build type is `Release` (`-O3`).

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
| `scene=<id>` | Landing scene: `valley`, `diner`, `tower`, `harbor` or `town`. Without it (or with an unknown id) one of the five is picked at random per visit. |
| `scenes` | Any value: shows a small picker at the bottom left to flip between the scenes live (it also updates `scene=` in the address bar). |
| `classic=1` | The old layout for debugging: the setup card (picker, drop zone, Advanced, Connect, modem panel) over the scene, with no menu bar, score or narrator box. |
| `perf=1` | Pass `--perf`: a timing line in the log every 5 s (see Performance). |
| `ips=N` | Guest instruction rate (`--ips`), default 20000000. |

## Landing scenes

`runtime/web/scene.js` is the engine and `runtime/web/scenes/<id>.js` one painting each (valley, diner, tower, harbor
and town). A scene module is `{ id, title, focus, paint(ctx, kit), anims, hotspots, objects, spark, termAt }` registered
with `InnScenes.register`. `paint` draws a 320x200
picture with Canvas 2D (soft gradients, wobbly brush shapes, dabs and grain); the engine adds a paper-and-wash texture,
cuts a 256-colour palette from the result (median cut) and maps it with an 8x8 ordered dither, so the load takes about
30 ms. The canvas is drawn with `image-rendering: pixelated` and covers the window, centred on the scene's `focus`. On a
narrow portrait screen the picture floats above a strip kept for the narrator box, fades into sky and ground at its
edges and can be dragged sideways.

Every scene has a day palette and a dusk palette; the dusk one is used when the browser is in dark mode, unless Options ▸
Time of day says "day" or "dusk" (stored as `inn.theme`: `light`, `dark`, or absent to follow the system; a `<head>`
script applies it before the first paint, and the music and legal pages follow it too). Small things move by
re-compositing only their own rectangle (`anims`: clouds, glints, blinking lights, and gags that replay every 20 s or so),
so the picture is never repainted per frame. All of it stops under `prefers-reduced-motion`. Hotspots are rectangles in
scene pixels; clicking one, or resting the mouse on it for a second, shows a short narrator line in a bordered text box.

Two hotspots per scene carry a `role`: `disks` (the object that holds the installation) and `phone` (the telephone). The
scene's `spark` gives a rectangle for each, where a few pixels twinkle (a burst of a second every 2.6 s; a still
twinkle when animation is off) while that object is the next step. `objects` holds the scene's own lines for each role
(`open`, `got`, `again`, `hint` for the disks; `refuse`, `answer`, `hint`, `busy` for the phone) and `termAt` the corner
where the modem box sits, on empty grass, sky or water. The five scenes:

| Scene | Disks | Telephone |
|---|---|---|
| valley | a crate of floppies by the road | a wall phone on the big pole, the repairman answers "Number please?" |
| diner | the chalkboard special "TODAY DISKS" and a plate of floppies | a payphone by the big sign |
| tower | a spellbook on a lectern, a floppy embossed on its cover | a candlestick telephone on the balcony, by the switchboard window |
| harbor | a crate marked DISKS with arrows, floppies poking out | a wall phone on the lighthouse |
| town | a box of disks in the post office window | a red telephone booth, a snail first in the queue |

Adding a scene: write `scenes/<id>.js`, add the id to `IDS` in `scene.js` and the file to the copy list in `CMakeLists.txt`.
The artwork is original code; nothing is traced or copied from any game.

## Music

The landing plays one short looping background track per scene. It is a tiny sequencer and synthesizer in
`runtime/web/music.js` on the Web Audio API, and the songs are text in `runtime/web/music/<scene>.js`. There are no
samples, no MIDI files and no third-party code, and the whole thing (code and all five songs) is about 35 KB. Every
composition is original work, written for this project, and is licensed with it under GPL-3.0-or-later. None of
them quotes or imitates an existing tune.

| Scene | Title | Key, meter, tempo | Form |
|---|---|---|---|
| town | Main Street, Mostly Paved | F major, 4/4, 104, light swing | AABA, 32 bars |
| valley | Hay Fever Waltz | G major / E minor, 3/4, 92 | A B A C A, 40 bars |
| diner | The Pie Is Not a Metaphor | A blues, 4/4, 100, shuffle | three 12-bar choruses |
| tower | The Stairs Have Opinions | D Dorian, 4/4, 72 | AA'BA'', 32 bars |
| harbor | Buoy Meets Gull | G Mixolydian, 6/8, 100 | AA'BA'', 32 bars |

### Synthesis

The sound is meant to recall an early-90s FM card: every voice is two operators. A modulator oscillator (at a set
ratio to the note) drives the frequency of a carrier oscillator, and both the modulation index and the carrier level
have their own envelopes, so a pluck is bright for a moment and then mellows, and a brass note swells in. Optional extras
are a delayed vibrato and a second, slightly detuned carrier. Eight patches are defined in `PATCH`: `harpsichord`,
`flute`, `strings`, `brass`, `bass`, `bell`, `organ` and `pluck`. Percussion (`k` kick, `s` snare, `h` closed hat,
`o` open hat, `r` rim) is a pitch-swept sine and filtered noise from a fixed noise buffer. The mix is up to 8 voices at
once (a new note steals the voice nearest its end, with a 20 ms ramp), then a gentle compressor, and the overall level is
around -18 dBFS RMS with peaks below -5 dBFS. Every gain change is a ramp; nothing steps.

Scheduling is a look-ahead: a 40 ms timer schedules the next 0.45 s of notes on `AudioContext.currentTime`, so the timer's
jitter never reaches the audio and the loop point is sample-accurate. `engine(ctx, out)` works on any context, so the
same code renders offline.

### Notation

A song is `InnMusic.song({ id, title, bpm, bar, bars, swing, sig, chords, parts })`. The grid is a sixteenth note (a
"step"; `bpm` counts quarter notes, four steps each), `bar` is the steps per bar (16 for 4/4, 12 for 3/4 and 6/8) and
`swing` delays the off-beat eighths by that fraction of an eighth. `chords` has one entry per bar (`'F'`, `'D7'`, `'Gm'`,
`'Am7'`, `'F.D'` for two chords splitting the bar); every part follows the song for `bars` bars and the last bar leads
back into the first.

A part is `{ i: patch, v: volume, pan, d: patch to use at dusk }` plus one of:

- `n`: notes, as text with `|` between bars. ABC-like pitches: `C D E F G A B` is the octave from middle C, `c d e f g a b` the
  next, `c'` above and `C,` below; `^` sharp, `_` flat, `=` natural before the note (`sig: '_B'` flats every B). A number
  after a note or rest (`z`) is its length in steps; `l<n>` sets the default length, `v<n>` the velocity (1 to 15);
  `[CEG]4` is a chord, `!` makes a note short, `~` ties it to the next note of the same pitch. Each bar must add up to
  `bar` steps; the engine warns in the console for any that do not.
- `p`: a generated accompaniment from the chord list, one pattern string per bar (cycled), one character per step: `1 3 5 6 7 8`
  chord tones in the bass range, `f` the fifth below, `n` a chromatic approach to the next chord's root, `c` the whole chord,
  `x y z w` the first to fourth tone of the chord in the part's `lo` range, `.` holds and `-` rests.
- `i: 'drums'` with `p`: drum patterns, one string per bar (cycled); `(kh)` plays two at once and capitals are accents.

### Adding a track

Write `music/<id>.js` calling `InnMusic.song`, add it to the copy list in `CMakeLists.txt`, and name it by scene id.
To check it without listening, render it: `parse` errors (bars that do not add up) are listed by `InnMusic.compile(song).errors`.

### Playback rules

- Nothing is created before a user gesture and there is no "enable audio" prompt. The first click or key press once the
  18+ notice has been answered starts it, and that includes the "I am 18 or older" click; a returning visitor starts it
  with their first click or key press. It stays silent on the refusal screen.
- Changing the scene crossfades to the new scene's track in about a second. When the iris into the game starts it fades out
  over 1.5 s (0.3 s with reduced motion), then the context is closed for good, so it never overlaps the game's own sound.
- While the tab is hidden the context is suspended; it resumes when the tab is shown.
- Controls: Options, Music: on/off; the note button at the right of the menu bar (`aria-pressed`); the M key on the
  landing (not while typing in a field, not with a dialog open, never in the game view). The choice is kept in
  `localStorage` as `inn.music`. The default is on, except that `prefers-reduced-data` or the Save-Data hint make it off
  until the visitor turns it on. Reduced motion does not affect music.
- A "Now playing: <title>" line shows for three seconds when a track starts.
- In dark (dusk) mode a track plays 10% slower and 20% softer, and a part with a `d` patch uses that patch instead.

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
  While a signed-in line is dialed `PLAYER.DIR` and the `.DTA` files are that account's own: the drive shows
  one view of them at a time (see "The persona on the drive"). On the way back from the host, a `.DTA` is
  written only when the `PLAYER.DIR` beside it names it.
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
  closes a file it wrote or renames onto a name; in the browser build that calls `Module.onFileClosed`.
  Native builds do nothing there. The page keeps the DB quiet in four ways:
  - *Debounce with a cap.* A path is uploaded once it has been quiet for 5 s, and at the latest 30 s after
    the first change of a burst, so a program that rewrites a file 20 times in a minute produces one or two
    uploads. (Tested: 20 rewrites in 8 s gave one upload.)
  - *Hash first.* Before any upload the page hashes the local file and compares it with the hash last
    synced; identical bytes put nothing on the wire, and if they did the host would write no version
    (`unchanged`). The host keeps 3 versions per file.
  - *Sweep.* Every 5 minutes while the tab is visible and the line is logged on, the allowlisted files are
    re-hashed and the ones that differ are uploaded. This catches a program that keeps a save file open
    (no close hook fires) or writes without closing. A hidden tab skips it, because hiding already pushed.
  - *Pushes.* Every changed file is also pushed when the tab is hidden and on `pagehide` (best effort: the
    frames are queued before the page goes away; Chromium delivered them for both a tab close and a
    navigation in headless tests, and the menu's Hang up is a reload), when the client exits, and at log
    off (next item).

  The timings are `debounce`, `maxWait` and `sweep` in `saves.js`; `?savedebounce=`, `?savemaxwait=` and
  `?savesweep=` (milliseconds, 0 turns the sweep off) override them for tests.
- **Log off.** The client hangs up like a modem user: it sends `+++` (three one-byte frames), then `ATH0`.
  `innkeeperd` ends the line, and with it the save channel, when it reads `ATH0`; from then on every
  message gets `not_logged_on`. The browser build's exit hook (`onGuestExit`) runs only after that, about
  0.2 s after the host stopped listening, so an upload sent then is refused. `uart_link_ws.c` therefore
  watches the outgoing serial bytes and calls `Module.onLineEscape` at the third consecutive `+`, and the
  page pushes every changed save file at that moment (`InnSaves.flushFinal`: all sends happen
  synchronously, with no wait). WebSocket frames reach the host in order, so those puts are handled before
  `ATH0` is read; in the test the host acknowledged them about 20 ms later and the client closed the socket
  1.4 s after that. `flushFinal` does not skip a path whose upload is already in flight (that one may carry
  older bytes). A false positive (a `+++` inside ordinary data) only costs a hash pass; nothing is sent when
  nothing changed. The exit hook and `pagehide` call the same function for the cases that end without a
  hang-up.
- **UI.** The settings drawer shows the sync state under Storage, "Last saved to the server: <time>" and a
  "Save to server now" button, which pushes everything changed and waits (up to 3 s) for the host to
  answer. Toasts are for problems only: a save that lost to a newer copy on the host, and a "Save to server
  now" that got no answer. "Forget saved installation" clears only this browser; the host's copies stay.
- **Failure.** If the host is unreachable or the account has not logged on, nothing is lost: the files stay
  in the drive and the next manifest reconciles them.

Verified 2026-10-16 in headless Chromium against a scratch `innkeeperd --data-dir`: a file changed with
no close was uploaded by the sweep; 6 identical rewrites sent nothing; 20 rewrites in 8 s gave one upload;
"Save to server now" stored a change and updated the time; two changed files were stored at a real log off
through the game's Exit and Yes (the `+++` hook), where before this change they were refused with
`not_logged_on`.

Verified 2026-10-14 in headless Chromium against a scratch `innkeeperd --data-dir`: the first install
uploaded its six save files after the logon, an edit closed through the hook became version 2, and a fresh
browser profile with an install that lacked the character files received all four, version 2 included.

## The page

`shell.html` and `page.js` are a single-page app around the canvas; the DOS screens themselves are
unchanged. The page has three views (`body[data-view]`): `setup`, `dialing` and `play`.

**Setup.** The landing is a scene (see Landing scenes), one of five painted ones picked at random per visit, and
no card sits over it. It follows the system colour scheme (day palette in light, dusk with lit windows in dark).
Setup is a small point-and-click game (`adv.js`; `page.js` calls its hooks):

1. *Get the disks.* The scene's disks object sparkles. Clicking it opens the folder picker and the narrator offers
   "Or perhaps a disk image (.zip)?" with `[Folder]` and `[.zip]` buttons. Dropping a folder or a .zip anywhere on the
   picture works too (dropped folders are walked with `webkitGetAsEntry`). After loading, a small disk icon appears
   top right and the narrator says so. A remembered install shows the icon from the start and the narrator greets the
   returning player.
2. *Dial.* With disks loaded, the telephone object sparkles. Clicking it (or Game, Dial) starts the client. Without
   disks it refuses with a joke. If nothing happens for about 10 s the narrator nudges toward the sparkling object, at
   most twice. Clicking empty scenery gives a dry remark.
3. *Errors* that the card used to show (no `TSNEXEC.EXE`, a bad zip, an invalid server address, a lost connection)
   appear in the narrator box with the same facts, the joke after them.

The menu bar slides down when the pointer touches the top edge (always visible on touch screens and narrow windows, and
while a menu is open). F10 or Alt opens it; the arrow keys move, Enter picks, Esc closes. It carries the ARIA menubar
roles and does everything the picture does: Game (Load disks..., Load disk image (.zip)..., Dial, Hang up, and with
sign-in configured "Sign in..." or, once signed in, "Who's playing..." and "Sign out"), Options (Server address..., Remember on this device, Animation), Scene (the five
scenes) and Help (How do I play?, About). Server address opens a small dialog with the field the card's Advanced section
had; it accepts only `ws://` or `wss://` addresses (empty means the default) and uses the same `inn.com1` key, stored
when you dial. "Remember" is the same `inn.remember` checkbox. A playful score on the right ("Score: 0 of 1993") is kept
in `sessionStorage`: loading the disks +10, dialing +5, connecting +25, each gag hotspot +1, each once per session. The
Animation switch is kept in `localStorage` (`inn.anim`).

`?classic=1` keeps the old card instead (folder and zip pickers, Advanced, Connect, modem panel): its markup is still in
`shell.html` and `page.js` keeps driving it, so the two layouts share one code path. `SITE_NAME` at the top of
`page.js` is the one place for a name: empty (the default) shows no wordmark and the title "Online world"; a value is
used for both.

**Sign-in.** Off by default: `AUTH_SERVER` at the top of `page.js` is empty, and innkeeperd's WebSocket
port does not answer HTTP. With `AUTH_SERVER` set to a URL, or `?auth=<url>` in the page's address, the
page fetches `<that origin>/auth/providers`; any other non-empty `?auth=` value asks the game server's
address with `ws` turned into `http` (same host and port). With no providers (an error, a 404 or an empty
list) nothing changes: phone, then dial. With providers, the flow below runs (contract in the next section).

1. *Who's calling.* Clicking the telephone (or Game, Dial) with the disks loaded makes the operator ask
   "Who's calling, please?". The narrator box offers a row of calling cards: a small pixel card per provider with
   its configured name as lettering (no logos or brand marks), plus a dashed "I have my own number" card, which is
   the classic path: no sign-in, the account already in the install. Game, "Sign in..." offers the provider cards
   only, and works without disks.
2. *On hold.* A card opens the login in a popup (`window.open` straight from the click) and the narrator says the
   operator has put you on hold (tiny hold-music notes; "Hang up on the operator" closes the popup). If the popup
   is blocked, the page redirects in the same window instead and takes the result from the address fragment.
3. *The card.* On success a calling-card icon joins the disk icon in the inventory and the score gains 5. Each
   INN account a sign-in owns is a line: an account number and a password, and a persona (a name, a face) that the
   original client makes at its own Select Player screen after the dial (see "The persona on the drive"). A first
   sign-in has no account, so the page opens one with no name and no dialog, and the narrator says "Welcome,
   Caller 7F3A! Your number is 100003. The client will ask who you are, so dress nicely." before dialing. One account
   is used straight away; several give "And who's playing today?" with a button per line (the persona, or "New
   caller (100003)" while it has none), plus "Someone new, please" (another empty account) when the server says
   `canCreate`, and "My own number".
4. *Credentials, then dial.* The page fetches the account's credential and writes it into the browser's copy of
   the install (below), then dials with the session token on the WebSocket address. The modem box gains an
   `AT+CARD=<provider id>` line before `ATDT`.
5. *Errors* are narrator boxes, the facts first: the popup closed without signing in, the provider's Deny
   (`access_denied`) or failure (`provider_error`, `server_error`), an expired session (401: the token is dropped
   and the cards come back), closed enrolment, a new line refused for today's limit (429), an age and terms
   answer the server wants again (403 `attest_required`: the notice is shown again and the call repeated once),
   a busy server (503) and an unreachable one. The table in the next section lists each. Everything is reachable by keyboard: F10, Game, "Sign in...", Enter on a card, the
   buttons of the narrator box (Left and Right move between them), the dialog.

Game, "Sign out" calls `POST /auth/logout`, forgets the token and has the narrator say goodbye; Help, About names
the signed-in identity. `?classic=1` keeps only the old "Sign in with ..." links (full redirect, `mode=redirect`),
and the token that comes back is kept but the classic card does not use it.

**Dialing.** Dial starts the client and opens a period-style terminal box near the telephone (`termAt`; docked at
the bottom on a phone): `ATZ`, `OK`, `ATDT <server>`, then `CONNECT` when the runtime logs `uart: websocket open`. 1.4 s
later an iris closes on the telephone and opens on the game view (about 0.8 s; instant under reduced motion or with
Animation off). `uart: websocket closed|error` before that shows `NO CARRIER` and a playful line in the same box with
`[Try again]` and `[Hang up]`. The runtime cannot be started twice, so both reload the page; Try again dials again by
itself on the new load when the disks are still there (kept with Remember).

**Play.** There is no app bar. The game fills the viewport at 4:3 on a dark letterbox. Controls are a
translucent strip that slides in when the pointer is within 8 px of the top edge (it stays while hovered
or focused and hides 1.5 s after the pointer leaves): COM1 state, save-sync state, mute (suspends SDL's
`AudioContext`), display mode (cycles Sharp, Smooth, CRT), full screen and the settings drawer (display
mode, server, storage, log, Forget). Toasts stay over the game. When the client exits, an overlay offers
to start again.

Native music is `adl.drv`. If `LSCI.CFG` names another music driver, the page shows a notice and
rewrites that line to `adl.drv` in the browser's copy of the file. Sound Blaster digital audio is
not played.

## Sign-in (server contract)

What the page expects from `innkeeperd` (or any server that offers sign-in). `innkeeperd` implements it
(TSN-INN-Decomp `docs/server/innkeeperd.md` section 8, with the choices it made where this contract is open in 8.6);
`tools/mock_auth.py` implements it too, for developing the page without a server. The page was driven end to end
against the real server on 2026-10-16: sign-in through a mock OpenID Connect provider, a new account and line, the
Login, the town map. Terms: the *page origin* is where `inn.html` is served, the *auth origin* is the
server's HTTP origin (`?auth=`, `AUTH_SERVER`, or the game server address with `ws` turned into `http`).

**Endpoints** (all on the auth origin; JSON bodies; errors are `{"error": "<code>", "message": "<words>"}`, and the
page shows `message` for a 4xx it does not know):

| Request | Success | Other |
|---|---|---|
| `GET /auth/providers` | 200 `[{"id": "example", "name": "Example Phone Co.", "url": "/auth/login?provider=example"}]` | anything else, or `[]`: sign-in stays off. `url` is relative to the auth origin or absolute on it; any other origin is ignored. `name` is shown as text only. |
| `GET <url>&return=<page URL>&mode=popup` | the login (consent) page; ends on `/auth/callback` | 400 for an unknown provider or a `return` whose origin is not an allowed page origin |
| `GET /auth/session`, `Authorization: Bearer <token>` | 200 `{"identity": {"handle": "Caller 7F3A", "provider": "<id>"}, "accounts": [{"id": 100001, "persona": "Guybrush"}, {"id": 100002, "persona": null}], "canCreate": true, "attested": {"age": 18, "terms": 1}}` (`attested` is `null` when none is on record) | 401: token missing, unknown or expired |
| `POST /auth/attest`, `{"attest": "18+v2"}` | 204; the answer is stored on the identity | 400 `bad_attest`; 401 |
| `POST /auth/accounts`, `{}` (an empty body works too) | 201 `{"id": 100002, "persona": null}`: an account with a number and a password and no persona yet. A body `{"persona": "Elaine"}` still works (1 to 10 of A-Z, a-z, 0-9, unique without regard to case) and answers `"persona": "Elaine"`; the page no longer sends it | 401; 403 `enrolment_closed`; 403 `attest_required`; 400 `bad_persona`; 409 `persona_taken` (only with a given persona); 429 `rate_limited` |
| `POST /auth/accounts/<id>/credential` | 200 `{"account": 100002, "password": "K3J9QWZ7"}`; every call rotates the password | 401; 404 when the identity does not own `<id>` |
| `GET /auth/accounts/<id>/saves` | 200 `{"account": 100002, "files": [{"path": "INN\\ELAINE.DTA", "version": 3, "hash": "<sha-256 hex>", "size": 69, "mtime": 1760000000000, "current": true, "data": "<base64>"}]}`: the files the save channel would hand that account, with `data` for as many as fit in 2 MiB in all (the rest come without it); `?path=<DOS path, URL-encoded>` answers one entry, always with `data` | 401; 404 `no_such_account` when the identity does not own `<id>`; with `?path=`: 400 `bad_path`, 404 `no_such_file` |
| `POST /auth/logout` | 204 and the token is revoked | the page forgets the token whatever the answer |

What the page does with the answers it does not just show:

| Answer | The page |
|---|---|
| 401 from any call | drops the token, brings the calling cards back ("Your sign-in has expired (the server answered 401)") |
| 403 `attest_required` on `POST /auth/accounts` (or the credential) | asks the age and terms notice again whatever this browser remembers, sends `POST /auth/attest`, and repeats the call once; a second refusal is shown as a failure |
| 429 `rate_limited` on a new line | says the card has reached its limit for the day (429), marks `canCreate` false and offers the lines it has |
| 404 `no_such_account` on a credential | says the line is not on this card (404) and offers the list again |
| 503 `busy`, other 5xx | "The switchboard is busy" or "The sign-in server had a problem", with the status, and a retry hint |
| callback `error`: `access_denied` | "Sign-in was declined at <provider>" |
| callback `error`: `provider_error`, `server_error` | the provider (or the game server) "could not sign you in / finish the sign-in (it reported an error)" |
| any other callback code | "Sign-in did not finish (<code>)" |

Every message is a narrator box with the fact first (what failed, the status or code), then a dry remark.

`persona` is `null` until the original client has named one: its Select Player screen makes the persona (name,
face, hobbies), and the name it presents on its 40/4 after the Login becomes the account's persona on the server
(TSN-INN-Decomp `docs/server/innkeeperd.md` sections 5 and 8.4). The page shows it, or "New caller (<id>)" while it is
`null`.

`id` is a positive integer below 2^32 (it becomes the `id` key of `LSCI.CFG`). `password` is 6 to 10 of A-Z and
0-9 (the stored form is 6 to 10 characters; innkeeper `docs/protocol/messages.md` section 4.1); the page refuses
anything else. `identity.provider` is one of the listed provider ids and appears in the modem line as
`AT+CARD=<provider>`. `canCreate` is true when enrolment lets this identity open another account.

**A PII-free sign-in.** The age notice and the legal pages are in "Legal pages and the age notice" below; the
contract that goes with them is:

- *Scope.* The login requests the `openid` scope only. The server MUST NOT request `email`, `profile` or any other
  scope. For a plain-OAuth provider (one with no OpenID Connect) it uses only the stable numeric or opaque user id
  and ignores whatever else the provider returns (a username, say), without storing it.
- *Stored identity.* The server stores `identity_key = HMAC-SHA256(server_secret, issuer + "\0" + subject)`, never
  the raw subject. A raw `sub` is a persistent identifier that follows a person across services; keyed with a secret
  the server holds, the stored value cannot be reversed from the database alone and cannot be linked to another
  deployment's data. Under GDPR this is still *pseudonymous* data, not anonymous (whoever holds the secret and the
  provider identifier can recompute it), but no email, name or profile is ever stored. The raw identifier itself
  is kept only sealed, as the innkeeperd audit record (see `docs/legal.md`); in routine operation the server reads
  only the HMAC.
- *Handle.* `identity.handle` is a pseudonymous label derived from the key (for example `Caller 7F3A`, four hex
  digits). No real names appear anywhere: the page shows the handle in the narrator line and in Help, About.
- *Attestation.* The login URL carries `attest=<minimumAge>+v<termsVersion>` (for example `18+v1`), the player's
  answer to the age and terms notice. The server stores `adult_attested_at` (a date) and `terms_version` on the
  identity and returns them as `attested` in the session JSON. When `attested` is missing or its `terms` is older
  than the page's `termsVersion`, the page makes sure the notice for the current terms has been answered (asking
  again if not, and showing the closing screen if the answer is no) and sends `POST /auth/attest`.
- *This is self-declaration, not verification.* Mainstream OIDC providers give no reliable 18+ claim without the
  user's birthdate or other personal data, which we choose not to collect.

**Login handshake.** The page opens `<url>&return=<page URL without fragment>&mode=popup` with `window.open` on the
click. The server runs the provider's login, then on `/auth/callback` serves a tiny page whose script does
`window.opener.postMessage(msg, <origin of return>)` and `window.close()`, where `msg` is
`{"type": "inn-auth", "session": "<token>"}` or, when the player declined or the provider failed,
`{"type": "inn-auth", "error": "access_denied"}` (any other code is shown as "did not finish"). The page accepts a
message only when `event.origin` is the auth origin, `event.source` is the popup it opened and `type` is `inn-auth`.
The `return` address must be checked against the configured page origins (an open redirect and a wildcard
`postMessage` target would leak the token). If the popup is closed with no message the page says so.

*Fallback.* When `window.open` returns null the page navigates itself to the same URL with `mode=redirect`. The
server answers the callback with a 302 to `<return>#inn-session=<token>` (or `#inn-error=<code>`). On load the page
reads the fragment, stores the token, cleans the address with `history.replaceState` and continues from the account
step. The disks are still there if "Remember on this device" was on (the saved install); otherwise the narrator asks
for them again.

**The token** is opaque to the page. It is kept in `sessionStorage` (`inn.session`), and also in `localStorage`
when "Remember on this device" is on; every storage access is in try/catch. A 401 from any call drops it from both.
The page also keeps the last chosen line in `sessionStorage` (`inn.line`) so "Try again" after a failed dial
redials the same account (it fetches a fresh credential, which rotates the password).

**CORS.** The page origin differs from the auth origin, and requests carry `Authorization` and a JSON body, so the
browser preflights. For the `/auth/*` JSON calls the server answers `OPTIONS` with 204 and sets on every answer:
`Access-Control-Allow-Origin: <the page origin, exactly, never *>`, `Vary: Origin`,
`Access-Control-Allow-Methods: GET, POST, OPTIONS`, `Access-Control-Allow-Headers: Authorization, Content-Type`
(and optionally `Access-Control-Max-Age`). No cookies are used, so no `Allow-Credentials`. The login and callback
pages are plain navigations and need no CORS. Send `Cache-Control: no-store` on all of it.

**The WebSocket.** Browsers cannot set headers on a WebSocket. The page appends the token to the address it gives
the runtime as `?session=<token>` (`ws://host:port/?session=...`); `page.server`, the modem lines and the status
text keep the plain address. A subprotocol (`inn-session.<token>`) would keep the token out of access logs, but
`uart_link_ws.c` opens the socket through `emscripten_websocket_new` with no protocol list and the address is the
only thing `--com1` carries, so the query parameter is what the page does. `innkeeperd` takes the token only on
the upgrade request, keeps it out of its logs, gives it a short lifetime (`--session-hours`) and binds that
connection to the identity (a Login is accepted only for an account the identity owns). With `--public` an upgrade
with no live token is a 401. One known leak: if the runtime cannot open the socket it prints the address it was given, which the page's log
(Copy log) then holds.

**What goes into the install.** Before dialing, `page.js` (`writeCredentials`) writes, for the account chosen:

- every `LSCI.CFG` in the drive (the root one and the `SL` and `LL` copies the installer made): the line
  ` id = <account>` is replaced, or appended with the file's own line ending. The account number is the `id` key
  (`docs/protocol/messages.md` section 4.1 item 3; `docs/dosbox.md` section 2 shows ` id = 100001`).
- `INN\PASS_SET.DTA`: `AB` followed by the 10-byte stored form of the password (messages.md section 4.1 items 1 and 2:
  uppercase it, XOR byte i with `"M34546788S"[i]`, zero-pad to 10 bytes, then for i = 0..n-1 XOR byte
  `(i+1) mod n` with byte i). With `AB` the client sends it unchanged as the Login's password with `fromFile` 1
  (captures.md section 4). `tools/test_auth.js` checks the encoder against that capture: `SWORDFISH` gives
  `1d 7a 01 66 16 66 18 73 03 00`.

The client sends the persona it picked in Select Player as the Login's name and again in a 40/4; `innkeeperd`
accepts the Login and stores the 40/4 name as the account's persona. "I have my own number" uses whatever the install
now holds, which after a sign-in dial is the last credential written. `writeCredentials` then gives the drive the
account's own view of the persona files and fetches them (below). It writes no persona of its own.

**The first sign-in.** After the sign-in, an identity with no account gets one at once, with no question: the page
posts `{}`, the narrator says "Welcome, Caller 7F3A! Your number is 100003. The client will ask who you are, so dress
nicely.", and the line is dialed. Several accounts: "And who's playing today?" shows one button per account, the
persona or "New caller (100003)", then "Someone new, please" (another empty account, no dialog) and "My own number".

### The persona on the drive

The web makes only the account. The persona is made by the original client itself: Select Player lists what
`INN\PLAYER.DIR` names and reads each persona from its `.DTA`, and its Create button runs the client's own editor
(name, age and town, then the look, hobbies and skills). A brand-new account therefore starts with no `PLAYER.DIR`
at all: the client says its welcome, offers the INN Guide, and opens a Select Player screen with only Create enabled.
(Checked with a missing `PLAYER.DIR`, in headless Chromium: Play, Change and Delete are greyed out and Create is
live.) The page writes `GUIDE.CFG` (two zero bytes) when it is missing; the welcome and the Guide question still
come, since they follow from having no persona.

*PLAYER.DIR* is a text file: one persona name per line, each ended by CR LF (`Zelda`, CR, LF). The *persona
file* is `<first 8 characters of the name, upper case>.DTA` in the same directory. Its layout, which the client
writes and the page only copies: the word 12, the home town (length word, text, zero), eight words (three look
words, two hobby words, age, two mail box words), 16 skill levels, 8 preference bytes and 9 words of cursor
positions, 65 bytes plus the town. The same pair of files exists in `INN\LL` and `INN\SL`.

**One view of the persona files at a time.** One INN account can hold several personas, and the drive is one
install shared by every account that signs in on this browser, so the page keeps only one view's `PLAYER.DIR` and
persona `.DTA` files (the hub's, CasinoLand's and SierraLand's; never `PASS_SET.DTA`) in the places the client reads.
A *view* is `own` (whatever the install came with, say the zip's Guybrush) or an account number. The other views
wait in `INNVIEWS/<view>/INN/...` at the top of the drive, with their modification times, and
`INNVIEWS/ACTIVE` names the view on the drive (missing means `own`). `InnSaves.useView(view)` (`saves.js`) stashes
the active view's files and restores the wanted view's, moving files and never deleting them, so the install's
personas are never lost and are put back byte for byte:

- `writeCredentials` (a signed-in line) calls `useView(<account>)` and then `prefetchSaves`. The client sees only
  that account's personas. Two accounts on one install never see each other's, and a new account's view is empty.
- `start()` without a session (a classic dial, "My own number") calls `useView('own')`.
- None of this is uploaded: files in `INNVIEWS` are not on the save allowlist. What syncs is the active view's
  files, as before (`INN\PLAYER.DIR`, `INN\*.DTA`, the `LL` and `SL` pairs), now in the account's save space, so a
  persona created in one browser is there in another.

A persona the client creates reaches the server at the first manifest after the Login (the page then uploads every
changed file), and the client's 40/4 sets the account's persona name there.

**PLAYER.DIR is authoritative.** The save channel has no delete. If the player deletes a persona in the client
(Select Player, Delete), the client rewrites `PLAYER.DIR` without the name and removes the `.DTA`, but the host still
holds the `.DTA`. A `.DTA` therefore comes back from the host (the HTTP prefetch or the manifest) only when the
`PLAYER.DIR` on the drive names it (`mayRestore`; `PLAYER.DIR` itself is handled first). The new, shorter
`PLAYER.DIR` is uploaded as the account's current one. A stray `.DTA` that is already on the drive still follows the
ordinary rules, and the host keeps its history for a restore by the operator.

**Fetched before the dial.** The client reads its personas once, at Select Player, before the host's manifest
exists (the manifest comes after the Login), so a browser that has never held the account would show an empty list
(and its Create would then write over the host's files). `writeCredentials` therefore calls `prefetchSaves`
(`page.js`) after `useView`: `GET /auth/accounts/<id>/saves` with the session token (8 s timeout), then
`InnSaves.prefetch` (`saves.js`) writes each file the line may sync (never `PASS_SET.DTA`, never `INNVIEWS`; a `.DTA`
only if `PLAYER.DIR` names it) into the drive with the host's modification time and records its version and hash as
synced (the per-account `localStorage` record), so the first manifest neither uploads nor downloads. Files the
listing left without bytes are fetched one by one with `?path=`. A hash that does not match is refused and counted. A
local file is replaced when it is missing or has not changed since the last sync, and also when this browser has never
synced the account at all, since then nothing in the install can be a newer copy of the account's (the install's own
starting `MYCHARS.DAT`, say): the browser's file is kept beside it with the extension `.OLD`, which is not a save name
and never syncs. Any other differing file is left for the manifest and the host's modification-time rule.

If the call fails (the server does not answer, 5xx, a hash mismatch) the line is dialed anyway: the narrator says in
a box that the saved files could not be fetched and why, then dials after a pause of about 3 s, and the modem output
gets `; SAVES NOT FETCHED (...)`. "I have my own number" does not call the route. `tools/test_saves.js` covers the
views, the prefetch rules and the `PLAYER.DIR` rule against a scripted host and an in-memory drive;
`tools/mock_auth.py --save ID:PATH=FILE` serves the route for page-only runs, and its `POST /auth/accounts` takes
`{}`.

Verified 2026-10-16 in headless Chromium against the real `innkeeperd` and `dev-oidc` (one run plain, one with
`--public --sandbox`; "My own number" needs the plain run, since a public host takes no line without a sign-in): a
first sign-in with a new subject showed no dialog, the narrator welcome, and a Select Player with only Create. The
persona Zelda was created in the client's editor (name, age, town, a changed hair style and clothes, Save) and
played to the town map; the host held the account's persona as Zelda and the session JSON listed it. After the game's
Exit, `ZELDA.DTA` (71 bytes) and `PLAYER.DIR` were on the host. A fresh browser profile with the same subject showed
Zelda with the same looks, her `.DTA` and `PLAYER.DIR` byte-identical, and the host's versions did not change at its
logon. In that profile "My own number" showed the zip's own `GUYBRUSH.DTA` and `PLAYER.DIR`, byte-identical to the
zip's, with no Zelda; "Someone new, please" made a second account whose Select Player was empty (only Create).

*Known limits.* Two personas whose first 8 characters match (`Abcdefgh1`, `Abcdefgh2`) share one file name; the
page does not try to tell them apart. The client's first-run welcome and Guide question come for every new account,
as in the original. A persona made while the server is unreachable stays in the browser until the next manifest. The
CasinoLand and SierraLand pairs follow the same views, but only the hub's was exercised.

## Legal pages and the age notice

`runtime/web/site.json` holds the operator's details. The repo ships it empty; each deployment fills in its own
(`docs/legal.md` is the checklist). Fields: `siteName`, `operatorName`, `contactEmail`, `privacyEmail` (optional,
falls back to `contactEmail`), `dmcaAgent` `{name, address, email, phone}`, `governingLaw`, `serverLocation`,
`effectiveDate`, `termsVersion` (an integer, starting at 1), `minimumAge` (18). Optional extras: `auditRetentionDays`
(shown in the privacy notice, default 90) and `authServer` (overrides `AUTH_SERVER`). `siteName` overrides
`SITE_NAME` in `page.js`; the title stays "Online world" when neither is set. The CMake copy list puts `site.json`
and `legal/` next to `inn.html`.

`legal/terms.html`, `privacy.html` and `copyright.html` are standalone documents. `legal/legal.js` fills their
placeholders from `../site.json`; a missing value shows a highlighted `[operator: set <field> in site.json]` and a
banner says the deployment has not filled in its operator details. Each page says it is not legal advice and should be
reviewed by a lawyer before a public launch.

Where the links are: the Help menu (Terms of Use, Privacy, Copyright & takedown, each in a new tab with
`rel="noopener"`); Help, About (operator, contact, terms version and the links); a small always-visible
"Terms · Privacy · Copyright" plate in the bottom-right of the landing (the narrator and telephone boxes sit above
it; on a phone it spans the bottom), also under `?classic=1`; the game view's settings drawer under "Fine print";
and the age dialog. There is no cookie banner: nothing here uses cookies or tracking.

**The age and terms notice** (`site.js`). On first visit, and again whenever `termsVersion` rises, a centred dialog
("Before you go in...") sits over the dimmed scene, which keeps animating unless reduced motion is set. It says the
service is for adults (`minimumAge`, default 18), links the three documents, and has two equal buttons, "I am 18 or
older" and "I am not". Nothing is pre-selected and nothing is pre-ticked. While it is open nothing else works: the
rest of the page is `inert`, file drops, shortcuts (F10, Ctrl+Alt+F) and autostart are blocked, focus is trapped in
the dialog, Esc does not dismiss it, and it is `role="dialog"` with `aria-modal`. Accepting stores
`{age, terms, at}` (a date only) in `localStorage` under `inn.consent`; with no storage it asks every visit. "I am
not" shows a closing screen, remembered in `sessionStorage` for that tab's session (a reload keeps it; a new tab
asks again). With sign-in on, the login URL carries `attest=<minimumAge>+v<termsVersion>`.

**This is self-declaration, not verification.** The page does not check anyone's age. The notice and the
server-side record exist so the operator can show that adults were asked and agreed to a given version of the terms.

## Developing against the mock

```
python3 tools/mock_auth.py --port 2843 --page-origin http://127.0.0.1:8080 --accounts 100001:Guybrush --enrol open
# page: http://127.0.0.1:8080/inn.html?auth=http://127.0.0.1:2843&com1=ws://127.0.0.1:2316
node tools/test_auth.js          # encoder, LSCI.CFG edit, the null persona and the empty account call
node tools/test_saves.js         # the persona views, the prefetch and the PLAYER.DIR rule
```

Flags for the error paths: `--attest-required` makes `POST /auth/accounts` answer 403 `attest_required` until the
session has posted `/auth/attest` (the session JSON still reports the attestation as current), and
`--accounts-per-day N` gives the 429 `rate_limited` and a false `canCreate`. The mock's consent page also has
"Provider error" and "Server error" buttons for the callback codes. To run the page against the real server
instead, `innkeeperd dev-oidc` is a mock provider and `--insecure-dev-keys` lets a loopback run work without
Azure (innkeeperd.md 8.5).

`tools/mock_auth.py` is for local development and tests only, never production: Python standard library, binds
127.0.0.1, in memory, no real authentication (its consent page has Allow and Deny buttons), and it prints the
passwords it issues. It serves two fake providers ("Example Phone Co." and "Test Telegraph"), CORS for
`--page-origin`, `--accounts [provider:]ID:Persona,...` (default `100001:Guybrush`, owned by the `example`
identity) and `--enrol open|closed`. `?mode=redirect` on the login URL exercises the fallback. It follows the PII-free contract above:
no names anywhere, `identity.handle` is derived from an HMAC of a per-run secret, `attest` is stored per identity and
returned as `attested`, and `POST /auth/attest` records a new answer. It does not touch the WebSocket.

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

## Packaging and deployment

`tools/package_web.sh <build-dir> <out-dir>` turns a finished public build into the folder that goes to Azure
Static Web Apps. The dev workflow does not change: `cmake --build` still copies the page files unminified, and
nothing below is needed to run or test locally.

```
emcmake cmake -S . -B build-web -G Ninja && cmake --build build-web
tools/package_web.sh build-web dist --game wss://play.example.net:2316 --auth https://play.example.net
# or: cmake --build build-web --target package-web   (writes build-web/package; -DINN_PACKAGE_ARGS="--game;wss://...;--auth;https://...")
tools/test_package.sh build-web      # the guards and the output checks
```

### The tool: esbuild, one binary

The page is classic scripts that share globals, with no `import`/`export` and no npm dependencies, and the browser
loads no provider SDK (sign-in is redirect based; the provider libraries are Rust crates in innkeeperd). So the job
is "minify, concatenate, hash", not "bundle a module graph". esbuild does that in one MIT-licensed binary, in
milliseconds, and brings no `node_modules`. The script downloads `@esbuild/linux-x64` 0.28.2 (released 2026-08-08)
into the gitignored `.cache/esbuild-0.28.2/`, checks its SHA-256 (pinned in the script) and refuses on a mismatch.
Webpack, Vite or Rollup would add a dependency tree and a config for no gain here. Node (already needed for the
headless tests) runs the small driver `tools/package_web.mjs`: hashing, HTML rewriting, the config file.

What the packager does:

- The eight page scripts (`site zip saves auth music adv scene page`) become one `assets/app.<hash>.js`, in the
  order the shell lists them (classic scripts share one global scope, so one concatenated script behaves as the
  eight did). The inline `<style>` becomes `assets/app.<hash>.css`. Inline scripts stay inline, minified, and their
  hashes go into the CSP.
- `inn.js` and `inn.wasm` get hashed names (`inn.js` names the wasm exactly once, and the packager rewrites it).
  Emscripten's output is already minified, so it is not minified again.
- `scenes/*.js` and `music/*.js` get hashed names. The page finds them through
  `<script type="application/json" id="inn-assets">`, which `innAsset()` in `site.js` reads; a dev build has no such list and the
  names are used as written. The three legal pages keep their paths (people link to them); their script and
  style are hashed. `site.json` is not hashed, because operators edit it.

### What is lazy, and what is not

| | Loaded | Why |
|---|---|---|
| the scene shown (`scenes/<id>.js`, 14-17 KB min) | when that scene is shown; the others only when picked from the Scene menu | already lazy before packaging; saves 28 KB gzip against loading all five |
| its song (`music/<id>.js`, about 1 KB) | with the scene | same |
| the synth and the audio context | on the first gesture (the age answer counts) | `ensureCtx()` builds them then; the code is 6 KB gzip inside `app.js` |
| legal pages | when opened | separate pages |
| `auth.js` | **with the page, not deferred** | 3.1 KB gzip minified. A deployment with sign-in needs it for the first `/auth/providers` answer anyway, `adv.js` and `page.js` call `InnAuth` synchronously in about forty places, and the saved request would cost as much as it saves. Without sign-in the page makes no `/auth/providers` request at all (`?auth=` empty). |
| `adv.js`, `music.js` (6 and 11 KB gzip) | with the page | the landing screen is the adventure and its menu bar; they are the first paint, and `adv.js` calls `InnMusic` while building |
| `inn.wasm` (338 KB gzip) | with the page, in parallel with the landing art | a visitor who never adds disks does fetch it; deferring it until Connect would put its download (and the 64 MB memory setup) on the Connect click. The right gate is "after the disks are picked", which needs a loader change in `page.js`; listed as an open item. |

Measured on 2026-10-16, landing with the town scene, bytes on the wire (level 9 gzip, default brotli):

| | raw before | gzip before | brotli before | raw after | gzip after | brotli after |
|---|---:|---:|---:|---:|---:|---:|
| `inn.html` | 31,949 | 8,546 | 7,483 | 8,202 | 2,969 | 2,494 |
| page scripts (after: `app.js` + `app.css`) | 192,540 | 64,961 | 56,508 | 133,747 | 47,624 | 41,444 |
| `inn.js` | 192,142 | 46,805 | 40,349 | 192,153 | 46,815 | 40,405 |
| `site.json` | 269 | 170 | 130 | 269 | 170 | 130 |
| scene and song (town) | 26,793 | 8,793 | 7,532 | 17,548 | 6,934 | 5,959 |
| `inn.wasm` | 977,166 | 338,444 | 266,962 | 977,166 | 338,444 | 266,962 |
| **first load** | 1,420,859 | 467,719 | 378,964 | 1,329,085 | 442,956 | 357,394 |
| first paint (html, scripts, css) | 224,758 | 73,677 | 64,121 | 142,218 | 50,763 | 44,068 |
| requests | 14 | | | 8 | | |

With sign-in configured the page adds `GET /auth/providers` (and `/auth/session` once signed in): a few hundred
bytes either way, since `auth.js` is in `app.js` in both cases. The wasm is 76% of the first load; what packaging
can reach is the 25 KB gzip of everything else, and it takes the first-paint path from 74 KB to 51 KB gzip and
from 14 requests to 8.

### Output and headers

```
inn.html  site.json  staticwebapp.config.json
legal/{terms,privacy,copyright}.html
assets/{app,inn,legal}.<hash>.{js,css,wasm}   assets/scenes/<id>.<hash>.js   assets/music/<id>.<hash>.js
```

`staticwebapp.config.json` (SWA reads it from the root; its limit is 20 KB):

- **Caching.** `/assets/*`: `public, max-age=31536000, immutable` (every name is content-hashed, so a new
  build is a new URL). `/`, `/inn.html`, `/site.json`, `/legal/*`: `no-cache`. `/` is rewritten to `/inn.html`.
  There is no `navigationFallback`: the page has no client-side routes, so an unknown path is a plain 404.
- **MIME.** `.wasm` is `application/wasm`, which `WebAssembly.instantiateStreaming` requires (Emscripten uses
  it; the packaged run logged no fallback warning).
- **CSP**, in `globalHeaders`: `default-src 'none'`; `script-src 'self' 'wasm-unsafe-eval'` plus the hashes of
  the inline scripts (the theme snippet in `<head>` runs before first paint, so it stays inline);
  `style-src 'self' https://fonts.googleapis.com` and `font-src https://fonts.gstatic.com` (VT323);
  `style-src-attr 'unsafe-inline'` (a few `style=` attributes and the page sets inline styles; no inline `<style>`
  remains); `img-src 'self' data: blob:`; `media-src 'self' blob:`; `base-uri 'none'`; `form-action 'none'`;
  `object-src 'none'`; `frame-ancestors 'none'`. No `unsafe-eval`.
- **`connect-src`** is the part the operator sets: `'self'`, one `--game wss://host:port` per game server (the
  page's default server is the page's own host on the WebSocket port; a custom address typed under Advanced is
  blocked unless its origin is listed), one `--auth https://host` per sign-in server (`authServer` in `site.json`
  is added automatically), and `--connect-any` for `ws: wss: https:` if you want visitors to type any server.
  The packager warns when `--game` is missing. To change it later, edit the `Content-Security-Policy` string in
  `staticwebapp.config.json` and redeploy.
- **Other.** `Referrer-Policy: no-referrer`, `X-Content-Type-Options: nosniff`, a `Permissions-Policy` that turns
  off camera, microphone, geolocation, payment, USB, serial and HID.
- **`Cross-Origin-Opener-Policy: same-origin-allow-popups`.** The sign-in popup posts its result to
  `window.opener`. Tested with the dev provider: this value signs in, plain `same-origin` severs the opener (the
  popup stays on `/auth/callback` and the page never gets the session). A provider whose own login page sends `Cross-Origin-Opener-Policy: same-origin` can make the browser
  drop the opener when the popup navigates there (untested; the dev provider sends none). The page already falls
  back to a full-page redirect when the popup is blocked, and an operator who hits this can leave COOP out.
- **Compression.** SWA compresses text responses itself and does not serve `.br` or `.gz` files next to the
  originals, so the packager writes none. (Whether it compresses `application/wasm` is not stated in its
  documentation; check `Content-Encoding` on `inn.<hash>.wasm` after the first deploy. If it does not, a CDN in
  front or Front Door compression would save 640 KB on that file.)

### Deploying to Static Web Apps

1. Build the public hybrid build (no `INN_NATIVE`, no `private/lifted`, no `INN_WEB_PRELOAD_DIR`), then run
   `tools/package_web.sh` with the operator's origins.
2. Fill in `site.json` in the output (operator name, contact addresses, governing law, effective date, and
   `authServer` if the sign-in server has its own origin); the legal pages say what is missing.
3. Deploy the folder as the app content with no build step, for example
   `swa deploy ./dist --deployment-token <token>` or the GitHub Action with `app_location: dist`,
   `skip_app_build: true`. Nothing here contacts Azure; the deployment is the operator's step.
4. The game server and the sign-in server must list the page's origin: `innkeeperd --page-origin https://<site>`.

### The IP guard

This repository is public. Only the hybrid build (the original program on the emulator, no Sierra-derived code
compiled in) may be packaged. `package_web.sh` reads the build's `CMakeCache.txt` and refuses when `INN_NATIVE` or
`INN_NATIVE_HW` is ON, when the `INN_LIFTED_DIR` directory exists, whatever its path (CMake compiles lifted code only from
an existing directory; on a machine that holds the private tree, configure the build to package with
`-DINN_LIFTED_DIR=/nonexistent`), when `lifted_table.c` is more than the two-line stub or
`lifted_entries.h` exists, when `INN_WEB_PRELOAD_DIR` is set, and when the build directory holds `inn.data`,
`*.exe`, `*.drv`, `*.dll`, `resource.*`, `*.zip` or `*.img`. The packager copies only the files it names, refuses an
unknown file among the scenes or songs, checks every output path against an allow list (and that none starts with
`MZ` or `PK`), and writes no game data: the visitor's install is read in the browser at runtime. `tools/test_package.sh`
covers each refusal.

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
