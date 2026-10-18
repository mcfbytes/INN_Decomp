# xBR (Hyllian)

- Files: `xbr-lv2.glsl`, `xbr-lv3.glsl` (the "xBR, strong" choice is level 3).
- Origin: https://github.com/libretro/glsl-shaders (commit ce9a41242e27f41e16bc35c28943466cee3e8ff8, 2026-10-17), `xbr/shaders/`.
- Licence: MIT, Copyright (C) 2011-2016 Hyllian (see `LICENSE`).
- Used by: runtime/web/filters.js, "Display filters" in docs/web.md.

The loader in runtime/web/filters.js supplies what these libretro-style files expect: it removes the `#version` line, prepends `#version 300 es` with `#define VERTEX` or `#define FRAGMENT` and `#define PARAMETER_UNIFORM`, turns every `mediump` into `highp` and comments out `#pragma parameter` lines (their defaults become uniforms). That is the only porting to GLSL ES 3.00 / WebGL2; unless listed under "Changes", the file is byte-for-byte as published.

## Changes
- `xbr-lv2.glsl`: the three global tables `delta`, `delta_l` and `delta_u` are now `const`. GLSL ES 3.00 only accepts constant expressions as global initialisers (desktop GLSL 1.30 was lenient).
