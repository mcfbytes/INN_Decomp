// Renders the site's square emblem for sign-in providers' consent screens (Google, Discord, GitHub): the
// harbour lighthouse at dusk, its lamp blinking like a modem's TX and RX lights, painted with the landing
// scenes' own kit (InnScenes.paintSmall: paper texture, a palette of its own, ordered dither). Original art,
// no lettering, so one picture serves every provider. It is painted at 128 x 128 and scaled up by whole
// numbers for the large sizes, and painted again at the small size a provider asks for, so no size is
// resampled. Uses a headless Chromium through playwright-core, neither of which the repo ships.
//   PLAYWRIGHT_CORE=<dir of playwright-core> CHROME=<chrome binary> node tools/make_logo.mjs [--out <dir>] [--port 2863]
// Writes logo-1024.png (Discord's app icon), logo-512.png (GitHub), logo-120.png (Google) and logo-128.png.
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

// The emblem, in a 128 x 128 space (the painter scales it to the canvas). Runs in the page.
function emblem(size) {
  return InnScenes.paintSmall(size, size, true, (g, P) => {
    g.scale(size / 128, size / 128);
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
  }, 64).toDataURL('image/png');
}

const browser = await chromium.launch({ executablePath: process.env.CHROME });
try {
  const page = await (await browser.newContext({ viewport: { width: 800, height: 600 } })).newPage();
  await page.goto(`http://127.0.0.1:${port}/shell.html?scene=harbor&classic=1`);
  await page.waitForFunction(() => typeof InnScenes !== 'undefined' && InnScenes.lastPaintMs > 0, null, { timeout: 20000 });
  mkdirSync(out, { recursive: true });
  const save = (name, url) => writeFileSync(join(out, name), Buffer.from(url.split(',')[1], 'base64'));
  save('logo-128.png', await page.evaluate(emblem, 128));
  save('logo-120.png', await page.evaluate(emblem, 120));
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
  console.log(`wrote logo-128, logo-120, logo-512 and logo-1024 to ${out}`);
} finally {
  await browser.close();
  server.close();
}
