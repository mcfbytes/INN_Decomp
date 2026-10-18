# ScaleFX (Sp00kyFox)

- Files: `scalefx-pass0.glsl` to `scalefx-pass4.glsl` (the multi-pass version; pass 4 scales by 3, a plain resample then fits the result to the window).
- Origin: https://github.com/libretro/glsl-shaders (commit ce9a41242e27f41e16bc35c28943466cee3e8ff8, 2026-10-17), `scalefx/shaders/`.
- Licence: MIT, Copyright (c) 2016 Sp00kyFox (see `LICENSE`; it is in each pass's header too).
- Passes 0 and 1 draw into 32-bit float targets (the preset's `float_framebuffer`), which needs EXT_color_buffer_float; without it the choice is greyed out.

The loader in runtime/web/filters.js supplies what these libretro-style files expect: it removes the `#version` line, prepends `#version 300 es` with `#define VERTEX` or `#define FRAGMENT` and `#define PARAMETER_UNIFORM`, turns every `mediump` into `highp` and comments out `#pragma parameter` lines (their defaults become uniforms). That is the only porting to GLSL ES 3.00 / WebGL2; unless listed under "Changes", the file is byte-for-byte as published.

## Changes
None.
