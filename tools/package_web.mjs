// Builds the deployable page from a finished public web build. Called by package_web.sh, which has
// already refused lifted/native builds. Usage: node package_web.mjs <build> <out> <esbuild> [options]
//   --game <ws(s)://host[:port]>   game server the page may open (repeatable)
//   --auth <http(s)://host>        sign-in server (repeatable; site.json "authServer" is added too)
//   --connect-any                  CSP lets the page connect to any ws/wss/https origin (operator typed addresses)
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { dirname, join, relative } from 'node:path';
import zlib from 'node:zlib';

const [build, out, esbuild, ...opts] = process.argv.slice(2);
const games = [], auths = [];
let connectAny = false;
for (let i = 0; i < opts.length; i++) {
  if (opts[i] === '--game') games.push(opts[++i]);
  else if (opts[i] === '--auth') auths.push(opts[++i]);
  else if (opts[i] === '--connect-any') connectAny = true;
  else fail(`unknown option ${opts[i]}`);
}
function fail(msg) { console.error(`package_web: refused: ${msg}`); process.exit(2); }
const origin = (u, schemes) => {
  let url;
  try { url = new URL(u); } catch { fail(`not a URL: ${u}`); }
  if (!schemes.includes(url.protocol)) fail(`${u} must use ${schemes.join(' or ')}`);
  return url.origin;
};

const read = (p) => readFileSync(join(build, p));
const hash = (buf) => createHash('sha256').update(buf).digest('hex').slice(0, 10);
const minify = (src, loader) => execFileSync(esbuild, ['--minify', `--loader=${loader}`, '--legal-comments=none', '--log-level=warning'], { input: src, maxBuffer: 1 << 28 });

if (existsSync(out)) rmSync(out, { recursive: true });
mkdirSync(out, { recursive: true });
const emitted = new Map();   // published path -> Buffer
const put = (p, buf) => { emitted.set(p, Buffer.from(buf)); };
const putHashed = (dir, name, ext, buf) => { const p = `${dir}/${name}.${hash(buf)}.${ext}`; put(p, buf); return p; };

// ---- the wasm and its loader ----
const wasm = read('inn.wasm');
const wasmPath = putHashed('assets', 'inn', 'wasm', wasm);
let innjs = read('inn.js').toString('utf8');
const wasmName = wasmPath.split('/').pop();
if ((innjs.match(/inn\.wasm/g) || []).length !== 1) fail('inn.js does not name inn.wasm exactly once');
innjs = innjs.replace('inn.wasm', wasmName);
const innjsPath = putHashed('assets', 'inn', 'js', innjs);

// ---- lazy files (scenes, songs): minified, hashed, listed in the page's asset map ----
const assetMap = {};
for (const dir of ['scenes', 'music']) {
  for (const f of readdirSync(join(build, dir)).sort()) {
    if (!f.endsWith('.js')) fail(`unexpected file ${dir}/${f}`);
    assetMap[`${dir}/${f}`] = putHashed(`assets/${dir}`, f.slice(0, -3), 'js', minify(read(`${dir}/${f}`), 'js'));
  }
}

// The page inserts inn.js itself when the runtime is wanted (page.js, wantRuntime), through innAsset().
assetMap['inn.js'] = innjsPath;

// ---- HTML pages: styles and scripts move out, hashed; inline scripts are listed for the CSP ----
const inlineHashes = new Set();
const attr = (a, n) => { const m = new RegExp(`\\b${n}=("([^"]*)"|'([^']*)'|([^\\s>]+))`).exec(a); return m ? (m[2] ?? m[3] ?? m[4]) : null; };
function page(file, prefix) {
  let html = read(file).toString('utf8');
  const stem = file === 'inn.html' ? 'app' : 'legal';
  const styles = [];
  html = html.replace(/<style\b[^>]*>([\s\S]*?)<\/style>/g, (_, css) => { styles.push(css); return styles.length === 1 ? '\0STYLE\0' : ''; });
  if (styles.length) {
    const p = putHashed('assets', stem, 'css', minify(styles.join('\n'), 'css'));
    html = html.replace('\0STYLE\0', `<link rel="stylesheet" href="${prefix}${p}">`);
  }
  const core = [];
  let firstCore = -1;
  html = html.replace(/<script\b([^>]*)>([\s\S]*?)<\/script>/g, (all, a, body) => {
    const src = attr(a, 'src');
    if (src === null) {   // inline: minify, allow by hash
      const min = minify(body, 'js').toString('utf8').trimEnd();
      inlineHashes.add(`'sha256-${createHash('sha256').update(min).digest('base64')}'`);
      return `<script>${min}</script>`;
    }
    if (/^(https?:)?\/\//.test(src)) fail(`${file} loads a script from another origin: ${src}`);
    if (src === 'inn.js') return '';   // Emscripten's tag, inert inside the shell's <template>: dropped, page.js loads the file
    core.push([src, read(src === 'legal.js' ? join(file.replace(/[^/]*$/, ''), src) : src).toString('utf8')]);
    if (firstCore < 0) firstCore = core.length;
    return core.length === 1 ? '\0APP\0' : '';
  });
  if (core.length) {
    // Classic scripts share one global scope, so one concatenated script behaves as the separate ones did.
    const name = file === 'inn.html' ? 'app' : 'legal';
    const p = putHashed('assets', name, 'js', minify(core.map(([, s]) => s).join('\n'), 'js'));
    html = html.replace('\0APP\0', `<script src="${prefix}${p}"></script>`);
  }
  return html;
}

let main = page('inn.html', '');
// What the shell wrapped around that tag goes with it, and the page must be the lazy kind.
main = main.replace(/(<!--(?:(?!-->)[\s\S])*-->\s*)?<template id="?inn-runtime"?>\s*<\/template>/, '');
if (/<template\b|<script[^>]*\bsrc=["']?inn\.js/.test(main)) fail('inn.html still carries the runtime tag');
if (!/data-runtime=["']?lazy/.test(main)) fail('inn.html is not a lazy-runtime page (data-runtime)');
// The asset map goes before the first script (site.js reads it); JSON is data, so the CSP ignores it.
main = main.replace(/<script\b/, `<script type="application/json" id="inn-assets">${JSON.stringify(assetMap)}</script><script`);
put('inn.html', main);
for (const f of ['terms', 'privacy', 'copyright']) put(`legal/${f}.html`, page(`legal/${f}.html`, '../'));

// ---- site.json (unhashed: operators edit it) ----
let site;
try { site = JSON.parse(read('site.json').toString('utf8')); } catch (e) { fail(`site.json is not valid JSON: ${e.message}`); }
put('site.json', read('site.json'));
const unset = ['siteName', 'operatorName', 'contactEmail', 'privacyEmail', 'governingLaw', 'serverLocation', 'effectiveDate'].filter((k) => !site[k]);
if (unset.length) console.warn(`package_web: note: site.json leaves ${unset.join(', ')} unset (the legal pages will say so); edit it in the out directory before deploying`);
if (site.authServer) auths.push(site.authServer);

// ---- staticwebapp.config.json ----
const connect = ["'self'"];
for (const g of games) connect.push(origin(g, ['ws:', 'wss:']));
for (const a of new Set(auths)) connect.push(origin(a, ['http:', 'https:']));
if (connectAny) connect.push('ws:', 'wss:', 'https:');
if (!games.length && !connectAny) console.warn('package_web: note: no --game given; the CSP lets the page reach only its own origin' + (auths.length ? ' and the sign-in server' : ''));
const csp = [
  "default-src 'none'",
  `script-src 'self' 'wasm-unsafe-eval' ${[...inlineHashes].join(' ')}`.trim(),
  "style-src 'self' https://fonts.googleapis.com",
  "style-src-attr 'unsafe-inline'",
  "font-src https://fonts.gstatic.com",
  "img-src 'self' data: blob:",
  "media-src 'self' blob:",
  `connect-src ${[...new Set(connect)].join(' ')}`,
  "base-uri 'none'", "form-action 'none'", "object-src 'none'", "frame-ancestors 'none'",
].join('; ');
const noCache = { 'Cache-Control': 'no-cache' };
const config = {
  routes: [
    { route: '/assets/*', headers: { 'Cache-Control': 'public, max-age=31536000, immutable' } },
    { route: '/', rewrite: '/inn.html', headers: noCache },
    { route: '/inn.html', headers: noCache },
    { route: '/site.json', headers: noCache },
    { route: '/legal/*', headers: noCache },
  ],
  // One page, no client-side routes: an unknown path is a plain 404, so there is no navigationFallback.
  globalHeaders: {
    'Content-Security-Policy': csp,
    // The sign-in popup keeps window.opener (it posts the result back); a plain same-origin would cut it.
    'Cross-Origin-Opener-Policy': 'same-origin-allow-popups',
    'Referrer-Policy': 'no-referrer',
    'X-Content-Type-Options': 'nosniff',
    'Permissions-Policy': 'camera=(), microphone=(), geolocation=(), payment=(), usb=(), serial=(), hid=()',
  },
  mimeTypes: { '.wasm': 'application/wasm', '.json': 'application/json' },
};
put('staticwebapp.config.json', JSON.stringify(config, null, 2) + '\n');

// ---- guard: nothing outside the expected list ships, and nothing that looks like game data ----
const ALLOWED = [/^inn\.html$/, /^site\.json$/, /^staticwebapp\.config\.json$/, /^legal\/(terms|privacy|copyright)\.html$/,
  /^assets\/(app|inn|legal)\.[0-9a-f]{10}\.(js|css|wasm)$/, /^assets\/(scenes|music)\/\w+\.[0-9a-f]{10}\.js$/];
for (const [p, buf] of emitted) {
  if (!ALLOWED.some((re) => re.test(p))) fail(`unexpected output file ${p}`);
  if (buf.length > 3 && ((buf[0] === 0x4d && buf[1] === 0x5a) || (buf[0] === 0x50 && buf[1] === 0x4b))) fail(`${p} looks like an executable or an archive`);
}
for (const p of emitted.keys()) {
  const full = join(out, p);
  mkdirSync(dirname(full), { recursive: true });
  writeFileSync(full, emitted.get(p));
}

// ---- report ----
const size = (b) => [b.length, zlib.gzipSync(b, { level: 9 }).length, zlib.brotliCompressSync(b).length];
console.log('file'.padEnd(46) + 'raw'.padStart(10) + 'gzip'.padStart(10) + 'brotli'.padStart(10));
let total = [0, 0, 0];
for (const p of [...emitted.keys()].sort()) {
  const s = size(emitted.get(p));
  total = total.map((t, i) => t + s[i]);
  console.log(p.padEnd(46) + s.map((n) => String(n).padStart(10)).join(''));
}
console.log('TOTAL'.padEnd(46) + total.map((n) => String(n).padStart(10)).join(''));
console.log(`package_web: wrote ${emitted.size} files to ${out}`);
