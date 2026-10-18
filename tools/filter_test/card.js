// A fixed 320x200 test card, drawn with no anti-aliasing so every filter gets the same input. Nothing in it is
// taken from the game: colour bars, lines at several slopes, a circle, a triangle, small invented sprites,
// thresholded text, and a one-pixel frame with coloured corners to expose any half-pixel shift.
'use strict';
function makeCard(W = 320, H = 200) {
  const px = new Uint32Array(W * H);
  const rgb = (r, g, b) => (0xff000000 | (r << 16) | (g << 8) | b) >>> 0;
  const set = (x, y, c) => { if (x >= 0 && y >= 0 && x < W && y < H) px[y * W + x] = c; };
  const rect = (x, y, w, h, c) => { for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) set(x + i, y + j, c); };
  // Sky and ground with a 2x2 dither between bands.
  const sky = [rgb(40, 60, 140), rgb(60, 90, 170), rgb(90, 130, 200), rgb(140, 180, 225)];
  for (let y = 0; y < 100; y++) for (let x = 0; x < W; x++) {
    const t = y / 25, band = Math.min(3, Math.floor(t)), f = t - Math.floor(t);
    const next = Math.min(3, band + 1);
    const d = [[0, 0.5], [0.75, 0.25]][y & 1][x & 1];
    set(x, y, f > d && next !== band ? sky[next] : sky[band]);
  }
  rect(0, 100, W, 100, rgb(60, 110, 50));
  for (let y = 100; y < H; y++) for (let x = 0; x < W; x++) if (((x >> 2) + (y >> 2)) & 1) set(x, y, rgb(50, 98, 44));
  // Colour bars (left): white, yellow, cyan, green, magenta, red, blue, black.
  const bars = [[255, 255, 255], [255, 255, 0], [0, 255, 255], [0, 255, 0], [255, 0, 255], [255, 0, 0], [0, 0, 255], [0, 0, 0]];
  bars.forEach((c, i) => rect(8 + i * 12, 8, 12, 24, rgb(...c)));
  // Grey ramp.
  for (let i = 0; i < 16; i++) rect(8 + i * 6, 34, 6, 6, rgb(i * 17, i * 17, i * 17));
  // Lines: horizontal, vertical, 1:1, 1:2, 2:1, 1:3.
  const line = (x0, y0, x1, y1, c) => {
    let dx = Math.abs(x1 - x0), dy = -Math.abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (;;) { set(x0, y0, c); if (x0 === x1 && y0 === y1) break; const e2 = 2 * e; if (e2 >= dy) { e += dy; x0 += sx; } if (e2 <= dx) { e += dx; y0 += sy; } }
  };
  const white = rgb(255, 255, 255), black = rgb(0, 0, 0), red = rgb(230, 40, 40), yellow = rgb(250, 220, 40);
  line(120, 10, 180, 10, white); line(120, 14, 120, 40, white);
  line(130, 14, 160, 44, white); line(166, 14, 196, 29, yellow); line(200, 14, 215, 44, yellow); line(220, 14, 250, 20, red);
  // Circle and filled triangle.
  const circle = (cx, cy, r, c) => { let x = r, y = 0, e = 1 - r; while (x >= y) { for (const [a, b] of [[x, y], [y, x], [-x, y], [-y, x], [x, -y], [y, -x], [-x, -y], [-y, -x]]) set(cx + a, cy + b, c); y++; if (e < 0) e += 2 * y + 1; else { x--; e += 2 * (y - x) + 1; } } };
  circle(60, 85, 30, white); circle(60, 85, 20, rgb(255, 200, 60));
  for (let y = 0; y < 30; y++) for (let x = 0; x <= y; x++) { set(130 + x, 75 + y, rgb(240, 120, 40)); set(130 - x, 75 + y, rgb(240, 120, 40)); }
  // Sprites, drawn from text.
  const sprite = (rows, ox, oy, pal) => rows.forEach((r, j) => [...r].forEach((ch, i) => { if (pal[ch]) set(ox + i, oy + j, pal[ch]); }));
  const heart = ['..hh...hh..', '.hrrh.hrrrh', 'hrrrrhrrrrh', 'hrrrrrrrrrh', 'hrrrrrrrrrh', '.hrrrrrrrh.', '..hrrrrrh..', '...hrrrh...', '....hrh....', '.....h.....'];
  sprite(heart.map((r) => r.replace(/^\./, '.')), 170, 62, { h: rgb(120, 0, 20), r: rgb(235, 40, 70) });
  const tree = ['....gg....', '...gGGg...', '..gGGGGg..', '.gGGgGGGg.', '..gGGGGg..', '.gGGGGGGg.', 'gGGgGGGGGg', '....bb....', '....bb....', '....bb....'];
  sprite(tree, 200, 62, { g: rgb(20, 90, 30), G: rgb(40, 150, 50), b: rgb(110, 70, 30) });
  const sword = ['.......ww', '......www', '.....www.', '.w..www..', '..www....', '..ww.....', 'bbww.....', 'bb.......'];
  sprite(sword, 224, 64, { w: rgb(220, 225, 235), b: rgb(150, 100, 40) });
  // Text, thresholded from the browser's own font so it has hard pixels.
  const text = (s, x, y, size, c) => {
    const cv = document.createElement('canvas'); cv.width = 200; cv.height = size + 4;
    const g = cv.getContext('2d', { willReadFrequently: true });
    g.fillStyle = '#fff'; g.font = `bold ${size}px monospace`; g.textBaseline = 'top'; g.fillText(s, 0, 0);
    const d = g.getImageData(0, 0, cv.width, cv.height).data;
    for (let j = 0; j < cv.height; j++) for (let i = 0; i < cv.width; i++) if (d[(j * cv.width + i) * 4 + 3] > 140) set(x + i, y + j, c);
  };
  text('FILTER TEST 0123', 20, 122, 12, white);
  text('The quick brown fox', 20, 140, 10, rgb(250, 230, 120));
  text('jumps over 13 lazy dogs.', 20, 154, 8, rgb(190, 230, 255));
  // Checker patch and fine stripes, which show moire and blurring at once.
  for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) set(250 + x, 100 + y, ((x ^ y) & 1) ? white : black);
  for (let x = 0; x < 16; x++) rect(270 + x, 100, 1, 16, (x & 1) ? white : black);
  for (let y = 0; y < 16; y++) rect(250, 120 + y, 16, 1, (y & 1) ? white : black);
  // A black bar and a white bar against the green: text-sized edges.
  rect(270, 120, 36, 6, black); rect(270, 128, 36, 3, white);
  // Frame, with a 4x4 marker in every corner.
  for (let x = 0; x < W; x++) { set(x, 0, white); set(x, H - 1, white); }
  for (let y = 0; y < H; y++) { set(0, y, white); set(W - 1, y, white); }
  rect(1, 1, 4, 4, rgb(255, 0, 0)); rect(W - 5, 1, 4, 4, rgb(0, 255, 0));
  rect(1, H - 5, 4, 4, rgb(0, 0, 255)); rect(W - 5, H - 5, 4, 4, rgb(255, 255, 0));
  return { px, W, H };
}

window.card = makeCard();
// Run one filter at a given output size and return the canvas as a PNG data URL.
window.shoot = async (id, scan, w, h, cardArg) => {
  Filters.layout(w, h, 1);
  await Filters.use(id, scan, true);
  const c = cardArg || window.card;
  Filters.feed(c.px, c.W, c.H);
  return document.getElementById('glcanvas').toDataURL('image/png');
};
window.timeIt = async (id, scan, w, h, n) => {
  Filters.capPixels = Infinity;   // measure the full-size cost, not the capped one
  Filters.layout(w, h, 1);
  await Filters.use(id, scan, true);
  Filters.feed(window.card.px, window.card.W, window.card.H);
  await Filters.time(2);
  return Filters.time(n);
};

// Where does the picture land? White squares of several sizes on black; returns the mean offset, in source
// pixels, between the brightness centroid of each square and its true centre ((0, 0) = the picture is not moved).
window.centroid = async (id, scan, w, h) => {
  const squares = [[100, 60, 8], [37, 121, 4], [200, 150, 6], [250, 30, 12], [150, 100, 5]];
  const px = new Uint32Array(320 * 200).fill(0xff000000);
  Filters.layout(w, h, 1);
  await Filters.use(id, scan, true);
  const gl = document.getElementById('glcanvas');
  const c = document.createElement('canvas'); c.width = gl.width; c.height = gl.height;
  const g = c.getContext('2d', { willReadFrequently: true });
  let ex = 0, ey = 0;
  for (const [x0, y0, n] of squares) {
    px.fill(0xff000000);
    for (let y = y0; y < y0 + n; y++) for (let x = x0; x < x0 + n; x++) px[y * 320 + x] = 0xffffffff;
    Filters.feed(px, 320, 200);
    g.drawImage(gl, 0, 0);
    const d = g.getImageData(0, 0, c.width, c.height).data;
    let sx = 0, sy = 0, sw = 0;
    for (let y = 0; y < c.height; y++) for (let x = 0; x < c.width; x++) {
      const v = d[(y * c.width + x) * 4 + 1];
      sx += (x + 0.5) * v; sy += (y + 0.5) * v; sw += v;
    }
    ex += sx / sw * 320 / c.width - (x0 + n / 2); ey += sy / sw * 200 / c.height - (y0 + n / 2);
  }
  return [ex / squares.length, ey / squares.length];
};
