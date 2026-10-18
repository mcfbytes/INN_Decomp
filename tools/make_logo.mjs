// Renders the site's square emblem for sign-in providers' consent screens (Google, Discord, GitHub): the
// harbour lighthouse at dusk, its lamp blinking like a modem's TX and RX lights, painted with the landing
// scenes' own kit (InnScenes.paintSmall: paper texture, a palette of its own, ordered dither). Original art,
// no lettering, so one picture serves every provider. It is painted at 128 x 128 and scaled up by whole
// numbers for the large sizes, and painted again at the small size a provider asks for, so no size is
// resampled. Uses a headless Chromium through playwright-core, neither of which the repo ships.
//   PLAYWRIGHT_CORE=<dir of playwright-core> CHROME=<chrome binary> node tools/make_logo.mjs
//     [--subject lighthouse|valley] [--time dusk|day] [--out <dir>] [--port 2863]
// Writes logo-1024.png (Discord's app icon), logo-512.png (GitHub), logo-120.png (Google) and logo-128.png.
// The lighthouse is painted at dusk only; the valley (the autumn town under its volcano) by day or at dusk.
import { createServer } from 'node:http';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const webDir = join(here, '..', 'runtime', 'web');
const args = process.argv.slice(2);
const opt = (name, fallback) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : fallback; };
const out = opt('--out', '.'), port = Number(opt('--port', 2863));
const subject = opt('--subject', 'lighthouse'), time = opt('--time', subject === 'lighthouse' ? 'dusk' : 'day');
if (!process.env.PLAYWRIGHT_CORE || !process.env.CHROME) { console.error('set PLAYWRIGHT_CORE and CHROME (see the top of this file)'); process.exit(64); }
const { chromium } = createRequire(import.meta.url)(process.env.PLAYWRIGHT_CORE);

const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.json': 'application/json' };
const server = createServer((req, res) => {
  const p = normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname)).replace(/^(\.\.[/\\])+/, '');
  let body;
  try { body = readFileSync(join(webDir, p === '/' ? 'shell.html' : p)); } catch { res.writeHead(404); return res.end(); }
  res.writeHead(200, { 'Content-Type': TYPES[extname(p)] || 'application/octet-stream' });
  res.end(body);
});
await new Promise((r) => server.listen(port, '127.0.0.1', r));

// The emblem, in a 128 x 128 space (the painter scales it to the canvas). Runs in the page; everything it
// uses is defined inside it.
function emblem([subject, time, size]) {
  const dusk = time === 'dusk';
  return InnScenes.paintSmall(size, size, dusk, (g, P) => {
    g.scale(size / 128, size / 128);
    if (subject === 'valley') valley(g, P);
    else lighthouse(g, P);
  }, 64).toDataURL('image/png');

  // The small painter's kit is the day one; grade it to dusk here, as the scenes' own kit does.
  function grade(P) {
    if (!dusk) return;
    const hex2 = (h) => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16));
    const css = (r, g, b) => `rgb(${Math.round(r)},${Math.round(g)},${Math.round(b)})`;
    const cl = (v) => Math.max(0, Math.min(255, v));
    P.col = (hex, night) => {
      if (hex.charAt(0) !== '#') return hex;
      if (night) return night;
      const [r, g, b] = hex2(hex);
      return css(cl(r * 0.34 + 20), cl(g * 0.36 + 22), cl(b * 0.54 + 54));
    };
    P.pick = (day, night) => night;
  }

  function lighthouse(g, P) {
    const HORIZON = 88, cx = 70, top = 40, base = 112, lamp = { x: cx, y: top - 9 };
    // sky: night overhead, the last of the sunset low down
    g.fillStyle = P.gradRaw(g, 0, 0, 0, HORIZON + 2, [[0, '#120c3c'], [0.38, '#3c2478'], [0.78, '#d0507c'], [1, '#ffb064']]);
    g.fillRect(0, 0, 128, HORIZON + 2);
    P.stars(g, 0, 48, 34, 9);
    P.glow(g, 22, HORIZON, 70, '#ffb060', 0.5);
    P.blob(g, 20, 76, 6, 6, ['#fff4d0', '#ffd890'], { wob: 0.3, seed: 3, n: 14 });
    P.glow(g, 20, 76, 18, '#ffd890', 0.6);
    P.cloud(g, 84, 14, 1.0, 5);
    // sea, with the sun's path across it
    g.fillStyle = P.gradRaw(g, 0, HORIZON, 0, 128, [[0, '#f0905c'], [0.22, '#a8487c'], [0.6, '#3c2a78'], [1, '#160e48']]);
    g.fillRect(0, HORIZON, 128, 40);
    g.save();
    g.globalCompositeOperation = 'lighter';
    g.fillStyle = P.gradRaw(g, 0, HORIZON, 0, 128, [[0, P.alpha('#ff9a50', 0.5)], [1, P.alpha('#ff9a50', 0)]]);
    g.beginPath(); g.moveTo(14, HORIZON); g.lineTo(28, HORIZON); g.lineTo(52, 128); g.lineTo(0, 128); g.closePath(); g.fill();
    g.restore();
    const r = P.mulberry(7);
    for (let i = 0; i < 60; i++) {
      const y = HORIZON + 2 + Math.pow(r(), 1.5) * 38, x = r() * 140 - 6, len = 3 + (y - HORIZON) * 0.3 * r();
      g.fillStyle = P.alpha(r() < 0.5 ? '#ffa080' : '#0c0830', 0.24);
      g.fillRect(x, y, len, 1);
    }
    // the rock the lighthouse stands on, and two lit cottages at its foot
    const rock = [[30, 128], [36, 116], [46, 108], [60, 104], [84, 103], [100, 108], [112, 118], [118, 128]];
    P.shape(g, rock, ['#6c6078', '#3c3450', '#1c1830'], { wob: 1.2, seed: 4, edge: '#100c20', ea: 0.5 });
    P.within(g, rock, { wob: 1.2, seed: 4 }, () => {
      P.dabs(g, 30, 102, 88, 26, ['#847890', '#2c2440'], 40, 2.5, 6, 0.35);
      P.dabs(g, 30, 102, 88, 26, ['#3c5c34'], 10, 2, 7, 0.5);
    });
    for (const [x, y, w, h, roof, seed] of [[38, 118, 10, 7, '#7c2820', 17], [98, 116, 9, 7, '#342868', 19]]) {
      P.rect(g, x, y - h, w, h, ['#5c4c5c', '#3c2c40'], { wob: 0.4, seed, edge: '#100810', ea: 0.4 });
      P.shape(g, [[x - 2, y - h + 1], [x + w / 2, y - h - 5], [x + w + 2, y - h + 1]], roof, { wob: 0.4, seed: seed + 1, edge: '#100810', ea: 0.5 });
      g.fillStyle = P.lit('#ffd24a'); g.fillRect(x + 2, y - h + 2, 3, 3);
      P.glow(g, x + 3.5, y - h + 3.5, 7, '#ffb030', 0.5);
    }
    // the tower (the harbour scene's, a little stouter), dusk-graded by hand
    const hw = (y) => 6 + (y - top) * 0.07;
    P.shadow(g, cx + 2, base + 1, 14, 2.5, 0.35);
    const pts = [[cx - hw(base), base], [cx - hw(top), top], [cx + hw(top), top], [cx + hw(base), base]];
    P.shape(g, pts, ['#c8c4dc', '#8c88b0', '#4c4470'], { wob: 0.4, seed: 41, edge: '#140c28', ea: 0.6 });
    P.within(g, pts, { wob: 0.4, seed: 41 }, () => {
      for (let i = 0; i < 4; i++) {
        const y0 = top + 7 + i * 18;
        g.fillStyle = '#8c2c3c'; g.fillRect(cx - 20, y0, 40, 8);
        g.fillStyle = P.alpha('#ffffff', 0.18); g.fillRect(cx - 20, y0, 40, 1.2);
      }
      g.fillStyle = P.alpha('#0c0620', 0.36); g.fillRect(cx + 2, top, 14, base - top);
      P.dabs(g, cx - 10, top, 20, base - top, ['#5c5480'], 12, 1.3, 5, 0.2);
    });
    P.shape(g, [[cx - 2.5, base], [cx - 2.5, base - 7], [cx, base - 9], [cx + 2.5, base - 7], [cx + 2.5, base]], '#24140c', { wob: 0.3, seed: 42 });
    P.blob(g, cx, top + 22, 1.8, 1.8, P.lit('#ffd24a'), { wob: 0.1, seed: 5, n: 8, edge: '#140c08', ea: 0.9 });
    P.glow(g, cx, top + 22, 6, '#ffb030', 0.5);
    // gallery, lamp room and dome
    P.shape(g, [[cx - 11, top + 0.5], [cx + 11, top + 0.5], [cx + 10, top + 3.5], [cx - 10, top + 3.5]], ['#3c3458', '#1c1430'], { wob: 0.3, seed: 43 });
    for (let x = cx - 9; x <= cx + 9; x += 4.5) P.rect(g, x, top - 4, 0.9, 4.4, '#1c1430', { wob: 0.1 });
    P.rect(g, cx - 10, top - 4.4, 20, 1, '#1c1430', { wob: 0.2, seed: 44 });
    P.shape(g, [[cx - 7, top - 4], [cx - 7, top - 14], [cx + 7, top - 14], [cx + 7, top - 4]], '#2c2848', { wob: 0.3, seed: 45, edge: '#140c28', ea: 0.9 });
    P.shape(g, [[cx - 9, top - 14], [cx - 5, top - 20], [cx, top - 23], [cx + 5, top - 20], [cx + 9, top - 14]], ['#b0404c', '#5c1424'], { wob: 0.4, seed: 46, edge: '#1c040c', ea: 0.5 });
    P.line(g, [[cx, top - 23], [cx, top - 29]], '#1c1430', 1);
    P.blob(g, cx, top - 29.6, 1.4, 1.4, P.lit('#ffd23c'), { wob: 0.1, seed: 4, n: 8 });
    // the lamp: TX green and RX amber, each throwing its own beam out to sea
    const wedge = (hex, dy, alpha) => {
      g.save();
      g.globalCompositeOperation = 'lighter';
      g.fillStyle = P.gradRaw(g, lamp.x, 0, 0, 0, [[0, P.alpha(hex, alpha)], [1, P.alpha(hex, 0)]]);
      g.beginPath(); g.moveTo(lamp.x - 2, lamp.y - 1); g.lineTo(-4, lamp.y - 26 + dy); g.lineTo(-4, lamp.y + 16 + dy); g.lineTo(lamp.x - 2, lamp.y + 3); g.closePath(); g.fill();
      g.restore();
    };
    wedge('#5cff8c', -10, 0.5);
    wedge('#ffb840', 10, 0.42);
    g.fillStyle = P.lit('#5cff8c'); g.fillRect(lamp.x - 5, lamp.y - 1, 4, 4);
    g.fillStyle = P.lit('#ffb840'); g.fillRect(lamp.x + 1, lamp.y - 1, 4, 4);
    P.glow(g, lamp.x - 3, lamp.y + 1, 14, '#5cff8c', 0.75);
    P.glow(g, lamp.x + 3, lamp.y + 1, 14, '#ffb840', 0.65);
  }

  // The valley scene's town, church and volcano (runtime/web/scenes/valley.js), without its telephone poles,
  // framed on the scene's x 96..276, y 8..188 so the plume, the spire and the road all fit.
  function valley(g, P) {
    grade(P);
    const AUTUMN = {
      orange: ['#8a3a10', '#d8641c', '#f6a84a'], red: ['#6c1a14', '#b02c1c', '#ec6840'],
      gold: ['#7a5a10', '#d0a028', '#f6e078'], pine: ['#10301e', '#1e4a2c', '#3c7040'],
    };
    g.scale(128 / 180, 128 / 180);
    g.translate(-96, -8);
    // sky
    g.fillStyle = P.gradRaw(g, 0, 0, 0, 125, P.pick(
      [[0, '#4a92dc'], [0.4, '#8cc4ee'], [0.78, '#f4e6bc'], [1, '#fbd49a']],
      [[0, '#15123e'], [0.4, '#3c2870'], [0.75, '#c8587a'], [1, '#f6a05a']]));
    g.fillRect(0, 0, 320, 125);
    if (P.dusk) {
      P.grain(g, 96, 8, 180, 62, '#ffffff', 50, 4, 0.9);
      P.glow(g, 128, 34, 34, '#c8c0ff', 0.3);
      P.blob(g, 128, 34, 8, 8, ['#fffbe8', '#d4cca8'], { wob: 0.4, seed: 2, n: 16 });
      P.dabs(g, 123, 29, 9, 9, ['#b8b090'], 6, 1.4, 5, 0.5);
    } else {
      P.glow(g, 122, 52, 80, '#fff6c8', 0.55);
      P.glow(g, 122, 52, 30, '#ffffff', 0.5);
      P.cloud(g, 112, 24, 1.4, 5);
    }
    // the far range and the volcano, its lava and its plume
    P.ridge(g, { y: 104, amp: 9, freq: 0.03, seed: 4, bottom: 130, cols: ['#a2b0da', '#c4c8dc'] });
    const body = [[168, 124], [196, 94], [214, 72], [226, 56], [236, 52], [247, 57], [258, 74], [276, 98], [312, 124]];
    P.shape(g, body, ['#b08a94', '#7c5a6c'], { wob: 1.4, seed: 3 });
    P.within(g, body, { wob: 1.4, seed: 3 }, () => {
      g.fillStyle = P.alpha('#2c1a3c', 0.5);
      g.beginPath(); g.moveTo(236, 51); g.lineTo(260, 76); g.lineTo(316, 126); g.lineTo(244, 126); g.lineTo(240, 90); g.closePath(); g.fill();
      P.dabs(g, 170, 56, 150, 70, ['#d8b8a0', '#a08090', '#8a6a7c'], 70, 4, 21, 0.28);
      for (const [x0, y0, x1, y1] of [[226, 60, 212, 112], [233, 56, 232, 118], [243, 60, 256, 116], [220, 70, 196, 118], [252, 68, 280, 118]])
        P.line(g, [[x0, y0], [(x0 + x1) / 2 + 3, (y0 + y1) / 2], [x1, y1]], P.alpha('#3a2444', 0.4), 2, { wob: 2, seed: x0 });
      g.fillStyle = P.gradRaw(g, 0, 90, 0, 126, [[0, P.alpha(P.pick('#f6e4c0', '#c8587a'), 0)], [1, P.alpha(P.pick('#f6e4c0', '#c8587a'), 0.75)]]);
      g.fillRect(150, 86, 180, 44);
    });
    P.shape(g, [[226, 56], [236, 52], [247, 57], [240, 60], [231, 60]], P.pick('#4a3040', '#2a1c34'), { wob: 0.8, seed: 5 });
    P.blob(g, 237, 56, 6, 1.8, P.lit('#ff8a2c'), { wob: 0.5, seed: 2 });
    P.glow(g, 237, 56, 22, '#ff7a24', P.dusk ? 0.85 : 0.5);
    P.line(g, [[236, 58], [234, 68], [237, 78], [233, 90]], P.lit('#ff7a24'), 2, { wob: 1.5, seed: 6 });
    for (const [x, y, r] of [[238, 46, 6], [234, 38, 7.5], [240, 29, 8], [247, 21, 9], [257, 15, 9], [268, 10, 8]]) {
      P.blob(g, x, y, r, r * 0.8, [P.pick('#fffaf0', '#8a7aa8'), P.pick('#cdbed0', '#4c3c78')], { wob: 1.5, seed: x });
    }
    // hills and their trees
    P.ridge(g, { y: 118, amp: 7, freq: 0.022, seed: 12, bottom: 140, cols: ['#b8b068', '#8a9a48'] });
    P.ridge(g, { y: 128, amp: 5, freq: 0.03, seed: 21, bottom: 160, cols: ['#a6a838', '#6c8c2c'] });
    const cols = [AUTUMN.orange, AUTUMN.gold, AUTUMN.red, AUTUMN.pine];
    for (let i = 5; i < 16; i++) {
      const x = 8 + i * 18 + (i % 3) * 3, y = 122 + Math.sin(i * 1.7) * 3;
      P.canopy(g, x, y, 5 + (i % 3), 4 + (i % 2), cols[i % 4].map((c) => P.col(c)), 40 + i);
    }
    const windowAt = (x, y, w, h) => {
      if (P.dusk) { g.fillStyle = P.lit('#ffd24a'); g.fillRect(x, y, w, h); P.glow(g, x + w / 2, y + h / 2, 9, '#ffb830', 0.5); }
      else { g.fillStyle = P.col('#9ec8dc'); g.fillRect(x, y, w, h); g.fillStyle = P.alpha('#ffffff', 0.4); g.fillRect(x, y, w, 1); }
      g.fillStyle = P.col('#4a2c20');
      g.fillRect(x + Math.floor(w / 2), y, 1, h);
      g.fillRect(x, y + Math.floor(h / 2), w, 1);
    };
    const cottage = (x, y, w, h, wall, roof, seed) => {
      P.shape(g, [[x, y - h], [x + w, y - h], [x + w, y], [x, y]], wall, { wob: 0.6, seed, edge: '#5a3a28', ea: 0.3 });
      P.within(g, [[x, y - h], [x + w, y - h], [x + w, y], [x, y]], { wob: 0.6, seed }, () => {
        g.fillStyle = P.alpha(P.col('#3a2418'), 0.25);
        g.fillRect(x + w * 0.62, y - h, w * 0.4, h);
      });
      const ov = 3, rf = [[x - ov, y - h + 1], [x + w / 2, y - h - h * 0.6], [x + w + ov, y - h + 1]];
      P.shape(g, rf, roof, { wob: 0.7, seed: seed + 1, edge: '#2c1410', ea: 0.35 });
      P.rect(g, x + w * 0.66, y - h - h * 0.72, 3, h * 0.45, '#8c4838', { wob: 0.4, seed });
      windowAt(x + 3, y - h + 3, 4, 4);
      if (w > 16) windowAt(x + w - 8, y - h + 3, 4, 4);
      P.rect(g, x + w / 2 - 2, y - 7, 4, 7, '#3a2418', { wob: 0.3, seed });
      if (P.dusk) P.glow(g, x + w / 2, y - 2, 8, '#ffb830', 0.18);
    };
    const pine = (x, y, h, seed) => {
      const [dk, md, lt] = AUTUMN.pine.map((c) => P.col(c));
      P.rect(g, x - 1, y - 3, 2, 4, '#3a2418', { wob: 0.2, seed });
      for (let i = 0; i < 4; i++) {
        const ty = y - 3 - i * h * 0.22, w = (h * 0.28) * (1 - i * 0.18);
        P.shape(g, [[x - w, ty], [x, ty - h * 0.3], [x + w, ty]], [lt, md, dk], { wob: 0.8, seed: seed + i });
      }
    };
    // the church and its clock spire
    const wall = ['#f6e8c0', '#cdb68a'];
    P.shape(g, [[138, 134], [138, 112], [168, 112], [168, 134]], wall, { wob: 0.6, seed: 31, edge: '#5a3a28', ea: 0.3 });
    P.shape(g, [[136, 113], [153, 100], [170, 113]], '#9c3a2c', { wob: 0.7, seed: 32, edge: '#2c1410', ea: 0.35 });
    P.shape(g, [[165, 134], [165, 90], [183, 90], [183, 134]], wall, { wob: 0.6, seed: 33, edge: '#5a3a28', ea: 0.3 });
    g.fillStyle = P.alpha(P.col('#3a2418'), 0.22);
    g.fillRect(176, 90, 7, 44);
    P.shape(g, [[162, 91], [174, 58], [186, 91]], ['#c4503c', '#7c2a20'], { wob: 0.7, seed: 34, edge: '#2c1410', ea: 0.4 });
    P.line(g, [[174, 58], [174, 50]], '#2c1c18', 1);
    P.shape(g, [[172, 51], [177, 51], [175, 53]], P.pick('#e8b830', '#c8a850'), { wob: 0.2, seed: 1 });
    P.blob(g, 174, 100, 4.6, 4.6, P.lit(P.pick('#fff6d8', '#ffe8a0')), { wob: 0.3, seed: 2, edge: '#3a2418', ea: 0.7, n: 12 });
    if (P.dusk) P.glow(g, 174, 100, 12, '#ffe8a0', 0.4);
    g.strokeStyle = P.col('#2c1c18'); g.lineWidth = 1;
    g.beginPath(); g.moveTo(174, 100); g.lineTo(174, 97); g.moveTo(174, 100); g.lineTo(176.5, 101); g.stroke();
    P.shape(g, [[171, 120], [171, 111], [172.5, 109], [174, 111], [174, 120]], '#2c1c18', { wob: 0.2, seed: 3 });
    P.rect(g, 148, 124, 5, 10, '#3a2418', { wob: 0.3, seed: 4 });
    windowAt(142, 118, 4, 5);
    windowAt(158, 118, 4, 5);
    // cottages, trees among them
    cottage(112, 142, 18, 12, ['#f2dcae', '#cdb084'], ['#b8503a', '#80281c'], 41);
    cottage(192, 140, 20, 13, ['#e6d4aa', '#bfa57c'], ['#3c5c8c', '#26386c'], 51);
    cottage(216, 146, 14, 10, ['#f0d0a4', '#c8a47a'], ['#9c3a2c', '#64261c'], 61);
    cottage(252, 138, 15, 10, ['#f4e2b6', '#d0b88c'], ['#b8503a', '#80281c'], 81);
    P.rect(g, 133, 138, 2, 7, '#3a2418', { wob: 0.2 });
    P.canopy(g, 134, 133, 8, 9, AUTUMN.orange.map((c) => P.col(c)), 91);
    P.rect(g, 187, 140, 2, 8, '#3a2418', { wob: 0.2 });
    P.canopy(g, 188, 134, 9, 9, AUTUMN.gold.map((c) => P.col(c)), 95);
    P.rect(g, 236, 142, 2, 8, '#3a2418', { wob: 0.2 });
    P.canopy(g, 237, 136, 9, 10, AUTUMN.red.map((c) => P.col(c)), 99);
    pine(104, 150, 22, 111);
    pine(270, 142, 20, 119);
    // the meadow and the road down from the village
    P.ridge(g, { y: 154, amp: 3, freq: 0.04, seed: 31, bottom: 200, cols: ['#92a634', '#5c7c28', '#3e6020'] });
    P.dabs(g, 96, 156, 180, 44, ['#c8a428', '#e0742c', '#3a5c1c', '#7c9a30'], 50, 3, 33, 0.35);
    const road = [[168, 150], [176, 150], [188, 162], [214, 182], [246, 200], [110, 200], [136, 184], [158, 164]];
    P.shape(g, road, ['#ecd49c', '#bf9e68'], { wob: 1.6, seed: 34, edge: '#6c4a2c', ea: 0.25 });
    P.within(g, road, { wob: 1.6, seed: 34 }, () => {
      P.dabs(g, 100, 150, 160, 50, ['#a8845a', '#f6e4b4', '#8c6a44'], 70, 2.2, 36, 0.35);
    });
    P.dabs(g, 96, 160, 180, 40, ['#e0742c', '#b02c1c', '#f0b030'], 24, 1.2, 77, 0.9);
    P.canopy(g, 104, 186, 16, 12, AUTUMN.gold.map((c) => P.col(c)), 140);
    P.canopy(g, 268, 190, 18, 12, AUTUMN.orange.map((c) => P.col(c)), 150);
  }
}

const browser = await chromium.launch({ executablePath: process.env.CHROME });
try {
  const page = await (await browser.newContext({ viewport: { width: 800, height: 600 } })).newPage();
  await page.goto(`http://127.0.0.1:${port}/shell.html?scene=harbor&classic=1`);
  await page.waitForFunction(() => typeof InnScenes !== 'undefined' && InnScenes.lastPaintMs > 0, null, { timeout: 20000 });
  mkdirSync(out, { recursive: true });
  const save = (name, url) => writeFileSync(join(out, name), Buffer.from(url.split(',')[1], 'base64'));
  save('logo-128.png', await page.evaluate(emblem, [subject, time, 128]));
  save('logo-120.png', await page.evaluate(emblem, [subject, time, 120]));
  for (const k of [4, 8]) {
    // whole-number scale-ups of the 128 painting, pixels kept square
    save(`logo-${128 * k}.png`, await page.evaluate(([src, n]) => new Promise((resolve) => {
      const img = new Image();
      img.onload = () => {
        const c = document.createElement('canvas');
        c.width = c.height = 128 * n;
        const g = c.getContext('2d');
        g.imageSmoothingEnabled = false;
        g.drawImage(img, 0, 0, c.width, c.height);
        resolve(c.toDataURL('image/png'));
      };
      img.src = src;
    }), [`data:image/png;base64,${readFileSync(join(out, 'logo-128.png')).toString('base64')}`, k]));
  }
  // --sizes 180,192,...: further native paintings (the page's touch and app icons)
  for (const n of (opt('--sizes', '') || '').split(',').filter(Boolean).map(Number)) {
    save(`logo-${n}.png`, await page.evaluate(emblem, [subject, time, n]));
  }
  console.log(`wrote logo-128, logo-120, logo-512 and logo-1024 to ${out}`);
} finally {
  await browser.close();
  server.close();
}
