# 2xSaI, Super 2xSaI, Super Eagle

- Files: `supereagle.glsl` (as published), `2xsai.glsl` and `super-2xsai.glsl` (ported by this project), and `2xsai.slang`, `super-2xsai.slang` (the published sources of the port, not shipped).
- Origin: `supereagle.glsl` from https://github.com/libretro/glsl-shaders (commit ce9a41242e27f41e16bc35c28943466cee3e8ff8, 2026-10-17), `eagle/shaders/`. The two .slang files are from https://github.com/libretro/slang-shaders (commit e1d75632a205c70f14a4cc947c46d5abb7b3f7f1), `edge-smoothing/eagle/shaders/`; the glsl-shaders repository has no 2xSaI.
- Licence: see `LICENSE`. Route taken: the existing guest(r) shaders, whose headers say "Copied from the Dosbox source code ... License: GNU-GPL"; DOSBox is GPL-2.0-or-later, and an unversioned GPL permits any version, so they are used under GPL-3.0-or-later. We did not write a clean-room version. The remaining uncertainty is only the chain of title before DOSBox (the original 2xSaI by Derek Liauw Kie Fa was first offered for non-commercial use; DOSBox and the shader headers record it as GPL).

The loader in runtime/web/filters.js supplies what these libretro-style files expect: it removes the `#version` line, prepends `#version 300 es` with `#define VERTEX` or `#define FRAGMENT` and `#define PARAMETER_UNIFORM`, turns every `mediump` into `highp` and comments out `#pragma parameter` lines (their defaults become uniforms). That is the only porting to GLSL ES 3.00 / WebGL2; unless listed under "Changes", the file is byte-for-byte as published.

## Changes
- `2xsai.glsl`, `super-2xsai.glsl`: the Vulkan-GLSL `.slang` sources rewritten in the libretro `#if defined(VERTEX)` convention: push constants and the UBO replaced by `TextureSize` and `MVPMatrix`, `Source` by `Texture`, `0.0.xxx` by `vec3(0.0)`. The filter code is unchanged.
- The three filters move the picture by a fraction of a source pixel (2xSaI by -0.25, -0.25; Super 2xSaI by -0.25, -0.5; Super Eagle by -0.5, -0.5, measured with tools/filter_test, as the libretro "fix-pixel-shift" presets do). filters.js undoes that in the resample after them; the shaders themselves are not touched.
