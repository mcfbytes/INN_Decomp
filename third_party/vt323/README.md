# VT323 (Peter Hull)

- Files: `VT323-Regular.ttf`, `OFL.txt`.
- Origin: https://github.com/google/fonts, `ofl/vt323/` (last commit touching the directory: 2da41909d08a7bfeab32cfc163edb7be0c34395d, 2026-04-24), downloaded 2026-10-17 from raw.githubusercontent.com.
- Licence: SIL Open Font License 1.1, "Copyright 2011, The VT323 Project Authors (peter.hull@oikoi.com)" (see `OFL.txt`). The copyright line declares no Reserved Font Name, so even a modified or renamed copy would not be restricted on that count.
- Used by: the `@font-face` in `runtime/web/shell.html` and `runtime/web/legal/*.html`; `tools/package_web.mjs` ships it as `assets/vt323.<hash>.ttf` (CSP `font-src 'self'`). Self-hosted so visitors' addresses never reach Google Fonts.

## Changes
None. The TTF is byte-for-byte as published: no subsetting and no WOFF2 conversion (no fontTools, brotli or woff2 tool was available offline), so the family name stays "VT323".

SHA-256 of the vendored files:

| File | SHA-256 |
|---|---|
| `VT323-Regular.ttf` | `cf4de751ada78ceac033dbe16a687742939995b77bc2a052ae17a4957958594d` |
| `OFL.txt` | `27d9af34210253e7ca1251fbace86c6f65b40031d6ce1a75493a1b2093631298` |
