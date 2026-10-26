# Third-party code

Vendored code lives untouched under `third_party/<name>/` with its license file. Local behaviour
that the upstream code lacks goes in our own files, never in patches to the vendored copy.

| Component | Path | Version | License | Upstream | Used by |
|---|---|---|---|---|---|
| Nuked-OPL3 | `third_party/nuked-opl3/` (`opl3.c`, `opl3.h`, `LICENSE`) | 1.8, commit `765ec962e473aeb767e4cba74ffdc8f588ffbfe8` (2026-08-24), fetched 2026-10-14 | LGPL-2.1-or-later | <https://github.com/nukeykt/Nuked-OPL3> | `runtime/src/opl.c` (OPL2 synthesis) |

## Nuked-OPL3

A cycle-accurate YMF262 (OPL3) emulator by Nuke.YKT, derived from die shots of the chip. With
the OPL3 `NEW` bit clear it behaves as the OPL2-compatible part, which is how DOSBox and others
emulate an AdLib with it. `opl.c` adds what the core does not model: the OPL2 status register and
timers, the waveform-select enable bit (register 01h bit 5), and resampling from 49716 Hz.

LGPL-2.1-or-later is compatible with distributing the combined work under GPL-3.0-or-later
(LGPL 2.1 section 3 allows relicensing the copy under the GPL, and "or later" reaches GPLv3);
the project's own license is not decided here.

SHA-256 of the vendored files:

| File | SHA-256 |
|---|---|
| `opl3.c` | `59eb873fdb6d52bc7977a0fcbb97c1bae03dd71cc88e3acddbcc01b7121bfe03` |
| `opl3.h` | `a84266b8d71a4929f15f573afbe407fd24310b77757d855835dacabf68763679` |
| `LICENSE` | `20c17d8b8c48a600800dfd14f95d5cb9ff47066a9641ddeab48dc54aec96e331` |

## VT323 (web page font)

The browser page (`runtime/web/shell.html`) and the legal pages use VT323 by Peter Hull for headings, the modem
panel and the control strip. It is vendored in `third_party/vt323/` (`VT323-Regular.ttf`, unmodified) and served
from the page's own origin, so visitors' addresses never reach a font host; the stylesheet falls back to the system
monospace font if the file cannot load. License: SIL Open Font License 1.1 (`OFL.txt`; the copyright line is
"Copyright 2011, The VT323 Project Authors"; no Reserved Font Name is declared). The license travels with the file;
the packaged site ships it as `assets/vt323.<hash>.ttf`.

## Sign-in provider marks (web page)

The sign-in buttons (`runtime/web/signin.js`) show each provider's own mark, as their brand guidelines require for a
"Sign in with ..." button. The marks are trademarks, used under each owner's guidelines for exactly that purpose, not
under a copyright license of ours. They are embedded in `signin.js` as SVG text (served as `data:` images), unmodified
apart from what the table says. Fetched 2026-10-26.

| Mark | Source | What was taken | SHA-256 of the download |
|---|---|---|---|
| Google "G" (gradient) | Google's sign-in branding assets, <https://developers.google.com/static/identity/images/signin-assets.zip> | `Android + Web/SVG/Light/Theme=Light, Show text=No, Shape=Square, Platform=Android+Web.svg` with the button's own fill and stroke removed and the viewBox framed on the G's 20 x 20 box; Figma metadata attributes dropped | `ba884069e12093b06bcfd776915081254a7c95094b80d50be2c5dc6bac1c1da1` (the zip) |
| Microsoft logo | "Sign in with Microsoft branding guidelines", <https://learn.microsoft.com/en-us/entra/identity-platform/media/howto-add-branding-in-apps/ms-symbollockup_mssymbol_19.svg> | the file; its `<title>` dropped | `929f48f88c8ca7f3f5d294be47ec4caf51acc28ac25340c19a903125d7ecd84a` |
| Discord symbol, Blurple | Discord's brand page, <https://discord.com/branding> (`66e3d80db9971f10a9757c99_Symbol.svg`) | the file (light theme) | `296286aa112c4400af8e96191ab888f81abd4bd1d5dc7294112a622ef43581b1` |
| Discord symbol, white | the same page (`66e3d7f4ef6498ac018f2c55_Symbol.svg`) | the file (dark theme) | `eb3bf5a2e8b9f9aa39c1b1d38aeaa7c2997084567634c55f4141aa39cf426cec` |
| GitHub mark | `@primer/octicons` 19.15.1 from npm, `build/svg/mark-github-24.svg` (MIT, Copyright (c) 2025 GitHub Inc.; the mark itself is a GitHub trademark under <https://github.com/logos>) | the path, filled black (light theme) or white (dark theme), the two colours GitHub's logo guidelines allow | `95377512a8b62c70f9934a1d65e660f522dc762c6b819b54376bab2dd6eb0689` (the tarball) |
