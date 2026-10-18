# CRT shader by Timothy Lottes

- File: `crt-lottes.glsl` (scanlines, shadow mask, bloom, slight curvature).
- Origin: https://github.com/libretro/glsl-shaders (commit ce9a41242e27f41e16bc35c28943466cee3e8ff8, 2026-10-17), `crt/shaders/`.
- Licence: public domain (see `LICENSE`).

The loader in runtime/web/filters.js supplies what these libretro-style files expect: it removes the `#version` line, prepends `#version 300 es` with `#define VERTEX` or `#define FRAGMENT` and `#define PARAMETER_UNIFORM`, turns every `mediump` into `highp` and comments out `#pragma parameter` lines (their defaults become uniforms). That is the only porting to GLSL ES 3.00 / WebGL2; unless listed under "Changes", the file is byte-for-byte as published.

## Changes
None.
