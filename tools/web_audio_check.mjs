// Headless-browser check of the Sound Blaster output of a web build (docs/web.md, "Sound"). It serves a build
// directory, opens it with ?autostart=1&audiotap=1, and reads window.innAudioTap (the runtime's per-source
// "audio: tap" counters plus what the AudioWorklet received). It asserts that
//   FM:      after logon the OPL source is non-silent and the worklet received sound while the DSP was silent;
//   digital: SierraLand -> 3-D Golf, driven with the click times of a scenario file read at run time, makes the
//            SB DSP source non-silent and the worklet receives it.
// The build must be made with -DINN_WEB_PRELOAD_DIR=<a C: drive whose LSCI.CFG names sndblast.drv>; such a build
// holds media and is never packaged or published. GOLF.EXE under INN starts on its PC speaker device, so the drive's
// copy has the device word set to the Sound Blaster (docs/web.md, "Sound"). A server must already listen on --com1,
// started fresh (a persona from an earlier run changes the scenario's screens).
//   PLAYWRIGHT_CORE=<dir of playwright-core> CHROME=<chrome binary> node tools/web_audio_check.mjs <build dir>
//     [--port 39082] [--com1 ws://127.0.0.1:39081] [--mode all|fm|dsp|off] [--page inn.html]
//     [--scenario <inn_sierraland.txt>] [--lead 28] [--fm-wait 120] [--min-peak 200] [--log out.log]
// --mode off opens the page without ?audiotap=1 and asserts that nothing taps (no window.innAudioTap, no "audio: tap"
// line) while the emulator still runs at real speed (median "perf: speed" within 0.99-1.01).
// Exit code 0 when every assertion held, 1 when one failed, 64 for a usage error.
import { createServer } from 'node:http';
import { readFileSync, appendFileSync, writeFileSync, existsSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const opt = (name, fallback) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : fallback; };
const buildDir = args.find((a, i) => !a.startsWith('--') && !(i > 0 && args[i - 1].startsWith('--')));
const port = Number(opt('--port', 39082));
const com1 = opt('--com1', `ws://127.0.0.1:${port - 1}`);
const mode = opt('--mode', 'all');
const pageName = opt('--page', 'inn.html');
const scenarioPath = opt('--scenario', join(here, '..', 'private', 'lifted', 'scenarios', 'inn_sierraland.txt'));
const leadS = Number(opt('--lead', 28));       // seconds after the play view until the land directory takes the first click
const fmWaitS = Number(opt('--fm-wait', 120)); // how long FM may stay silent after the play view
const minPeak = Number(opt('--min-peak', 200)); // int16 peak that counts as sound (not a decaying tail)
const logPath = opt('--log', '');
if (!buildDir || !['all', 'fm', 'dsp', 'off'].includes(mode)) { console.error('usage: web_audio_check.mjs <build dir> [--port N] [--mode all|fm|dsp] ...'); process.exit(64); }
if (!process.env.PLAYWRIGHT_CORE || !process.env.CHROME) { console.error('set PLAYWRIGHT_CORE and CHROME (see the top of this file)'); process.exit(64); }
const { chromium } = createRequire(import.meta.url)(process.env.PLAYWRIGHT_CORE);
const out = (s) => { console.log(s); if (logPath) appendFileSync(logPath, s + '\n'); };
if (logPath) writeFileSync(logPath, '');

// ---- static server for the build directory ----
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.json': 'application/json', '.wasm': 'application/wasm',
  '.css': 'text/css', '.png': 'image/png', '.svg': 'image/svg+xml', '.ico': 'image/x-icon', '.woff2': 'font/woff2',
  '.webmanifest': 'application/manifest+json' };
const server = createServer((req, res) => {
  const p = normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname)).replace(/^(\.\.[/\\])+/, '');
  let body;
  try { body = readFileSync(join(buildDir, p === '/' ? pageName : p)); } catch { res.writeHead(404); return res.end(); }
  res.writeHead(200, { 'Content-Type': TYPES[extname(p)] || 'application/octet-stream' });
  res.end(body);
});
await new Promise((r) => server.listen(port, '127.0.0.1', r));

// ---- the scenario's input events (times read from the file, not copied) ----
const SET1 = { '01': 'Escape', '0f': 'Tab', '2d': 'x', '15': 'y', '38': 'Alt' };
function loadScenario(path) {
  const ev = [];
  let golfNext = false;
  for (const raw of readFileSync(path, 'utf8').split('\n')) {
    const line = raw.trim();
    if (!line) continue;
    if (line[0] === '#') { if (/^#\s*3-D Golf/i.test(line)) golfNext = true; continue; }
    const m = /^(\d+)\s+(\w+)\s*(.*)$/.exec(line);
    if (!m) continue;
    const [, ms, verb, rest] = m;
    const e = { ms: Number(ms), verb, rest };
    if (verb === 'click' || verb === 'move') [e.x, e.y] = rest.split(/\s+/).map(Number);
    if (golfNext && verb !== 'shot') { e.golf = true; golfNext = false; }
    if (verb !== 'shot') ev.push(e);
  }
  return ev;
}

const browser = await chromium.launch({ executablePath: process.env.CHROME, headless: true,
  args: ['--autoplay-policy=no-user-gesture-required', '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
let failed = 0;
const check = (ok, what) => { out(`${ok ? 'PASS' : 'FAIL'}: ${what}`); if (!ok) failed++; };
try {
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 960 } });
  await ctx.addInitScript(() => { try { localStorage.setItem('inn.consent', JSON.stringify({ age: 99, terms: 99 })); } catch {} });
  const page = await ctx.newPage();
  const tapLines = [], perfSpeeds = [];
  page.on('console', (m) => { const t = m.text(); if (/^audio: tap/.test(t)) tapLines.push(t);
    else if (/perf: speed/.test(t)) { perfSpeeds.push(Number(/speed ([\d.]+)x/.exec(t)[1])); out(`[page] ${t.slice(0, 160)}`); }
    else if (/error/i.test(t)) out(`[page] ${t.slice(0, 200)}`); });
  page.on('pageerror', (e) => out(`[pageerror] ${e.message}`));
  await page.goto(`http://127.0.0.1:${port}/${pageName}?autostart=1&${mode === 'off' ? '' : 'audiotap=1&'}perf=1&classic=1&com1=${encodeURIComponent(com1)}`);
  if (mode !== 'off') await page.waitForFunction(() => !!window.innAudioTap, null, { timeout: 30000 });
  await page.waitForFunction(() => document.body.dataset.view === 'play', null, { timeout: 180000 });
  const playAt = Date.now();
  out('web_audio_check: play view reached');

  const snap = () => page.evaluate(() => JSON.parse(JSON.stringify({ ...window.innAudioTap })));
  const show = (s, label) => out(`tap ${label}: t=${s.t} ` + Object.entries(s.sources).map(([k, v]) => `${k}{peak=${v.peak} max=${v.maxPeak} frames=${v.totalFrames}}`).join(' ')
    + (s.worklet ? ` worklet{received=${s.worklet.received} peak=${s.worklet.peak} sec=${s.worklet.peakSec} msgs=${s.worklet.messages}}` : ' worklet{none}'));
  const sinceMs = () => Date.now() - playAt;

  if (mode === 'off') {
    await page.waitForTimeout((leadS + 60) * 1000);
    check(await page.evaluate(() => window.innAudioTap === undefined), 'tap off: window.innAudioTap does not exist');
    check(tapLines.length === 0, `tap off: no "audio: tap" line was logged (${tapLines.length})`);
    // The first windows are the dial and the load; a loaded host adds the odd dip. The median is the speed.
    const sorted = perfSpeeds.slice(2).sort((x, y) => x - y), median = sorted[sorted.length >> 1];
    check(sorted.length >= 5 && median >= 0.99 && median <= 1.01, `tap off: median perf speed ${median} within 0.99-1.01 (${perfSpeeds.join(' ')})`);
    out(failed ? `web_audio_check: ${failed} assertion(s) failed` : 'web_audio_check: all assertions held');
    await browser.close(); server.close(); process.exit(failed ? 1 : 0);
  }
  // One sampler runs through the whole session. FM: after logon (the land directory is up, leadS after the play view)
  // the OPL source and the worklet are non-silent in a second where the DSP is silent. Digital: from the scenario's
  // 3-D Golf comment on, the SB DSP source and the worklet are non-silent in the same second.
  const ev = mode === 'fm' && !args.includes('--scenario') ? [] : loadScenario(scenarioPath);
  const first = ev.length ? ev.find((e) => e.verb === 'click').ms : 0;
  const box = async () => page.locator('#canvas').boundingBox();
  const toPx = async (x, y) => { const b = await box(); return [b.x + (x + 0.5) * b.width / 320, b.y + (y + 0.5) * b.height / 200]; };
  await page.waitForTimeout(leadS * 1000);
  const logon = await snap();
  show(logon, 'logon');
  const startAt = Date.now();
  let fm = null, digital = null, golfStarted = false, golfLoud = new Map(), lastLog = 0, lastLoud = '';
  const sample = async () => {
    const s = await snap();
    if (!s.worklet) return;
    const loud = ['opl', 'sb', 'speaker'].filter((k) => s.sources[k] && s.sources[k].peak >= minPeak).join('+');
    if (loud !== lastLoud || Date.now() - lastLog > 60000) { lastLog = Date.now(); lastLoud = loud; show(s, loud ? `loud ${loud}` : 'quiet'); }
    const opl = s.sources.opl, sb = s.sources.sb;
    if (!fm && opl && opl.peak >= minPeak && s.worklet.peakSec >= minPeak && (!sb || sb.peak < minPeak)) fm = s;
    // The DSP's own start-up click is one loud second; sound is at least two separate seconds of it while 3-D Golf runs.
    if (golfStarted && sb && sb.peak >= minPeak && s.worklet.peakSec >= minPeak && !golfLoud.has(s.t)) golfLoud.set(s.t, s);
    if (!digital && golfLoud.size >= 2) digital = s;
  };
  const wait = async (until, done) => {
    while (Date.now() < until && !done()) { await page.waitForTimeout(Math.min(500, until - Date.now()) | 0 || 1); await sample(); }
  };
  const wantFm = mode !== 'dsp', wantDsp = mode !== 'fm';
  if (!ev.length) await wait(startAt + fmWaitS * 1000, () => !!fm);
  for (const e of ev) {
    if (e.golf) { golfStarted = true; show(await snap(), 'before 3-D Golf'); }
    await wait(startAt + (e.ms - first), () => false);
    if (e.verb === 'click') {
      const [px, py] = await toPx(e.x, e.y);
      await page.mouse.move(px, py); await page.waitForTimeout(100);
      await page.mouse.down(); await page.waitForTimeout(200); await page.mouse.up();
    } else if (e.verb === 'move') {
      const [px, py] = await toPx(e.x, e.y); await page.mouse.move(px, py);
    } else if (e.verb === 'type') {
      await page.keyboard.type(e.rest, { delay: 120 });
    } else if (e.verb === 'key' || e.verb === 'down' || e.verb === 'up') {
      const k = SET1[e.rest.trim().toLowerCase()];
      if (!k) { out(`web_audio_check: scenario key ${e.rest} has no mapping`); continue; }
      if (e.verb === 'key') { await page.keyboard.down(k); await page.waitForTimeout(60); await page.keyboard.up(k); }
      else if (e.verb === 'down') await page.keyboard.down(k); else await page.keyboard.up(k);
    }
    if (mode === 'fm' && fm) break;
  }
  await wait(Date.now() + 3000, () => false);
  const end = await snap();
  show(end, 'end');

  if (wantFm) {
    check(!!fm, `FM (OPL source) is non-silent after logon: ${fm ? `peak ${fm.sources.opl.peak} in the second at t=${fm.t}` : 'it never reached ' + minPeak}`);
    check(!!fm && fm.worklet.received > logon.worklet.received && fm.worklet.peakSec >= minPeak, `FM reaches the AudioWorklet: ${fm ? `${fm.worklet.received - logon.worklet.received} frames received since logon, peak ${fm.worklet.peakSec} in that second` : 'no loud second'}`);
    check(!!fm && fm.sources.sb.peak < minPeak, `FM was heard on its own: the SB DSP peak was ${fm ? fm.sources.sb.peak : 'n/a'} in that second`);
  }
  if (wantDsp) {
    if (!ev.some((e) => e.golf)) out('web_audio_check: warning: no "# 3-D Golf" comment in the scenario');
    check(!!digital, `digital (SB DSP source) is non-silent once 3-D Golf runs: ${golfLoud.size} loud seconds (${[...golfLoud.values()].map((x) => `t=${x.t} peak ${x.sources.sb.peak} rms ${x.sources.sb.rms}`).join(', ')})`);
    check(!!digital && digital.worklet.peakSec >= minPeak, `digital reaches the AudioWorklet: ${[...golfLoud.values()].map((x) => `t=${x.t} peak ${x.worklet.peakSec}`).join(', ') || 'no loud second'}`);
  }
  out(`web_audio_check: ${tapLines.length} 'audio: tap' console lines`);
} catch (err) {
  out(`FAIL: ${err.stack || err}`); failed++;
} finally {
  await browser.close();
  server.close();
}
out(failed ? `web_audio_check: ${failed} assertion(s) failed` : 'web_audio_check: all assertions held');
process.exit(failed ? 1 : 0);
