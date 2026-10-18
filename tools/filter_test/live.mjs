// Drives the packaged (or dev) page in headless Chromium with a local install and server, with a display filter on,
// and reports the emulator's own ?perf=1 lines plus the filter's counters. Optionally saves a screenshot of the frame.
//   node live.mjs --url=http://127.0.0.1:2902/inn.html --install=<C: drive dir> --com1=ws://127.0.0.1:2904
//        [--filter=xbr3] [--scan] [--size=1920x1440] [--dpr=1] [--secs=40] [--shot=out.png]
// The install is the user's own copy: screenshots of it are for local inspection only and are never committed.
import { createRequire } from 'node:module';
import { writeFileSync } from 'node:fs';
const require = createRequire(import.meta.url);
const { chromium } = require(process.env.PLAYWRIGHT_CORE || '/mnt/source/mistarr/web/node_modules/playwright-core');
const chrome = process.env.CHROME || `${process.env.HOME}/.cache/ms-playwright/chromium-1243/chrome-linux64/chrome`;
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const url = arg('url'), install = arg('install'), com1 = arg('com1');
const filter = arg('filter', 'sharp'), scan = process.argv.includes('--scan');
const [vw, vh] = arg('size', '1920x1440').split('x').map(Number);
const dpr = Number(arg('dpr', '1')), secs = Number(arg('secs', '40')), shot = arg('shot', '');
const browser = await chromium.launch({ executablePath: chrome, headless: true,
  args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required'] });
const ctx = await browser.newContext({ viewport: { width: vw, height: vh }, deviceScaleFactor: dpr });
await ctx.addInitScript(([f, s]) => { try { localStorage.setItem('inn.consent', JSON.stringify({ age: 99, terms: 99 })); localStorage.setItem('inn.scale', f); localStorage.setItem('inn.scanlines', s ? '1' : '0'); } catch {} }, [filter, scan]);
const page = await ctx.newPage();
const perf = [];
page.on('console', (m) => { const t = m.text(); if (/perf:|Display filter|error/i.test(t)) { perf.push(t); console.log('[page]', t.slice(0, 300)); } });
page.on('pageerror', (e) => console.log('[pageerror]', e.message));
await page.goto(`${url}?classic=1&perf=1&com1=${encodeURIComponent(com1)}&scene=valley`);
await page.setInputFiles('#pick-dir', install);
await page.waitForFunction(() => !document.getElementById('start').disabled, null, { timeout: 60000 });
await page.click('#start');
await page.waitForFunction(() => document.body.dataset.view === 'play', null, { timeout: 120000 }).catch(() => console.log('never reached the play view'));
await page.waitForTimeout(secs * 1000);
const click = arg('click', '');
if (click) {
  const [cx, cy] = click.split(',').map(Number);
  const before = await page.evaluate(() => Filters.stats.uploads);
  await page.mouse.move(cx, cy); await page.waitForTimeout(300); await page.mouse.down(); await page.waitForTimeout(250); await page.mouse.up();
  await page.waitForTimeout(8000);
  console.log('uploads before/after click:', before, await page.evaluate(() => Filters.stats.uploads), 'focused canvas:', await page.evaluate(() => document.activeElement.id));
}
const info = await page.evaluate(() => ({ stats: Filters.stats, sizes: Filters.sizes(), active: Filters.active, shown: Filters.shown,
  gl: { w: document.getElementById('glcanvas').width, h: document.getElementById('glcanvas').height } }));
console.log('filters:', JSON.stringify(info));
if (shot) { await page.locator('#frame').screenshot({ path: shot }); console.log('saved', shot); }
await browser.close();
