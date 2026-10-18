# Anime4K (bloc97)

- File: `Anime4K_Upscale_CNN_x2_S.glsl`: the small (S) 2x upscaling network, 5 passes in mpv's user-shader format.
- Origin: https://github.com/bloc97/Anime4K (commit 7684e9586f8dcc738af08a1cdceb024cc184f426), `glsl/Upscale/`.
- Licence: MIT, Copyright (c) 2019-2021 bloc97 (see `LICENSE`).
- The file is not changed. Its `//!` directives (HOOK, BIND, SAVE, WIDTH, HEIGHT) are read by a small loader in runtime/web/filters.js, which wraps each `hook()` in a fragment shader, defines `<name>_tex`, `<name>_texOff`, `<name>_pos`, `<name>_pt` and `<name>_size` for the bound textures, and runs the passes into RGBA16F targets (needs EXT_color_buffer_float or EXT_color_buffer_half_float; otherwise greyed out). The WHEN condition (apply only when the output is at least 1.2 times larger) is ignored: the filter always runs.
- Experimental: Anime4K is tuned for anime; on other pixel art it sharpens lines and leaves some ringing.

## Changes
None.
