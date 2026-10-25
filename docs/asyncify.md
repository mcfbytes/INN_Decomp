# The native web client without JSPI

The native web build (`emcmake -DINN_NATIVE=ON`, [native.md](native.md) sections 4 and 10) suspends its
whole C stack with JSPI and links only with `INN_WEB_SUSPEND=JSPI`. This note decides how it runs in a
browser without JSPI (Safari, a Firefox without it, Chromium with it off). Measured 2026-10-25, emsdk 6.0.12,
Node 24.15 (V8 13.6, JSPI off unless `--experimental-wasm-jspi`), Chromium 153 (Playwright build 1243).

## Decision

**(A) Full-instrumentation Asyncify, linked as a second variant from the same objects. The page picks the
variant with `typeof WebAssembly.Suspending`.**

- Asyncify is a link-time pass, so one compile serves both: `inn.js`/`inn.wasm` (JSPI) and
  `inn-asyncify.js`/`inn-asyncify.wasm`. The objects stay compiled with `-fwasm-exceptions
  -sSUPPORT_LONGJMP=wasm`. In emsdk 6.0.12 Asyncify and the **legacy** wasm EH encoding work together. emcc
  warns `ASYNCIFY=1 is not compatible with -fwasm-exceptions`, but Binaryen instruments `try` bodies. The
  unsupported case is an unwind that starts inside a `catch` body, and the C code's only catch bodies are
  setjmp landing pads.
  The standard (exnref) encoding crashes `wasm-opt --asyncify`, so the build pins `-sWASM_LEGACY_EXCEPTIONS=1`.
- Emscripten's JS longjmp (`SUPPORT_LONGJMP=emscripten`, `invoke_*` treated as async imports) also works,
  but it costs the same and needs a second compile of every object, so it is not used.
- JSPI stays the first choice. Asyncify costs 38 to 42 % more instructions in V8, and its wasm is 25 % larger.
- **(B) Falling back to the hybrid build is rejected.** It would hand non-JSPI browsers a different client
  (the emulator with lifted code), not the native one. The hybrid web build with lifted code is broken today:
  it traps (`unreachable` during Asyncify rewind) when a driver starts. The direct calls of package 2a
  (`call_lifted_direct`, `call_direct`, `guest_call_near`/`far`) reach a sleep through function pointers that
  `ASYNCIFY_IGNORE_INDIRECT` does not follow. Fixing it means full Asyncify anyway: relinked without
  `IGNORE_INDIRECT` and the add list, it reaches the town map. The deployed hybrid build has no lifted code
  and is unaffected: it reaches the map with JSPI removed.
- **(C) A Worker with `SharedArrayBuffer` and `Atomics.wait` is rejected.** It needs no instrumentation, but:
  - Cross-origin isolation needs COOP `same-origin` plus COEP. The deployed page sends
    `same-origin-allow-popups` because plain `same-origin` breaks the sign-in popup ([web.md](web.md),
    "Output and headers"), and the dev server would need custom headers.
  - Every object would be rebuilt with `-pthread`, and memory growth with pthreads slows JS heap access
    (emcc warns).
  - SDL2 would run off the main thread (`PROXY_TO_PTHREAD`, `OFFSCREENCANVAS_SUPPORT`).
  - The 12 `EM_JS`/`EM_ASM` sites (the AudioWorklet post, page callbacks, the WebSocket control channel, the
    filter upload of every frame) would be proxied to the main thread.
  - The C: drive's file I/O would be proxied, or moved to WasmFS.
  - `emscripten_sleep` and `browser_turn` would be replaced by `emscripten_thread_sleep` plus queued-call
    processing.

  The objects do link that way (2,693,862 bytes), before any of the plumbing is written. With Asyncify
  already at real speed (below), that much plumbing buys nothing.

## Measurements

The prototype under Node needs one frontend change. A headless run has no platform, so it never returns to
the event loop and WebSocket data never arrives. The prototype gives Emscripten headless runs a platform
whose `sleep_us` is the existing `browser_turn`/`emscripten_sleep` and whose `poll_events` applies the input
script, and it sets `INN_NATIVE_DETERMINISTIC=1`. The game loop under `run_child` yields every
`HEADLESS_CHUNK_US`. Instructions (`perf stat -e instructions:u`) are the load-independent figure. During
the runs other agents kept the 32-thread host at a load average of 30 to 140, so wall times are indicative.

| Build (same objects unless noted) | wasm bytes (gzip -9) | Link (3 runs, load ~113) | Logon to map, 65 s emulated: instructions / wall | Yserbius round trip, 150 s: instructions / wall |
|---|---|---|---|---|
| x86-64 native, gcc 15 `-O2` (reference) | n/a | n/a | 117.3 G / 6.0-11.8 s | 327.8 G / 38.9 s |
| wasm JSPI (Node with `--experimental-wasm-jspi`) | 2,682,753 (618,955) | 100-102 G insn, 17.9-22.5 s | 201.4 G / 18.2-34.8 s | 557.6 G / 105.9 s |
| wasm Asyncify, wasm SjLj (**chosen**) | 3,353,375 (790,035) | 193-194 G insn, 27.3-27.9 s | 277.0-277.3 G (+38 %) / 27.1-47.5 s | 790.3 G (+42 %) / 128.9 s |
| wasm Asyncify, JS longjmp (objects rebuilt) | 3,337,419 | | 276.7-276.9 G / 18.9-34.2 s | 791.7 G / 129.7 s |
| wasm Asyncify, wasm SjLj, `ASYNCIFY_REMOVE=['cpu_run']` | 3,349,168 | | 277.0 G | 713.4 G (+28 %) / 79.4 s |
| JSPI build under Node without the flag | | | `TypeError: WebAssembly.Suspending is not a constructor` | |

The sizes of the JS longjmp and `ASYNCIFY_REMOVE` rows come from the Node link, which differs from the web
link by less than 0.1 %. Link time follows the instruction count: Asyncify roughly doubles the work of
the link step (Binaryen's `--asyncify` pass at `-O3`).

Correctness and limits:

- Both Asyncify variants reach the map in every run. The two Yserbius screenshots (gallery, back on the map)
  are pixel-identical to the x86 run for JSPI and for every Asyncify variant.
- `test_native` passes under Node 24 without the flag, linked with Asyncify in either longjmp mode. That test
  sleeps 100 times inside a setjmp scope, longjmps out, then makes a paced platform wait.
- `ASYNCIFY_ADVISE` instruments 1,727 of the 3,079 functions.
- `ASYNCIFY_STACK_SIZE=16384` already completes the Yserbius round trip; 262144 is kept.
- With exnref (`-sWASM_LEGACY_EXCEPTIONS=0`), the link fails in Binaryen's Flatten pass with `unexpected expr
  type`.

**Browser.** Headless Chromium 153 has no switch that turns JSPI off: `--js-flags=--no-experimental-wasm-jspi`
and `--disable-features=WebAssemblyJSPromiseIntegration` both leave `WebAssembly.Suspending` defined. The
non-JSPI runs therefore delete `WebAssembly.Suspending` and `WebAssembly.promising` in an init script. With
that, the JSPI build stops with the same `TypeError`. Each run is a 90 s session: Play at 11 s, then the
`perf:` windows after the first 20 s. Only the round at load 35 to 48 separates the builds. In the other
rounds (load 55 to 87) every build, JSPI included, fell to speed 0.34 to 0.85.

| Build in Chromium | speed min | host busy mean / max | longest slice | underruns | town map |
|---|---|---|---|---|---|
| native JSPI | 0.998 | 43.9 % / 59.8 % | 25 ms | 0 | yes |
| native Asyncify, JSPI removed | 0.983 | 38.7 % / 57.8 % | 17 ms | 1 | yes |
| hybrid with lifted code, full Asyncify, JSPI removed | 1.000 | 57.3 % / 67.5 % | 48 ms | 0 | yes |
| hybrid with lifted code, as built today (`IGNORE_INDIRECT`) | | | | | traps at driver start |
| hybrid without lifted code (deployed), JSPI removed | 1.000 | 20.3 % / 62.7 % | 19 ms | 0 | yes |

## Build flags

The flags for `inn-asyncify-impl`. The JSPI link is unchanged.

```
# compile and link, every INN_NATIVE web build
-fwasm-exceptions -sSUPPORT_LONGJMP=wasm -sWASM_LEGACY_EXCEPTIONS=1
# inn (JSPI), as today
-sJSPI
# inn-asyncify: same objects, a second executable target with SUFFIX ".js" (no html)
-sASYNCIFY -sASYNCIFY_STACK_SIZE=262144
# (no ASYNCIFY_IGNORE_INDIRECT, no ASYNCIFY_ADD; the rest as inn: -sSTACK_SIZE=1048576 -sUSE_SDL=2
#  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 -sFORCE_FILESYSTEM=1
#  -sENVIRONMENT=web -sEXPORTED_RUNTIME_METHODS=callMain,FS -lidbfs.js -lwebsocket.js
#  --pre-js runtime/web/native-pre.js)
```

CMake: `INN_WEB_SUSPEND` gains `BOTH`, the default for `INN_NATIVE`, which links `inn` with JSPI and
`inn-asyncify` with Asyncify. `ASYNCIFY` with `INN_NATIVE` links only the Asyncify variant, as `inn`. The
`FATAL_ERROR` stays only for unknown values. `test_native` is linked twice: `test_native` runs under
`node --experimental-wasm-jspi` and `test_native_asyncify` runs under plain `node`. The comment above the
`FATAL_ERROR` and the `-sJSPI`-only `test_native` comment change with it. `ASYNCIFY_REMOVE=['cpu_run']`
(+28 % instead of +42 % over JSPI while a game runs on the emulator) is an optional second step. It is safe only once the
assertion run below passes, because an unwind through an uninstrumented function corrupts the stack without
a trap.

## Runtime changes

- **Sleep primitive: none.** `emscripten_sleep` and the `EM_ASYNC_JS` `browser_turn` in `frontend_sdl.c`
  work under both. Only comments that say "JSPI" change (`host_platform.h`, `frontend_sdl.c`).
- **setjmp/longjmp: none.** `native_exec`'s terminate frames and the native LSCITV main's Restart frame keep
  `setjmp`/`longjmp` on wasm EH. The rule to keep: never reach a sleep from inside a wasm `catch` body. In
  this C code the only catch bodies are the setjmp landing pads, which branch out before the program
  continues.
- **host_platform: none.** The headless Node platform above is a test tool, kept for the verify plan below.
- **Page.** `shell.html` gets `data-asyncify="@INN_WEB_ASYNCIFY_JS@"` (empty unless the build has the
  variant). `wantRuntime` in `page.js` loads `innAsset(dataset.asyncify)` instead of `innAsset('inn.js')`
  when the variant exists and either `typeof WebAssembly.Suspending !== 'function'` or `?suspend=asyncify` is
  given. The query parameter exists because Chromium cannot switch JSPI off. Each Emscripten script finds its
  own `.wasm` by name. `package_web` keeps refusing native builds, so the deployed page is unchanged.

## Verify plan for inn-asyncify-impl

1. Configure all four web builds (hybrid; hybrid JSPI; native `BOTH`; native `ASYNCIFY`) and run ctest.
   `test_native` and `test_native_asyncify` pass, the latter under Node 24 without the flag. Add a case that
   sleeps inside two nested setjmp scopes (an EXEC frame and a Restart frame) and longjmps to each. No
   scenario runs Restart.
2. Node headless: run the 30 `M9-NATIVE.txt` scenarios with the Asyncify variant through the Node platform.
   Compare with the x86 native build using `tools/m9_compare.sh` rules (traffic, screens, files, EXEC
   sequence). Expected failures are those listed under the Status section of native.md.
3. Run logon and `m9_ys_return` once with `-sASSERTIONS=1` on the Asyncify link. That adds Binaryen's
   `asyncify-asserts`, which trap when an unwind passes through uninstrumented code. Repeat with `ASYNCIFY_REMOVE=['cpu_run']` before
   adopting it.
4. Browser, Playwright and Chromium on an idle host (load < 16):
   - Run with `?suspend=asyncify`, and again with the JSPI API deleted by an init script. The network log
     shows `inn-asyncify.wasm` loaded.
   - Log on to the town map, then the Yserbius gallery and back. `perf:` speed is at least 0.99 after logon,
     with no `long frame` outside loading.
   - A plain run still loads `inn.wasm`.
5. The hybrid web build (no lifted code) and its package are unchanged: `tools/test_package.sh`.

Fix the broken hybrid-with-lifted-code web build in its own package. Either drop `ASYNCIFY_IGNORE_INDIRECT` and
the add list when `INN_LIFTED_DIR` exists, or add the direct-call functions to the list.

## Reproducing

The prototype scripts used for these numbers are in `/mnt/source/scratch/lift/wt/inn-asyncify-design/proto/`
(scratch, not committed):

| Script | What it does |
|---|---|
| `link.sh` | Relinks the `inn` objects of a configured native web build with other flags. |
| `rebuild_objs.py` | Recompiles every object from `compile_commands.json` with swapped flags. |
| `frontend_node.c`, `cc_front.sh` | The headless Node platform. |
| `run.sh` | Runs innkeeperd (TCP or `--ws-bind`), then the client under `perf stat` and `/usr/bin/time`. |
| `browser.mjs`, `browser.sh` | The Playwright runs. |
| `jspiflag.mjs` | The Chromium switch check. |

```
W=/mnt/source/scratch/lift/wt/inn-asyncify-design; source /mnt/source/tools/emsdk/emsdk_env.sh
emcmake cmake -S $W/public -B $W/build-jspi -G Ninja -DINN_WEB_SUSPEND=JSPI -DINN_NATIVE=ON \
  -DINN_LIFTED_DIR=<private>/lifted && ninja -C $W/build-jspi -j6        # JSPI size; ctest
cmake -S $W/public -B $W/build-native -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DINN_NATIVE=ON \
  -DINN_LIFTED_DIR=<private>/lifted && ninja -C $W/build-native -j6      # x86 reference
proto/cc_front.sh x proto/obj/front-wasmeh.o -fwasm-exceptions -sSUPPORT_LONGJMP=wasm
FRONTOBJ=proto/obj/front-wasmeh.o proto/link.sh build-jspi v/jspi-node/inn.js node \
  -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -sJSPI                        # run: node --experimental-wasm-jspi
FRONTOBJ=proto/obj/front-wasmeh.o proto/link.sh build-jspi v/asy-wasmeh-node/inn.js node \
  -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -sASYNCIFY -sASYNCIFY_STACK_SIZE=262144 [-sASYNCIFY_ADVISE] \
  ["-sASYNCIFY_REMOVE=['cpu_run']"]
python3 -I proto/rebuild_objs.py build-jspi objs-emjs '-fwasm-exceptions -sSUPPORT_LONGJMP=wasm' \
  '-sSUPPORT_LONGJMP=emscripten' && ninja -C objs-emjs -j6               # JS longjmp variant (also -pthread, exnref)
INN_NATIVE_DETERMINISTIC=1 proto/run.sh 39000 ws <work>/inn-c proto/logon.txt 65000 v/<variant>/run
INN_NATIVE_DETERMINISTIC=1 proto/run.sh 39000 ws <work>/yserbius-c \
  <private>/lifted/scenarios/m9_ys_return.txt 150000 v/<variant>/run     # x86: tcp and build-native/inn
perf stat -e instructions:u proto/link.sh build-jspi v/linkbench/{jspi,asy}.html web <flags>   # link rows
proto/link.sh build-jspi v/asy-web/inn.html web -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -sASYNCIFY \
  -sASYNCIFY_STACK_SIZE=262144                                           # into a copy of build-jspi's page files
proto/browser.sh <name> <served dir> 3900x 39000 --secs=90 [--nojspi]   # browser rows
gzip -9c <wasm> | wc -c
```

`logon.txt` is `11000 click 285 59` and `60000 shot @OUT@/map.ppm`. The Node `run` wrappers are
`node --stack-size=8000 inn.js "$@"`, with `--experimental-wasm-jspi` for the JSPI variant.
