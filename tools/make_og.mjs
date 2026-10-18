// Renders runtime/web/og.png, the 1200x630 link-preview image, from one of the page's own landing scenes
// (runtime/web/scenes, original artwork drawn in code). Run it again when a scene or the wording changes,
// and commit the PNG. It uses a headless Chromium through playwright-core, neither of which the repo ships.
//   PLAYWRIGHT_CORE=<dir of playwright-core> CHROME=<chrome binary> node tools/make_og.mjs [--scene harbor] [--day] [--port 2861] [--out runtime/web/og.png]
// The words on the image are generic on purpose: no site name, no game name.
import { createServer } from 'node:http';
import { readFileSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const webDir = join(here, '..', 'runtime', 'web');
const args = process.argv.slice(2);
const opt = (name, fallback) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : fallback; };
const scene = opt('--scene', 'harbor'), port = Number(opt('--port', 2861)), out = opt('--out', join(webDir, 'og.png'));
const day = args.includes('--day');
if (!process.env.PLAYWRIGHT_CORE || !process.env.CHROME) { console.error('set PLAYWRIGHT_CORE and CHROME (see the top of this file)'); process.exit(64); }
const { chromium } = createRequire(import.meta.url)(process.env.PLAYWRIGHT_CORE);

// A static server for runtime/web, so the scene code runs exactly as the page runs it.
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.json': 'application/json' };
const server = createServer((req, res) => {
  const p = normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname)).replace(/^(\.\.[/\\])+/, '');
  let body;
  try { body = readFileSync(join(webDir, p === '/' ? 'shell.html' : p)); } catch { res.writeHead(404); return res.end(); }
  res.writeHead(200, { 'Content-Type': TYPES[extname(p)] || 'application/octet-stream' });
  res.end(body);
});
await new Promise((r) => server.listen(port, '127.0.0.1', r));

const browser = await chromium.launch({ executablePath: process.env.CHROME });
try {
  const page = await (await browser.newContext({ colorScheme: day ? 'light' : 'dark', viewport: { width: 1280, height: 800 } })).newPage();
  await page.goto(`http://127.0.0.1:${port}/shell.html?scene=${scene}&classic=1`);
  await page.waitForFunction(() => typeof InnScenes !== 'undefined' && InnScenes.lastPaintMs > 0, null, { timeout: 20000 });
  const url = await page.evaluate(() => {
    const src = document.getElementById('scene-canvas');
    const W = 1200, H = 630, SCALE = 3.75, ROWS = H / SCALE, TOP = 12;   // 320x200 scene: rows TOP..TOP+168 fill the image
    const c = document.createElement('canvas');
    c.width = W; c.height = H;
    const g = c.getContext('2d');
    g.imageSmoothingEnabled = false;
    g.drawImage(src, 0, TOP, 320, ROWS, 0, 0, W, H);
    const band = g.createLinearGradient(0, H - 190, 0, H);
    band.addColorStop(0, 'rgba(10,8,28,0)');
    band.addColorStop(1, 'rgba(10,8,28,.82)');
    g.fillStyle = band;
    g.fillRect(0, H - 190, W, 190);
    g.fillStyle = '#fff6dc';
    g.shadowColor = 'rgba(0,0,0,.6)'; g.shadowOffsetX = 3; g.shadowOffsetY = 3;
    g.font = 'bold 64px ui-monospace, "Cascadia Mono", Menlo, Consolas, monospace';
    g.fillText('A 1990s online world', 56, H - 82);
    g.font = '36px ui-monospace, "Cascadia Mono", Menlo, Consolas, monospace';
    g.fillStyle = '#ffd890';
    g.fillText('playable in your browser, free of charge', 56, H - 30);
    return c.toDataURL('image/png');
  });
  writeFileSync(out, Buffer.from(url.split(',')[1], 'base64'));
  console.log(`make_og: wrote ${out}`);
} finally {
  await browser.close();
  server.close();
}
