// Landing scenes: procedurally painted 320x200 backdrops in the look of early-90s hand-painted VGA
// adventure art (soft washes, paper texture, wobbly brush edges, a 256-colour palette with ordered
// dithering). Every scene is original artwork written as drawing code in runtime/web/scenes/<id>.js.
// `?scene=<id>` picks one (otherwise one is picked at random per visit), `?scenes` shows a small
// picker. A scene names two hotspot roles, `disks` (holds the installation) and `phone` (dials), and
// the rectangles where each one sparkles while it is the next step (adv.js drives the game around
// them). Query parameters are listed in docs/web.md.
'use strict';

const InnScenes = (() => {
  const W = 320, H = 200;
  const IDS = ['valley', 'diner', 'tower', 'harbor', 'town'];
  const CLASSIC = new URLSearchParams(location.search).has('classic');
  const FPS = 10;
  const DITHER = 20;          // ordered-dither spread, in 8-bit levels
  const HOVER_MS = 1000, QUIP_MS = 7000, TYPE_MS = 22;

  const registry = {};
  const state = { id: '', spark: { disks: false, phone: false }, scene: null, built: null, running: false, t0: 0, last: 0, raf: 0 };
  let els = null;

  // ---------- small numeric helpers ----------

  function mulberry(seed) {
    let a = seed >>> 0;
    return () => {
      a = (a + 0x6d2b79f5) >>> 0;
      let t = Math.imul(a ^ (a >>> 15), 1 | a);
      t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }

  function hash2(ix, iy, s) {
    let h = Math.imul(ix, 374761393) + Math.imul(iy, 668265263) + Math.imul(s | 0, 1442695041);
    h = Math.imul(h ^ (h >>> 13), 1274126177);
    return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
  }

  // Smooth value noise in [0,1].
  function noise(x, y, s = 0) {
    const ix = Math.floor(x), iy = Math.floor(y), fx = x - ix, fy = y - iy;
    const u = fx * fx * (3 - 2 * fx), v = fy * fy * (3 - 2 * fy);
    const a = hash2(ix, iy, s), b = hash2(ix + 1, iy, s), c = hash2(ix, iy + 1, s), d = hash2(ix + 1, iy + 1, s);
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
  }

  function fbm(x, y, s = 0) {
    return noise(x, y, s) * 0.55 + noise(x * 2.1, y * 2.1, s + 7) * 0.30 + noise(x * 4.3, y * 4.3, s + 13) * 0.15;
  }

  const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

  function parseHex(hex) {
    const h = hex.charAt(0) === '#' ? hex.slice(1) : hex;
    const n = parseInt(h.length === 3 ? h.replace(/./g, '$&$&') : h, 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
  }

  const toCss = (r, g, b) => `rgb(${r | 0},${g | 0},${b | 0})`;

  // ---------- the painting kit handed to every scene ----------

  // 5x7 pixel lettering for painted signs.
  const GLYPHS = {
    A: '01110 10001 10001 11111 10001 10001 10001', B: '11110 10001 10001 11110 10001 10001 11110',
    C: '01110 10001 10000 10000 10000 10001 01110', D: '11110 10001 10001 10001 10001 10001 11110',
    E: '11111 10000 10000 11110 10000 10000 11111', F: '11111 10000 10000 11110 10000 10000 10000',
    G: '01110 10001 10000 10111 10001 10001 01111', H: '10001 10001 10001 11111 10001 10001 10001',
    I: '01110 00100 00100 00100 00100 00100 01110', J: '00111 00010 00010 00010 00010 10010 01100',
    K: '10001 10010 10100 11000 10100 10010 10001', L: '10000 10000 10000 10000 10000 10000 11111',
    M: '10001 11011 10101 10101 10001 10001 10001', N: '10001 11001 10101 10011 10001 10001 10001',
    O: '01110 10001 10001 10001 10001 10001 01110', P: '11110 10001 10001 11110 10000 10000 10000',
    Q: '01110 10001 10001 10001 10101 10010 01101', R: '11110 10001 10001 11110 10100 10010 10001',
    S: '01111 10000 10000 01110 00001 00001 11110', T: '11111 00100 00100 00100 00100 00100 00100',
    U: '10001 10001 10001 10001 10001 10001 01110', V: '10001 10001 10001 10001 10001 01010 00100',
    W: '10001 10001 10001 10101 10101 11011 10001', X: '10001 10001 01010 00100 01010 10001 10001',
    Y: '10001 10001 01010 00100 00100 00100 00100', Z: '11111 00001 00010 00100 01000 10000 11111',
    0: '01110 10001 10011 10101 11001 10001 01110', 1: '00100 01100 00100 00100 00100 00100 01110',
    2: '01110 10001 00001 00010 00100 01000 11111', 3: '11110 00001 00001 01110 00001 00001 11110',
    4: '00010 00110 01010 10010 11111 00010 00010', 5: '11111 10000 11110 00001 00001 10001 01110',
    6: '00110 01000 10000 11110 10001 10001 01110', 7: '11111 00001 00010 00100 01000 01000 01000',
    8: '01110 10001 10001 01110 10001 10001 01110', 9: '01110 10001 10001 01111 00001 00010 01100',
    ' ': '00000 00000 00000 00000 00000 00000 00000', '.': '00000 00000 00000 00000 00000 01100 01100',
    '-': '00000 00000 00000 11111 00000 00000 00000', '/': '00001 00010 00010 00100 01000 01000 10000',
    '!': '00100 00100 00100 00100 00100 00000 00100', '?': '01110 10001 00001 00010 00100 00000 00100',
    ',': '00000 00000 00000 00000 01100 00100 01000', ':': '00000 01100 01100 00000 01100 01100 00000',
    '^': '00100 01110 10101 10100 10101 01110 00100', '&': '01100 10010 10100 01000 10101 10010 01101',
    '+': '00000 00100 00100 11111 00100 00100 00000', '\'': '00100 00100 01000 00000 00000 00000 00000', '$': '00100 01111 10100 01110 00101 11110 00100',
  };
  const glyphRows = {};
  for (const k of Object.keys(GLYPHS)) glyphRows[k] = GLYPHS[k].split(' ');

  const BAYER8 = (() => {
    const m = [[0]];
    let size = 1;
    let cur = [[0]];
    while (size < 8) {
      const next = [];
      for (let y = 0; y < size * 2; y++) {
        next.push([]);
        for (let x = 0; x < size * 2; x++) {
          const q = (y < size ? 0 : 2) + (x < size ? 0 : 1);
          const base = cur[y % size][x % size] * 4;
          next[y].push(base + [0, 2, 3, 1][q]);
        }
      }
      cur = next;
      size *= 2;
    }
    void m;
    return cur.flat();
  })();

  function makeKit(dark) {
    const P = { dusk: dark, W, H, mulberry, noise, fbm, clamp };

    // Colour in the scene's mode. Day colours are written once; dusk is a cool, dark grade of them.
    // An optional second argument overrides the dusk colour.
    P.col = (hex, night) => {
      if (!dark || hex.charAt(0) !== '#') return hex;
      if (night) return night;
      const [r, g, b] = parseHex(hex);
      return toCss(clamp(r * 0.34 + 20, 0, 255), clamp(g * 0.36 + 22, 0, 255), clamp(b * 0.54 + 54, 0, 255));
    };
    P.lit = (hex) => { const c = parseHex(hex); return toCss(c[0], c[1], c[2]); };   // light sources keep their colour in both modes
    P.pick = (day, night) => (dark ? night : day);
    P.mix = (a, b, k) => {
      const A = parseHex(a), B = parseHex(b);
      return toCss(A[0] + (B[0] - A[0]) * k, A[1] + (B[1] - A[1]) * k, A[2] + (B[2] - A[2]) * k);
    };
    P.alpha = (hex, a) => {
      const c = parseHex(hex);
      return `rgba(${c[0]},${c[1]},${c[2]},${a})`;
    };

    P.grad = (g, x0, y0, x1, y1, stops) => {
      const gr = g.createLinearGradient(x0, y0, x1, y1);
      for (const [k, c] of stops) gr.addColorStop(k, c.charAt(0) === '#' ? P.col(c) : c);
      return gr;
    };

    // Same, but the colours are used as given in both modes (skies, lamps).
    P.gradRaw = (g, x0, y0, x1, y1, stops) => {
      const gr = g.createLinearGradient(x0, y0, x1, y1);
      for (const [k, c] of stops) gr.addColorStop(k, c);
      return gr;
    };

    P.radial = (g, x, y, r0, r1, stops) => {
      const gr = g.createRadialGradient(x, y, r0, x, y, r1);
      for (const [k, c] of stops) gr.addColorStop(k, c);
      return gr;
    };

    // Brush wobble: a stable offset for a point, so neighbouring shapes share the same ragged seam.
    const jit = (x, y, seed) => (noise(x * 0.17 + seed * 3.1, y * 0.17 - seed, seed) - 0.5) * 2;

    // Builds (without filling) a soft, wobbly outline through `pts`.
    P.trace = (g, pts, wob = 1, seed = 1, closed = true) => {
      const out = [];
      const n = pts.length, last = closed ? n : n - 1;
      for (let i = 0; i < last; i++) {
        const a = pts[i], b = pts[(i + 1) % n];
        const len = Math.hypot(b[0] - a[0], b[1] - a[1]);
        const steps = Math.max(1, Math.round(len / 5));
        for (let s = 0; s < steps; s++) {
          const k = s / steps, x = a[0] + (b[0] - a[0]) * k, y = a[1] + (b[1] - a[1]) * k;
          out.push([x + jit(x, y, seed) * wob, y + jit(y, x, seed + 5) * wob]);
        }
      }
      if (!closed) out.push([pts[n - 1][0], pts[n - 1][1]]);
      g.beginPath();
      if (out.length < 3) { g.moveTo(out[0][0], out[0][1]); for (const p of out) g.lineTo(p[0], p[1]); return; }
      if (closed) {
        const m0 = [(out[0][0] + out[out.length - 1][0]) / 2, (out[0][1] + out[out.length - 1][1]) / 2];
        g.moveTo(m0[0], m0[1]);
        for (let i = 0; i < out.length; i++) {
          const c = out[i], d = out[(i + 1) % out.length];
          g.quadraticCurveTo(c[0], c[1], (c[0] + d[0]) / 2, (c[1] + d[1]) / 2);
        }
        g.closePath();
      } else {
        g.moveTo(out[0][0], out[0][1]);
        for (let i = 1; i < out.length - 1; i++) {
          const c = out[i], d = out[i + 1];
          g.quadraticCurveTo(c[0], c[1], (c[0] + d[0]) / 2, (c[1] + d[1]) / 2);
        }
        g.lineTo(out[out.length - 1][0], out[out.length - 1][1]);
      }
    };

    const bbox = (pts) => {
      let x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
      for (const p of pts) { x0 = Math.min(x0, p[0]); y0 = Math.min(y0, p[1]); x1 = Math.max(x1, p[0]); y1 = Math.max(y1, p[1]); }
      return [x0, y0, x1, y1];
    };

    // fill: one colour, or an array of colours washed across the shape (light at the upper left,
    // dark at the lower right, or top to bottom with o.v).
    const makeFill = (g, fill, pts, o) => {
      if (!Array.isArray(fill)) return fill.charAt(0) === '#' ? P.col(fill) : fill;
      const [x0, y0, x1, y1] = bbox(pts);
      const stops = fill.map((c, i) => [fill.length === 1 ? 0 : i / (fill.length - 1), c]);
      return o.v ? P.grad(g, 0, y0, 0, y1, stops) : P.grad(g, x0, y0, x1, y1, stops);
    };

    // A flat brushed shape. o: wob, seed, edge (colour for the pigment line), ea (its alpha).
    P.shape = (g, pts, fill, o = {}) => {
      const wob = o.wob === undefined ? 1.1 : o.wob;
      P.trace(g, pts, wob, o.seed || 1, o.closed !== false);
      g.fillStyle = makeFill(g, fill, pts, o);
      g.fill();
      if (o.edge) {
        g.strokeStyle = P.alpha(o.edge.charAt(0) === '#' ? rgbOf(P.col(o.edge)) : o.edge, o.ea === undefined ? 0.4 : o.ea);
        g.lineWidth = o.ew || 1;
        g.stroke();
      }
    };

    const rgbOf = (css) => {
      if (css.charAt(0) === '#') return css;
      const m = /(\d+),(\d+),(\d+)/.exec(css);
      return '#' + [m[1], m[2], m[3]].map((v) => (+v).toString(16).padStart(2, '0')).join('');
    };

    P.ellipsePts = (cx, cy, rx, ry, n = 14, rot = 0) => {
      const pts = [];
      for (let i = 0; i < n; i++) {
        const a = (i / n) * Math.PI * 2;
        const x = Math.cos(a) * rx, y = Math.sin(a) * ry;
        pts.push([cx + x * Math.cos(rot) - y * Math.sin(rot), cy + x * Math.sin(rot) + y * Math.cos(rot)]);
      }
      return pts;
    };

    P.blob = (g, cx, cy, rx, ry, fill, o = {}) => P.shape(g, P.ellipsePts(cx, cy, rx, ry, o.n || 14, o.rot || 0), fill, o);

    P.rect = (g, x, y, w, h, fill, o = {}) => P.shape(g, [[x, y], [x + w, y], [x + w, y + h], [x, y + h]], fill, { wob: 0.5, ...o });

    // Runs fn with the drawing clipped to a shape; fn paints texture inside it.
    P.within = (g, pts, o, fn) => {
      g.save();
      P.trace(g, pts, o && o.wob !== undefined ? o.wob : 1.1, (o && o.seed) || 1, !(o && o.closed === false));
      g.clip();
      fn();
      g.restore();
    };

    // Loose brush dabs: n small soft marks inside a box, coloured at random from `cols`.
    P.dabs = (g, x, y, w, h, cols, n, size, seed, a = 0.6) => {
      const r = mulberry(seed);
      for (let i = 0; i < n; i++) {
        const dx = x + r() * w, dy = y + r() * h, s = size * (0.5 + r());
        g.fillStyle = P.alpha(rgbOf(P.col(cols[(r() * cols.length) | 0])), a);
        g.beginPath();
        g.ellipse(dx, dy, s, s * (0.5 + r() * 0.4), (r() - 0.5) * 1.2, 0, Math.PI * 2);
        g.fill();
      }
    };

    // Single-pixel grain (speckle, gravel, stars, sparkle).
    P.grain = (g, x, y, w, h, col, n, seed, a = 1) => {
      const r = mulberry(seed);
      g.fillStyle = col.charAt(0) === '#' ? P.alpha(rgbOf(P.col(col)), a) : col;
      for (let i = 0; i < n; i++) g.fillRect(Math.floor(x + r() * w), Math.floor(y + r() * h), 1, 1);
    };

    // A mass of foliage: dark underpaint, mid body, light dabs up toward the sun, speckled rim.
    P.canopy = (g, cx, cy, rx, ry, cols, seed) => {
      const [dk, md, lt] = cols;
      const pts = P.ellipsePts(cx, cy, rx, ry, 12);
      P.shape(g, pts, dk, { wob: Math.max(1.5, rx * 0.12), seed });
      P.within(g, pts, { wob: Math.max(1.5, rx * 0.12), seed }, () => {
        P.dabs(g, cx - rx, cy - ry, rx * 1.7, ry * 1.4, [md], Math.round(rx * ry * 0.12), 2.6, seed + 1, 0.85);
        P.dabs(g, cx - rx * 0.9, cy - ry, rx * 1.1, ry * 0.9, [lt], Math.round(rx * ry * 0.07), 2.0, seed + 2, 0.75);
        P.dabs(g, cx - rx * 0.6, cy - ry * 0.9, rx * 0.7, ry * 0.5, [lt], Math.round(rx * ry * 0.03), 1.6, seed + 3, 0.9);
        P.dabs(g, cx - rx * 0.2, cy + ry * 0.1, rx * 1.2, ry * 0.9, [dk], Math.round(rx * ry * 0.05), 2.2, seed + 4, 0.55);
      });
    };

    // A ridge line across the picture: filled down to `bottom` with a vertical wash.
    P.ridge = (g, o) => {
      const { y, amp, freq = 0.02, seed = 1, cols, bottom = H, x0 = -4, x1 = W + 4, step = 4, tilt = 0 } = o;
      const pts = [[x0, bottom]];
      for (let x = x0; x <= x1; x += step) pts.push([x, y + tilt * (x - x0) / (x1 - x0) + (fbm(x * freq, seed, seed) - 0.5) * 2 * amp]);
      pts.push([x1, bottom]);
      g.beginPath();
      g.moveTo(pts[0][0], pts[0][1]);
      for (let i = 1; i < pts.length; i++) g.lineTo(pts[i][0], pts[i][1]);
      g.closePath();
      g.fillStyle = P.grad(g, 0, y - amp, 0, bottom, cols.map((c, i) => [i / (cols.length - 1), c]));
      g.fill();
      return pts;
    };

    P.line = (g, pts, col, w = 1, o = {}) => {
      P.trace(g, pts, o.wob || 0, o.seed || 1, false);
      g.strokeStyle = col.charAt(0) === '#' ? P.col(col) : col;
      g.lineWidth = w;
      g.lineCap = 'round';
      g.lineJoin = 'round';
      g.stroke();
    };

    // A cable that sags: quadratic arc from a to b with the given droop.
    P.wire = (g, a, b, droop, col, w = 1) => {
      g.beginPath();
      g.moveTo(a[0], a[1]);
      g.quadraticCurveTo((a[0] + b[0]) / 2, (a[1] + b[1]) / 2 + droop * 2, b[0], b[1]);
      g.strokeStyle = col.charAt(0) === '#' ? P.col(col) : col;
      g.lineWidth = w;
      g.stroke();
    };

    // Soft light: a radial glow added on top.
    P.glow = (g, x, y, r, hex, a = 0.6) => {
      g.save();
      g.globalCompositeOperation = 'lighter';
      g.fillStyle = P.radial(g, x, y, 0, r, [[0, P.alpha(hex, a)], [0.4, P.alpha(hex, a * 0.35)], [1, P.alpha(hex, 0)]]);
      g.fillRect(x - r, y - r, r * 2, r * 2);
      g.restore();
    };

    // A soft cast shadow on the ground.
    P.shadow = (g, cx, cy, rx, ry, a = 0.3) => {
      g.fillStyle = P.alpha(P.pick('#2c1c18', '#0c0820'), a);
      g.beginPath(); g.ellipse(cx, cy, rx, ry, 0, 0, 7); g.fill();
    };

    // A fat, soft cloud (lit from the upper left).
    P.cloud = (g, x, y, s, seed) => {
      const dark = P.pick('#c0d0e8', '#684888'), light = P.pick('#ffffff', '#d8aac8');
      const pts = [[-14, 0], [-10, -5], [-3, -8], [4, -7], [10, -4], [15, -1], [11, 3], [-8, 3]].map(([a, b]) => [x + a * s, y + b * s]);
      P.shape(g, pts, [light, P.mix(light, dark, 0.75)], { wob: 1.4, seed, v: true });
    };

    P.stars = (g, y0, y1, n, seed) => {
      P.grain(g, 0, y0, 320, y1 - y0, '#ffffff', n, seed, 0.9);
      P.grain(g, 0, y0, 320, (y1 - y0) * 0.7, '#ffffff', Math.round(n / 3), seed + 1, 1);
    };

    // A 3.5 inch floppy disk seen from the front, w pixels wide: body, metal shutter, paper label.
    P.floppy = (g, x, y, w, body = '#2c3c8c', label = '#f6f0dc') => {
      const k = w / 10;
      P.shape(g, [[x, y], [x + w - 1.6 * k, y], [x + w, y + 1.6 * k], [x + w, y + w], [x, y + w]], [P.mix(body, '#ffffff', 0.18), body], { wob: 0.15, seed: 3, edge: '#0c0c20', ea: 0.7 });
      g.fillStyle = P.col('#c8ccd8'); g.fillRect(x + 2.2 * k, y, 5 * k, 3.2 * k);
      g.fillStyle = P.col('#5c6074'); g.fillRect(x + 5.6 * k, y + 0.6 * k, 1.2 * k, 2 * k);
      g.fillStyle = P.col(label); g.fillRect(x + 1.4 * k, y + 5 * k, 7.2 * k, 4.6 * k);
      g.fillStyle = P.alpha(P.col('#2c2c44'), 0.7);
      g.fillRect(x + 2 * k, y + 6.4 * k, 4.5 * k, Math.max(1, 0.7 * k));
    };

    // An old wall telephone, 11 x 16 px with its handset on the left: backboard, two bells, mouthpiece, crank.
    P.wallPhone = (g, x, y, sc = 1) => {
      g.save(); g.translate(x, y); g.scale(sc, sc); x = 0; y = 0;
      P.shape(g, [[x, y + 3], [x + 11, y + 3], [x + 11, y + 16], [x, y + 16]], ['#e0a458', '#a86a2c'], { wob: 0.3, seed: 8, edge: '#140a06', ea: 0.9 });
      P.blob(g, x + 3, y + 3, 2.4, 2.2, ['#ffe27a', '#b8801c'], { wob: 0.1, seed: 3, n: 8, edge: '#3a2410', ea: 0.7 });
      P.blob(g, x + 8, y + 3, 2.4, 2.2, ['#ffe27a', '#b8801c'], { wob: 0.1, seed: 4, n: 8, edge: '#3a2410', ea: 0.7 });
      P.blob(g, x + 5.5, y + 9, 2.4, 2.4, ['#4a4a5c', '#14141c'], { wob: 0.1, seed: 5, n: 8, edge: '#000000', ea: 0.8 });
      g.fillStyle = P.col('#ece4d0'); g.fillRect(x + 3, y + 13, 5, 1);
      P.line(g, [[x + 11, y + 9], [x + 14, y + 11]], '#c8c8d0', 1);
      P.blob(g, x + 14.5, y + 11.3, 1.2, 1.2, '#ffe27a', { wob: 0, seed: 1, n: 6 });
      P.shape(g, [[x - 3.5, y + 5], [x - 1, y + 5], [x - 1, y + 14], [x - 3.5, y + 14]], ['#2c2c3c', '#0c0c14'], { wob: 0.15, seed: 6, edge: '#000', ea: 0.7 });
      P.line(g, [[x - 2, y + 14], [x - 4, y + 17], [x + 1, y + 18], [x + 3, y + 16]], '#1c1c28', 1);
      g.restore();
    };

    // A candlestick telephone, 8 x 12 px: heavy base, stem, mouthpiece, earpiece on its fork.
    P.candlePhone = (g, x, y, sc = 1) => {
      g.save(); g.translate(x, y); g.scale(sc, sc); x = 0; y = 0;
      P.shape(g, [[x - 4, y], [x + 4, y], [x + 3, y - 2], [x - 3, y - 2]], ['#3c3c4c', '#0c0c14'], { wob: 0.15, seed: 2, edge: '#000', ea: 0.7 });
      P.rect(g, x - 0.8, y - 9, 1.6, 7.5, ['#e8c04a', '#8c6418'], { wob: 0.1, seed: 3 });
      P.shape(g, [[x - 2.4, y - 9], [x + 2.4, y - 9], [x + 1.6, y - 11.5], [x - 1.6, y - 11.5]], ['#e8c04a', '#8c6418'], { wob: 0.1, seed: 4, edge: '#3a2410', ea: 0.7 });
      P.shape(g, [[x + 1, y - 6], [x + 4.5, y - 6.5], [x + 4.5, y - 4.5], [x + 1, y - 4]], ['#3c3c4c', '#0c0c14'], { wob: 0.1, seed: 5, edge: '#000', ea: 0.7 });
      g.restore();
    };

    // Pixel lettering. Returns the width drawn.
    P.text = (g, str, x, y, col, scale = 1, space = 1) => {
      g.fillStyle = col.charAt(0) === '#' ? P.col(col) : col;
      let cx = x;
      for (const ch of str.toUpperCase()) {
        const rows = glyphRows[ch] || glyphRows['?'];
        for (let r = 0; r < 7; r++)
          for (let c = 0; c < 5; c++)
            if (rows[r].charCodeAt(c) === 49) g.fillRect(cx + c * scale, y + r * scale, scale, scale);
        cx += (5 + space) * scale;
      }
      return cx - x;
    };
    P.textWidth = (str, scale = 1, space = 1) => str.length * (5 + space) * scale;

    return P;
  }

  // ---------- turning a painting into an indexed 256-colour picture ----------

  // Paper and wash: broad blotches of pigment, fine fibre, grain and a faint vignette.
  function texture(rgba, dark, w = W, h = H) {
    const warm = dark ? [0.96, 0.98, 1.04] : [1.02, 1.0, 0.95];
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const i = (y * w + x) * 4;
        const n1 = noise(x * 0.035, y * 0.05, 3) - 0.5;
        const n2 = noise(x * 0.25, y * 0.09, 5) - 0.5;
        const gr = hash2(x, y, 9) - 0.5;
        const vx = (x / w - 0.5) * 2, vy = (y / h - 0.5) * 2;
        const vig = 1 - 0.07 * (vx * vx + vy * vy * 0.6);
        const f = (1 + n1 * 0.13 + n2 * 0.05 + gr * 0.05) * vig;
        const wr = 1 + (warm[0] - 1) * (0.5 + n1), wg = 1 + (warm[1] - 1) * (0.5 + n1), wb = 1 + (warm[2] - 1) * (0.5 + n1);
        rgba[i] = clamp(rgba[i] * f * wr, 0, 255);
        rgba[i + 1] = clamp(rgba[i + 1] * f * wg, 0, 255);
        rgba[i + 2] = clamp(rgba[i + 2] * f * wb, 0, 255);
      }
    }
  }

  // Median cut over a 15-bit histogram: entries are [count, sumR, sumG, sumB].
  function medianCut(hist, want) {
    const entries = [];
    for (const [key, v] of hist) {
      entries.push({ r: (key >> 10) & 31, g: (key >> 5) & 31, b: key & 31, c: v[0], sr: v[1], sg: v[2], sb: v[3], w: Math.sqrt(v[0]) });
    }
    const boxes = [{ e: entries }];
    const stat = (b) => {
      let r0 = 31, r1 = 0, g0 = 31, g1 = 0, b0 = 31, b1 = 0, sw = 0;
      for (const e of b.e) {
        if (e.r < r0) r0 = e.r; if (e.r > r1) r1 = e.r;
        if (e.g < g0) g0 = e.g; if (e.g > g1) g1 = e.g;
        if (e.b < b0) b0 = e.b; if (e.b > b1) b1 = e.b;
        sw += e.w;
      }
      b.rng = [r1 - r0, g1 - g0, b1 - b0];
      b.score = Math.max((r1 - r0) * 1.3, (g1 - g0) * 1.6, (b1 - b0) * 1.0) * Math.sqrt(sw);
    };
    stat(boxes[0]);
    while (boxes.length < want) {
      let bi = -1, bs = 0;
      for (let i = 0; i < boxes.length; i++) if (boxes[i].e.length > 1 && boxes[i].score > bs) { bs = boxes[i].score; bi = i; }
      if (bi < 0) break;
      const b = boxes[bi];
      const ch = b.rng[1] * 1.3 >= b.rng[0] && b.rng[1] * 1.3 >= b.rng[2] ? 'g' : b.rng[0] >= b.rng[2] ? 'r' : 'b';
      b.e.sort((p, q) => p[ch] - q[ch]);
      let tot = 0;
      for (const e of b.e) tot += e.w;
      let acc = 0, cut = 1;
      for (let i = 0; i < b.e.length - 1; i++) { acc += b.e[i].w; cut = i + 1; if (acc >= tot / 2) break; }
      const lo = { e: b.e.slice(0, cut) }, hi = { e: b.e.slice(cut) };
      stat(lo); stat(hi);
      boxes[bi] = lo;
      boxes.push(hi);
    }
    return boxes.map((b) => {
      let c = 0, r = 0, g = 0, bl = 0;
      for (const e of b.e) { c += e.c; r += e.sr; g += e.sg; bl += e.sb; }
      return [Math.round(r / c), Math.round(g / c), Math.round(bl / c)];
    });
  }

  function addHist(hist, rgba, weight, alphaOnly) {
    for (let i = 0; i < rgba.length; i += 4) {
      if (alphaOnly && rgba[i + 3] < 128) continue;
      const r = rgba[i], g = rgba[i + 1], b = rgba[i + 2];
      const key = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
      const v = hist.get(key);
      if (v) { v[0] += weight; v[1] += r * weight; v[2] += g * weight; v[3] += b * weight; }
      else hist.set(key, [weight, r * weight, g * weight, b * weight]);
    }
  }

  function makeQuantiser(palette) {
    const n = palette.length;
    const pal32 = new Uint32Array(n);
    const lut = new Int16Array(32768).fill(-1);
    for (let i = 0; i < n; i++) pal32[i] = (255 << 24) | (palette[i][2] << 16) | (palette[i][1] << 8) | palette[i][0];
    const nearest = (key) => {
      const r = ((key >> 10) & 31) * 8 + 4, g = ((key >> 5) & 31) * 8 + 4, b = (key & 31) * 8 + 4;
      let best = 0, bd = 1e12;
      for (let i = 0; i < n; i++) {
        const dr = palette[i][0] - r, dg = palette[i][1] - g, db = palette[i][2] - b;
        const d = dr * dr * 2 + dg * dg * 4 + db * db * 3;
        if (d < bd) { bd = d; best = i; }
      }
      return best;
    };
    const offs = new Int8Array(64);
    for (let i = 0; i < 64; i++) offs[i] = Math.round(((BAYER8[i] + 0.5) / 64 - 0.5) * DITHER);
    return {
      pal32,
      // Dither and map the rectangle (x,y,w,h) of an RGBA source (sw pixels wide) into the 32-bit output.
      run(src, out32, x0, y0, w, h, sw = W) {
        for (let y = y0; y < y0 + h; y++) {
          for (let x = x0; x < x0 + w; x++) {
            const i = (y * sw + x) * 4, o = offs[((y & 7) << 3) | (x & 7)];
            const r = src[i] + o, g = src[i + 1] + o, b = src[i + 2] + o;
            const key = ((r < 0 ? 0 : r > 255 ? 255 : r) >> 3 << 10) | ((g < 0 ? 0 : g > 255 ? 255 : g) >> 3 << 5) | ((b < 0 ? 0 : b > 255 ? 255 : b) >> 3);
            let idx = lut[key];
            if (idx < 0) idx = lut[key] = nearest(key);
            out32[y * sw + x] = pal32[idx];
          }
        }
      },
    };
  }

  function canvas2d(w, h) {
    const c = document.createElement('canvas');
    c.width = w; c.height = h;
    return { c, g: c.getContext('2d', { willReadFrequently: true }) };
  }

  // Paints a scene and returns everything the animation loop needs.
  function build(scene, dark) {
    const t0 = performance.now();
    const P = makeKit(dark);
    const { c: baseCv, g } = canvas2d(W, H);
    scene.paint(g, P, 0);
    const base = g.getImageData(0, 0, W, H).data;
    texture(base, dark);

    const hist = new Map();
    addHist(hist, base, 1, false);
    const anims = (scene.anims || []).concat(sparkAnims(scene)).map((a) => ({ ...a, on: false, rect: [a.x, a.y, a.w, a.h] }));
    const sprite = canvas2d(W, H);
    for (const a of anims) {
      sprite.g.clearRect(0, 0, W, H);
      sprite.g.save();
      a.draw(sprite.g, a.sample === undefined ? 0 : a.sample, P, a.gag || a.when ? 0.5 : undefined);
      sprite.g.restore();
      addHist(hist, sprite.g.getImageData(a.x, a.y, a.w, a.h).data, 3, true);
    }
    const extra = (scene.accents || []).map((hex) => parseHex(P.col(hex))).concat(SPARK_COLS.map(parseHex));
    const palette = medianCut(hist, 256 - extra.length).concat(extra);
    const quant = makeQuantiser(palette);

    const display = canvas2d(W, H);
    const imageData = display.g.createImageData(W, H);
    const out32 = new Uint32Array(imageData.data.buffer);
    quant.run(base, out32, 0, 0, W, H);
    display.g.putImageData(imageData, 0, 0);
    void baseCv;
    const ms = performance.now() - t0;
    return { P, base, anims, quant, display, imageData, out32, work: new Uint8ClampedArray(base), sprite, ms, dark };
  }

  // A small picture in the scenes' look (tools/make_logo.mjs paints the site's emblem with it): fn(g, P) paints with
  // the kit, then the same paper texture, a palette of its own and the same ordered dither. The kit is the day one
  // (colours as given, P.dusk still says the mode): the picture grades its own dusk.
  function paintSmall(w, h, dark, fn, colours = 40) {
    const P = makeKit(false);
    P.dusk = dark;
    const { c, g } = canvas2d(w, h);
    fn(g, P);
    const img = g.getImageData(0, 0, w, h);
    texture(img.data, dark, w, h);
    const hist = new Map();
    addHist(hist, img.data, 1, false);
    makeQuantiser(medianCut(hist, colours)).run(img.data, new Uint32Array(img.data.buffer), 0, 0, w, h, w);
    g.putImageData(img, 0, 0);
    return c;
  }

  // ---------- the animation loop ----------

  function gagPhase(a, t) {
    const s = t - a.gag.start;
    if (s < 0) return -1;
    const m = s % a.gag.period;
    return m < a.gag.dur ? m / a.gag.dur : -1;
  }

  // Twinkling pixels on an object that is the next step: a burst of a second every 2.6 s (static when
  // motion is reduced), drawn only while state.spark[role] is set.
  const SPARK_COLS = ['#ffffff', '#ffe040', '#fff4b0'];
  const SPARK_EVERY = 2.6, SPARK_BURST = 1.1;

  function sparkAnims(scene) {
    return Object.entries(scene.spark || {}).map(([role, r]) => ({
      x: r[0], y: r[1], w: r[2], h: r[3], sample: 0, role,
      when: (t, still) => (!state.spark[role] ? -1 : still ? 0.5 : t % SPARK_EVERY < SPARK_BURST ? (t % SPARK_EVERY) / SPARK_BURST : -1),
      draw(g, t, P, u) {
        const rnd = mulberry(Math.floor(t / SPARK_EVERY) * 17 + role.length);
        for (let i = 0; i < 5; i++) {
          const px = Math.floor(r[0] + 3 + rnd() * (r[2] - 6)), py = Math.floor(r[1] + 3 + rnd() * (r[3] - 6));
          const k = Math.sin(clamp(u + i * 0.07, 0, 1) * Math.PI);
          if (k < 0.15) continue;
          const arm = k > 0.7 ? 2 : 1;
          g.fillStyle = SPARK_COLS[1];
          g.fillRect(px - arm, py, arm * 2 + 1, 1);
          g.fillRect(px, py - arm, 1, arm * 2 + 1);
          g.fillStyle = SPARK_COLS[0];
          g.fillRect(px, py, 1, 1);
        }
      },
    }));
  }

  function setSpark(role, on) {
    if (state.spark[role] === on) return;
    state.spark[role] = on;
    if (state.built && !state.running) frame(state.built, 0, true);
  }

  function frame(b, t, still) {
    const rects = [];
    const live = [];
    for (const a of b.anims) {
      let u;
      let on = true;
      if (a.when) { u = a.when(t, still); on = u >= 0; }
      else if (a.gag) { u = gagPhase(a, t); on = !still && u >= 0; }
      if (on) live.push([a, u]);
      if (on || a.on) rects.push(a.rect);
      a.on = on;
    }
    if (!rects.length) return;
    const { work, base, sprite, quant, out32 } = b;
    for (const [x, y, w, h] of rects)
      for (let yy = y; yy < y + h; yy++) work.set(base.subarray((yy * W + x) * 4, (yy * W + x + w) * 4), (yy * W + x) * 4);
    for (const [a, u] of live) {
      sprite.g.clearRect(a.x, a.y, a.w, a.h);
      sprite.g.save();
      sprite.g.beginPath();
      sprite.g.rect(a.x, a.y, a.w, a.h);
      sprite.g.clip();
      a.draw(sprite.g, t, b.P, u);
      sprite.g.restore();
      const px = sprite.g.getImageData(a.x, a.y, a.w, a.h).data;
      for (let yy = 0; yy < a.h; yy++) {
        for (let xx = 0; xx < a.w; xx++) {
          const s = (yy * a.w + xx) * 4, al = px[s + 3];
          if (!al) continue;
          const d = ((a.y + yy) * W + a.x + xx) * 4, k = al / 255;
          work[d] = work[d] + (px[s] - work[d]) * k;
          work[d + 1] = work[d + 1] + (px[s + 1] - work[d + 1]) * k;
          work[d + 2] = work[d + 2] + (px[s + 2] - work[d + 2]) * k;
        }
      }
    }
    const g = b.display.g;
    for (const [x, y, w, h] of rects) {
      quant.run(work, out32, x, y, w, h);
      g.putImageData(b.imageData, 0, 0, x, y, w, h);
    }
  }

  let animOff = false;
  const reduced = () => animOff || matchMedia('(prefers-reduced-motion: reduce)').matches;
  function setAnimation(on) {
    animOff = !on;
    if (state.built) { frame(state.built, 0, true); syncRunning(); }
  }
  const isDark = () => { const t = document.documentElement.dataset.theme; return t ? t === 'dark' : !matchMedia('(prefers-color-scheme: light)').matches; };
  const landingShown = () => document.body.dataset.view === 'setup' || document.body.dataset.view === 'dialing';

  function loop(now) {
    state.raf = 0;
    if (!state.running || !state.built) return;
    if (now - state.last >= 1000 / FPS) {
      state.last = now;
      frame(state.built, (now - state.t0) / 1000, false);
    }
    state.raf = requestAnimationFrame(loop);
  }

  function syncRunning() {
    const want = !!state.built && landingShown() && !document.hidden && !reduced();
    if (want && !state.running) {
      state.running = true;
      state.t0 = performance.now();
      state.raf = requestAnimationFrame(loop);
    } else if (!want && state.running) {
      state.running = false;
      if (state.raf) cancelAnimationFrame(state.raf);
      state.raf = 0;
    }
  }

  // ---------- placing the canvas, hotspots and the narrator box ----------

  const view = { s: 1, ox: 0, oy: 0, cx: 0, wide: false, top: '', bot: '' };
  const adventure = () => !CLASSIC && typeof Adv !== 'undefined';

  function layout() {
    if (!els || !state.scene) return;
    const vw = innerWidth, vh = innerHeight;
    const f = state.scene.focus || { x: 220, y: 120 };
    const portrait = vw < vh;
    // On a phone the picture floats above a strip left for the narrator box and fades into sky and
    // ground at its edges; drag it sideways.
    const s = portrait ? Math.max(vw / W, clamp(vh - 250, vh * 0.45, vh * 0.62) / H) : Math.max(vw / W, vh / H);
    if (!view.cx) view.cx = f.x;
    const mid = CLASSIC && !portrait ? 0.62 : 0.5;     // the old card covers the left of the picture
    const ox = clamp(vw * mid - view.cx * s, vw - W * s, 0);
    const oy = portrait ? Math.max(40, vh - 210 - H * s) : clamp(vh * 0.5 - f.y * s, vh - H * s, 0);
    view.s = s; view.ox = ox; view.oy = oy; view.wide = W * s > vw + 1;
    view.cx = (vw * mid - ox) / s;
    const st = els.canvas.style;
    st.maskImage = portrait ? 'linear-gradient(to bottom, transparent, #000 12%, #000 88%, transparent)' : '';
    st.webkitMaskImage = st.maskImage;
    st.width = `${W * s}px`; st.height = `${H * s}px`; st.left = `${ox}px`; st.top = `${oy}px`;
    if (view.top) els.root.style.background = `linear-gradient(to bottom, ${view.top} ${oy}px, ${view.bot} ${oy + H * s}px)`;
  }

  // Brings scene column x to the middle of the screen (a no-op when the whole picture fits).
  function panTo(x) {
    view.cx = x;
    layout();
  }

  // Where a scene rectangle sits on screen, in CSS pixels.
  function screenRect(r) {
    return { x: view.ox + r[0] * view.s, y: view.oy + r[1] * view.s, w: r[2] * view.s, h: r[3] * view.s };
  }

  const quip = { timer: 0, hover: 0, hoverId: '', type: 0, count: {} };

  function hideQuip() {
    clearTimeout(quip.timer); clearInterval(quip.type);
    els.quip.hidden = true;
  }

  function hotAt(clientX, clientY) {
    const sc = state.scene;
    if (!sc || !sc.hotspots) return null;
    const x = (clientX - view.ox) / view.s, y = (clientY - view.oy) / view.s;
    for (let i = sc.hotspots.length - 1; i >= 0; i--) {
      const h = sc.hotspots[i];
      if (x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h) return h;
    }
    return null;
  }

  // The next line a hotspot says (they take turns).
  function nextQuip(h) {
    const key = `${state.id}/${h.id}`;
    const n = quip.count[key] = (quip.count[key] || 0) + 1;
    return h.say[(n - 1) % h.say.length];
  }

  // The floating box of the ?classic=1 layout; the adventure layout uses the narrator box (adv.js).
  function say(h) {
    const text = nextQuip(h);
    const el = els.quip;
    clearTimeout(quip.timer); clearInterval(quip.type);
    el.hidden = false;
    el.textContent = text;
    const bw = Math.min(330, innerWidth - 24);
    el.style.width = `${bw}px`;
    const cx = view.ox + (h.x + h.w / 2) * view.s, top = view.oy + h.y * view.s, bottom = view.oy + (h.y + h.h) * view.s;
    const hh = el.offsetHeight;
    el.style.left = `${clamp(cx - bw / 2, 8, innerWidth - bw - 8)}px`;
    const above = top - hh - 14;
    el.style.top = `${clamp(above >= 8 ? above : bottom + 14, 8, innerHeight - hh - 8)}px`;
    if (reduced()) { quip.timer = setTimeout(hideQuip, QUIP_MS); return; }
    el.textContent = '';
    let i = 0;
    quip.type = setInterval(() => {
      el.textContent = text.slice(0, ++i);
      if (i >= text.length) clearInterval(quip.type);
    }, TYPE_MS);
    quip.timer = setTimeout(hideQuip, QUIP_MS + text.length * TYPE_MS);
  }

  function wireScene() {
    const root = els.root;
    let down = null;
    root.addEventListener('pointermove', (e) => {
      if (down && down.id === e.pointerId) {
        const dx = e.clientX - down.x;
        if (!down.moved && Math.abs(dx) > 8 && view.wide) { down.moved = true; els.canvas.classList.add('dragging'); }
        if (down.moved) { panTo(down.cx - dx / view.s); return; }
      }
      const h = hotAt(e.clientX, e.clientY);
      root.style.cursor = h ? 'pointer' : '';
      const id = h ? h.id : '';
      if (id === quip.hoverId) return;
      quip.hoverId = id;
      if (h && h.role === 'disks') wantRuntime();   // page.js: the player is reaching for the disks
      clearTimeout(quip.hover);
      if (h && e.pointerType === 'mouse') quip.hover = setTimeout(() => (adventure() ? Adv.hover(h) : say(h)), HOVER_MS);
    });
    root.addEventListener('pointerdown', (e) => { root.dataset.dragged = ''; down = { id: e.pointerId, x: e.clientX, cx: view.cx, moved: false }; });
    const up = () => { const was = down && down.moved; down = null; els.canvas.classList.remove('dragging'); return was; };
    root.addEventListener('pointercancel', up);
    root.addEventListener('pointerup', (e) => { root.dataset.dragged = up() ? '1' : ''; void e; });
    root.addEventListener('pointerleave', () => { quip.hoverId = ''; clearTimeout(quip.hover); });
    root.addEventListener('click', (e) => {
      if (root.dataset.dragged) { root.dataset.dragged = ''; return; }
      const h = hotAt(e.clientX, e.clientY);
      clearTimeout(quip.hover);
      if (adventure()) Adv.click(h, e);
      else if (h) say(h); else hideQuip();
    });
  }

  // ---------- choosing and loading scenes ----------

  function chooseId() {
    const q = new URLSearchParams(location.search).get('scene');
    return IDS.includes(q) ? q : IDS[Math.floor(Math.random() * IDS.length)];
  }

  function loadScript(id) {
    return new Promise((resolve, reject) => {
      if (registry[id]) return resolve(registry[id]);
      const s = document.createElement('script');
      s.src = innAsset(`scenes/${id}.js`);
      s.onload = () => (registry[id] ? resolve(registry[id]) : reject(new Error(`scene ${id} did not register`)));
      s.onerror = () => reject(new Error(`could not load scene ${id}`));
      document.head.append(s);
    });
  }

  function paintNow() {
    if (!state.scene) return;
    const b = build(state.scene, isDark());
    state.built = b;
    const ctx = els.canvas.getContext('2d');
    ctx.drawImage(b.display.c, 0, 0);
    // Keep drawing into the canvas the page shows.
    b.display = { c: els.canvas, g: ctx };
    frame(b, 0, true);
    const top = b.base;
    let tr = 0, tg = 0, tb = 0, br = 0, bg = 0, bb = 0;
    for (let x = 0; x < W; x++) {
      tr += top[x * 4]; tg += top[x * 4 + 1]; tb += top[x * 4 + 2];
      const q = ((H - 1) * W + x) * 4;
      br += top[q]; bg += top[q + 1]; bb += top[q + 2];
    }
    view.top = toCss(tr / W, tg / W, tb / W);
    view.bot = toCss(br / W, bg / W, bb / W);
    layout();
    InnScenes.lastPaintMs = b.ms;
    performance.mark('inn-scene-painted');   // the first one is the landing's first paint (measured in docs/web.md)
    console.log(`scene ${state.id}: painted in ${b.ms.toFixed(1)} ms`);
    syncRunning();
    if (typeof Adv !== 'undefined' && Adv.scenePainted) Adv.scenePainted();   // the line under the sign-in buttons follows the scene
  }

  async function setScene(id, push) {
    if (!IDS.includes(id)) id = IDS[0];
    hideQuip();
    state.id = id;
    for (const b of els.pick ? els.pick.querySelectorAll('button') : []) b.setAttribute('aria-pressed', String(b.dataset.id === id));
    if (push) {
      const u = new URL(location.href);
      u.searchParams.set('scene', id);
      history.replaceState(null, '', u);
    }
    try {
      const sc = await loadScript(id);
      if (state.id !== id) return;
      state.scene = sc;
      view.cx = 0;
      els.canvas.hidden = false;
      layout();
      paintNow();
      if (typeof Adv !== 'undefined' && Adv.sceneChanged) Adv.sceneChanged(sc);
    } catch (err) {
      console.log(String(err));
      if (id !== IDS[0]) setScene(IDS[0], false);
    }
  }

  function buildPicker() {
    const bar = document.createElement('div');
    bar.id = 'scenepick';
    bar.setAttribute('role', 'group');
    bar.setAttribute('aria-label', 'Scenes');
    const label = document.createElement('span');
    label.textContent = 'SCENE';
    bar.append(label);
    for (const id of IDS) {
      const b = document.createElement('button');
      b.type = 'button'; b.dataset.id = id; b.textContent = id;
      b.addEventListener('click', (e) => { e.stopPropagation(); setScene(id, true); });
      bar.append(b);
    }
    document.body.append(bar);
    return bar;
  }

  function init() {
    const root = document.querySelector('.scene');
    if (!root) return;
    const canvas = document.createElement('canvas');
    canvas.id = 'scene-canvas';
    canvas.width = W; canvas.height = H;
    canvas.hidden = true;
    const box = document.createElement('div');
    box.id = 'quip';
    box.hidden = true;
    box.setAttribute('role', 'status');
    root.append(canvas);
    document.body.append(box);
    els = { root, canvas, quip: box, pick: null };
    if (new URLSearchParams(location.search).has('scenes')) els.pick = buildPicker();
    wireScene();
    addEventListener('resize', () => { layout(); hideQuip(); });
    document.addEventListener('visibilitychange', syncRunning);
    new MutationObserver(() => { syncRunning(); if (!landingShown()) hideQuip(); })
      .observe(document.body, { attributes: true, attributeFilter: ['data-view'] });
    const redo = () => { paintNow(); };
    matchMedia('(prefers-color-scheme: light)').addEventListener('change', redo);
    new MutationObserver(redo).observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
    matchMedia('(prefers-reduced-motion: reduce)').addEventListener('change', () => { if (state.built) { frame(state.built, 0, true); syncRunning(); } });
    setScene(chooseId(), false);
  }

  return {
    register(def) { registry[def.id] = def; },
    init, setScene, setSpark, setAnimation, reduced, panTo, screenRect, nextQuip, hotAt,
    scene: () => state.scene, id: () => state.id, view: () => view, paintSmall,
    dark: () => (state.built ? state.built.dark : isDark()),
    classic: CLASSIC, lastPaintMs: 0, ids: IDS,
  };
})();

InnScenes.init();
