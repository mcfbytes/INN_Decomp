# HQx (Maxim Stepin's HQ2x, HQ3x and HQ4x)

- Files: `hqx-pass1.glsl`, `hqx-pass2.glsl`, `hq2x.png`, `hq3x.png`, `hq4x.png` (lookup tables).
- Origin: https://github.com/libretro/glsl-shaders (commit ce9a41242e27f41e16bc35c28943466cee3e8ff8, 2026-10-17), `hqx/shader-files/` and `hqx/resources/`.
- Licence: GNU LGPL 2.1 or later (Copyright (C) 2003 Maxim Stepin, 2010 Cameron Zemek, 2014 Jules Blok); see `LICENSE`. LGPL-2.1-or-later code may be combined into a GPL-3.0-or-later work. The files are separate and replaceable here.
- The page picks the 2x, 3x or 4x tables from the window size.

The loader in runtime/web/filters.js supplies what these libretro-style files expect: it removes the `#version` line, prepends `#version 300 es` with `#define VERTEX` or `#define FRAGMENT` and `#define PARAMETER_UNIFORM`, turns every `mediump` into `highp` and comments out `#pragma parameter` lines (their defaults become uniforms). That is the only porting to GLSL ES 3.00 / WebGL2; unless listed under "Changes", the file is byte-for-byte as published.

## Changes
None.
