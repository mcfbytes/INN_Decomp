// Renders the test card through every display filter in headless Chromium and writes PNGs.
//   node run.mjs <out-dir> [filter-id ...] [--size=WxH] [--scan] [--time=N]
// Needs playwright-core and a Chromium (PLAYWRIGHT_CORE, CHROME in the environment; see docs/web.md).
import { createRequire } from 'node:module';
import { rmSync, copyFileSync, mkdirSync, readdirSync, writeFileSync, mkdtempSync } from 'node:fs';
import { createServer } from 'node:http';
import { readFileSync } from 'node:fs';
import { dirname, join, extname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const require = createRequire(import.meta.url);
const { chromium } = require(process.env.PLAYWRIGHT_CORE || '/mnt/source/mistarr/web/node_modules/playwright-core');
const chrome = process.env.CHROME || `${process.env.HOME}/.cache/ms-playwright/chromium-1243/chrome-linux64/chrome`;

const args = process.argv.slice(2);
const out = args.shift();
const flags = args.filter((a) => a.startsWith('--'));
const ids = args.filter((a) => !a.startsWith('--'));
const size = (flags.find((f) => f.startsWith('--size=')) || '--size=960x720').slice(7).split('x').map(Number);
const scan = flags.includes('--scan');
const timeN = Number((flags.find((f) => f.startsWith('--time=')) || '').slice(7)) || 0;
const port = Number(process.env.PORT || 2901);
const stage = process.env.STAGE || mkdtempSync(join(process.env.SCRATCH || here, 'stage-'));

mkdirSync(join(stage, 'shaders'), { recursive: true });
for (const f of ['index.html', 'card.js']) copyFileSync(join(here, f), join(stage, f));
copyFileSync(join(root, 'runtime/web/filters.js'), join(stage, 'filters.js'));
for (const d of readdirSync(join(root, 'third_party'))) {
  for (const f of readdirSync(join(root, 'third_party', d))) {
    if (/\.(glsl|png)$/.test(f)) copyFileSync(join(root, 'third_party', d, f), join(stage, 'shaders', f));
  }
}

const types = { '.html': 'text/html', '.js': 'text/javascript', '.glsl': 'text/plain', '.png': 'image/png' };
const server = createServer((req, res) => {
  const p0 = new URL(req.url, 'http://x').pathname.replace(/\/+/g, '/'); const p = p0 === '/' ? '/index.html' : p0;
  if (p.includes('..')) { res.writeHead(400).end(); return; }
  try {
    const body = readFileSync(join(stage, p));
    res.writeHead(200, { 'Content-Type': types[extname(p)] || 'application/octet-stream' }).end(body);
  } catch { res.writeHead(404).end(); }
}).listen(port, '127.0.0.1');

const browser = await chromium.launch({
  executablePath: chrome, headless: true,
  args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--enable-webgl'],
});
const page = await browser.newPage({ viewport: { width: 400, height: 300 } });
page.on('console', (m) => console.log('[page]', m.text()));
page.on('pageerror', (e) => console.log('[pageerror]', e.message));
await page.goto(`http://127.0.0.1:${port}/`);
await page.waitForFunction(() => typeof window.shoot === 'function');
const list = await page.evaluate(() => Filters.list.map((f) => f.id));
mkdirSync(out, { recursive: true });
let failed = 0;
for (const id of ids.length ? ids : list) {
  try {
    if (flags.includes('--centroid')) {
      const [cx, cy] = await page.evaluate(([i, s, w, h]) => window.centroid(i, s, w, h), [id, scan, size[0], size[1]]);
      console.log(`${id.padEnd(11)} shift ${cx.toFixed(3)}, ${cy.toFixed(3)} source px  (want 0, 0)`);
    } else if (timeN) {
      const ms = await page.evaluate(([i, s, w, h, n]) => window.timeIt(i, s, w, h, n), [id, scan, size[0], size[1], timeN]);
      console.log(`${id.padEnd(11)} ${size[0]}x${size[1]}${scan ? ' +scan' : ''}  ${ms.toFixed(2)} ms/frame`);
    } else {
      const url = await page.evaluate(([i, s, w, h]) => window.shoot(i, s, w, h), [id, scan, size[0], size[1]]);
      writeFileSync(join(out, `${id}${scan ? '-scan' : ''}.png`), Buffer.from(url.split(',')[1], 'base64'));
      console.log('ok  ', id);
    }
  } catch (e) { failed++; console.log('FAIL', id, String(e.message || e).slice(0, 600)); }
}
await browser.close();
server.close();
if (!process.env.STAGE) rmSync(stage, { recursive: true, force: true });
process.exit(failed ? 1 : 0);
