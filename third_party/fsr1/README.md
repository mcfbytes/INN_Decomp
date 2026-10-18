# FidelityFX Super Resolution 1 (AMD): EASU and RCAS

- Files: `fsr-pass0.glsl` (EASU, edge-adaptive upscale) and `fsr-pass1.glsl` (RCAS, sharpen).
- Origin: https://github.com/libretro/glsl-shaders (commit ce9a41242e27f41e16bc35c28943466cee3e8ff8, 2026-10-17), `fsr/shaders/`, which is a port of the Shadertoy https://www.shadertoy.com/view/stXSWB of AMD's `ffx_fsr1.h` (https://github.com/GPUOpen-Effects/FidelityFX-FSR, commit a21ffb8f6c13233ba336352bdff293894c706575). The comments in the shader are AMD's.
- Licence: MIT, Copyright (c) 2021 Advanced Micro Devices, Inc. (`LICENSE` is AMD's `license.txt`).

The loader in runtime/web/filters.js supplies what these libretro-style files expect: it removes the `#version` line, prepends `#version 300 es` with `#define VERTEX` or `#define FRAGMENT` and `#define PARAMETER_UNIFORM`, turns every `mediump` into `highp` and comments out `#pragma parameter` lines (their defaults become uniforms). That is the only porting to GLSL ES 3.00 / WebGL2; unless listed under "Changes", the file is byte-for-byte as published.

## Changes
- `fsr-pass0.glsl`: `fragCoord` is `vTexCoord * OutputSize - 0.5`. The published port passed the pixel centre where AMD's code expects the integer pixel position, which moved the picture by half an output pixel (measured with tools/filter_test: 0.15 source pixels at 3x).
