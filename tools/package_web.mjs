// Builds the deployable page from a finished public web build. Called by package_web.sh, which has
// already refused lifted/native builds. Usage: node package_web.mjs <build> <out> <esbuild> [options]
//   --game <ws(s)://host[:port]>   game server the page may open (repeatable)
//   --auth <http(s)://host>        sign-in server (repeatable; site.json "authServer" is added too). Its ws(s)
//                                  twin is allowed as well: the page dials the sign-in server when no game server is set
//   --connect-any                  CSP lets the page connect to any ws/wss/https origin (operator typed addresses)
//   --site-url <https://host>      the public address (overrides site.json "siteUrl"): canonical and og:url tags,
//                                  robots.txt and sitemap.xml are written only when there is one
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { dirname, join, relative } from 'node:path';
import zlib from 'node:zlib';

const [build, out, esbuild, ...opts] = process.argv.slice(2);
const games = [], auths = [];
let connectAny = false, siteUrlOpt = '';
for (let i = 0; i < opts.length; i++) {
  if (opts[i] === '--game') games.push(opts[++i]);
  else if (opts[i] === '--auth') auths.push(opts[++i]);
  else if (opts[i] === '--connect-any') connectAny = true;
  else if (opts[i] === '--site-url') siteUrlOpt = opts[++i] || '';
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

// ---- display-filter shaders and lookup tables (third_party/, copied to <build>/shaders by CMake) ----
// Fetched by filters.js when a filter is first chosen. GLSL stays as published: the licence headers are part of it.
for (const f of readdirSync(join(build, 'shaders')).sort()) {
  const m = /^([\w+-]+)\.(glsl|png)$/.exec(f);
  if (!m) fail(`unexpected file shaders/${f}`);
  assetMap[`shaders/${f}`] = putHashed('assets/shaders', m[1], m[2], read(`shaders/${f}`));
}

// The page inserts inn.js itself when the runtime is wanted (page.js, wantRuntime), through innAsset().
assetMap['inn.js'] = innjsPath;
// The audio sink's worklet module, which the runtime loads through innAsset() (audio_sdl.c).
assetMap['audio-worklet.js'] = putHashed('assets', 'worklet', 'js', minify(read('audio-worklet.js'), 'js'));

// ---- the page font (third_party/vt323/, SIL OFL 1.1): same origin, hashed; the CSS is moved to assets/, so it names the file beside it ----
const fontPath = putHashed('assets', 'vt323', 'ttf', read('fonts/VT323-Regular.ttf'));
const fontName = fontPath.split('/').pop();
const FONT_URL = /url\(["']?(?:\.\.\/)?fonts\/VT323-Regular\.ttf["']?\)/g;

// ---- HTML pages: styles and scripts move out, hashed; inline scripts are listed for the CSP ----
const inlineHashes = new Set();
const attr = (a, n) => { const m = new RegExp(`\\b${n}=("([^"]*)"|'([^']*)'|([^\\s>]+))`).exec(a); return m ? (m[2] ?? m[3] ?? m[4]) : null; };
function page(file, prefix) {
  let html = read(file).toString('utf8');
  const stem = file === 'inn.html' ? 'app' : 'legal';
  const styles = [];
  html = html.replace(/<style\b[^>]*>([\s\S]*?)<\/style>/g, (_, css) => { styles.push(css.replace(FONT_URL, `url("${fontName}")`)); return styles.length === 1 ? '\0STYLE\0' : ''; });
  if (styles.length) {
    const p = putHashed('assets', stem, 'css', minify(styles.join('\n'), 'css'));
    html = html.replace('\0STYLE\0', `<link rel="stylesheet" href="${prefix}${p}">`);
  }
  const core = [];
  let firstCore = -1;
  html = html.replace(/<script\b([^>]*)>([\s\S]*?)<\/script>/g, (all, a, body) => {
    if (/\btype=["']?application\/ld\+json/.test(a)) return all;   // data, not code: the CSP ignores it and it is not minified
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

// ---- site.json (unhashed: operators edit it) ----
let site;
try { site = JSON.parse(read('site.json').toString('utf8')); } catch (e) { fail(`site.json is not valid JSON: ${e.message}`); }
put('site.json', read('site.json'));
const unset = ['siteName', 'operatorName', 'contactEmail', 'privacyEmail', 'governingLaw', 'serverLocation', 'effectiveDate'].filter((k) => !site[k]);
if (unset.length) console.warn(`package_web: note: site.json leaves ${unset.join(', ')} unset (the legal pages will say so); edit it in the out directory before deploying`);
if (site.authServer) auths.push(site.authServer);
if (site.gameServer) games.push(site.gameServer);

// The public address. Without one there is no canonical link, og:url, og:image, robots.txt or sitemap.xml:
// a preview image needs an absolute address, and a sitemap must name the real host.
const siteUrl = siteUrlOpt || (typeof site.siteUrl === 'string' ? site.siteUrl.trim() : '');
const base = siteUrl ? origin(siteUrl, ['https:', 'http:']) : '';
if (siteUrl && new URL(siteUrl).pathname.replace(/\/$/, '')) fail(`--site-url must be an origin without a path: ${siteUrl}`);
if (!base) console.warn('package_web: note: no --site-url and no site.json "siteUrl": writing no canonical link, og:url, og:image, robots.txt or sitemap.xml, and the legal pages stay noindex');

// Anonymous diagnostics (runtime/web/telemetry.js): on only with a connection string, and the CSP then names its ingestion origin.
let telemetryOrigin = '';
const cs = site.telemetry && typeof site.telemetry.connectionString === 'string' ? site.telemetry.connectionString.trim() : '';
if (cs) {
  const parts = Object.fromEntries(cs.split(';').map((p) => [p.slice(0, p.indexOf('=')).trim().toLowerCase(), p.slice(p.indexOf('=') + 1).trim()]));
  if (!/^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i.test(parts.instrumentationkey || '')) fail('site.json telemetry.connectionString has no valid InstrumentationKey');
  let u;
  try { u = new URL(parts.ingestionendpoint || ''); } catch { fail('site.json telemetry.connectionString has no valid IngestionEndpoint'); }
  const loopback = ['127.0.0.1', 'localhost'].includes(u.hostname);
  if (u.protocol !== 'https:' && !(u.protocol === 'http:' && loopback)) fail('the telemetry IngestionEndpoint must be https (http only for a loopback test address)');
  telemetryOrigin = u.origin;
} else console.warn('package_web: note: site.json has no telemetry.connectionString; the page sends no diagnostics');

// ---- search and link-preview tags ----
const TAGLINE = 'a 1990s online world in your browser';   // the same words page.js puts in document.title
const esc = (t) => t.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
const siteName = typeof site.siteName === 'string' ? site.siteName.trim() : '';
// file: 'inn.html' or 'legal/<name>.html'. The sources carry generic text; this fills in the name and the address.
// The shell arrives already minified by Emscripten (unquoted attributes, reordered), so tags are read with attr().
function seo(html, file) {
  const legal = file !== 'inn.html';
  const url = base + (legal ? `/${file}` : '/');
  const set = {}, drop = new Set();
  if (siteName) {
    const title = legal ? html.match(/<title>([^<]*) · Online world<\/title>/)[1] + ` · ${esc(siteName)}` : `${esc(siteName)}: ${TAGLINE}`;
    html = html.replace(/<title>[^<]*<\/title>/, `<title>${title}</title>`);
    set['og:title'] = set['twitter:title'] = title;
    set['og:site_name'] = esc(siteName);
    if (!legal) html = html.replace(/(<h1\b[^>]*\bid="?site-title"?[^>]*>)[^<]*/, `$1${title}`);
  }
  if (base) {
    set['og:image'] = set['twitter:image'] = `${base}/og.png`;
    if (legal) drop.add('robots');   // the legal pages are listed in the sitemap
  } else for (const k of ['og:image', 'og:image:width', 'og:image:height', 'og:image:alt', 'twitter:image', 'twitter:image:alt']) drop.add(k);
  html = html.replace(/<meta\b([^>]*)>/g, (all, a) => {
    const prop = attr(a, 'property'), key = prop ?? attr(a, 'name');
    if (key === null) return all;
    if (drop.has(key)) return '';
    const media = attr(a, 'media');
    const out = `<meta ${prop !== null ? 'property' : 'name'}="${key}" content="${key in set ? set[key] : (attr(a, 'content') ?? '')}"${media ? ` media="${media}"` : ''}>`;
    return key === 'description' && base ? `${out}<link rel="canonical" href="${url}"><meta property="og:url" content="${url}">` : out;
  });
  // JSON-LD: the name and, with an address, the url and image; without one, no image.
  html = html.replace(/<script\b([^>]*)>([\s\S]*?)<\/script>/g, (all, a, body) => {
    if (attr(a, 'type') !== 'application/ld+json') return all;
    const ld = JSON.parse(body);
    if (siteName) ld.name = siteName;
    if (base) { ld.url = url; ld.image = `${base}/og.png`; } else delete ld.image;
    return `<script type="application/ld+json">${JSON.stringify(ld).replace(/</g, '\\u003c')}</script>`;
  });
  return html;
}

let main = page('inn.html', '');
// What the shell wrapped around that tag goes with it, and the page must be the lazy kind.
main = main.replace(/(<!--(?:(?!-->)[\s\S])*-->\s*)?<template id="?inn-runtime"?>\s*<\/template>/, '');
if (/<template\b|<script[^>]*\bsrc=["']?inn\.js/.test(main)) fail('inn.html still carries the runtime tag');
if (!/data-runtime=["']?lazy/.test(main)) fail('inn.html is not a lazy-runtime page (data-runtime)');
// The asset map goes before the first script (site.js reads it); JSON is data, so the CSP ignores it.
main = main.replace(/<script\b/, `<script type="application/json" id="inn-assets">${JSON.stringify(assetMap)}</script><script`);
put('inn.html', seo(main, 'inn.html'));
for (const f of ['terms', 'privacy', 'copyright']) put(`legal/${f}.html`, seo(page(`legal/${f}.html`, '../'), `legal/${f}.html`));

// ---- the preview image, robots.txt and sitemap.xml ----
const og = read('og.png');
if (!(og.length > 8 && og.readUInt32BE(16) === 1200 && og.readUInt32BE(20) === 630)) fail('og.png is not a 1200x630 PNG (tools/make_og.mjs writes it)');
put('og.png', og);
// The page icons (tools/make_favicon.py and tools/make_logo.mjs write them): unhashed, the pages and the manifest name them by path.
const ICONS = ['favicon.ico', 'icon.svg', 'apple-touch-icon.png', 'icon-192.png', 'icon-512.png', 'manifest.webmanifest'];
for (const f of ICONS) put(`icons/${f}`, read(`icons/${f}`));
if (base) {
  put('robots.txt', `User-agent: *\nAllow: /\n\nSitemap: ${base}/sitemap.xml\n`);
  const urls = ['/', '/legal/terms.html', '/legal/privacy.html', '/legal/copyright.html'];
  put('sitemap.xml', `<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n${urls.map((u) => `  <url><loc>${base}${u}</loc></url>`).join('\n')}\n</urlset>\n`);
}

// ---- staticwebapp.config.json ----
const connect = ["'self'"];
for (const g of games) connect.push(origin(g, ['ws:', 'wss:']));
for (const a of new Set(auths)) {
  const o = origin(a, ['http:', 'https:']);
  connect.push(o, o.replace(/^http/, 'ws'));
}
if (telemetryOrigin) connect.push(telemetryOrigin);
if (connectAny) connect.push('ws:', 'wss:', 'https:');
if (!games.length && !connectAny) console.warn('package_web: note: no --game given; the CSP lets the page reach only its own origin' + (auths.length ? ' and the sign-in server (which it then dials)' : ''));
const csp = [
  "default-src 'none'",
  `script-src 'self' 'wasm-unsafe-eval' ${[...inlineHashes].join(' ')}`.trim(),
  "style-src 'self'",
  "style-src-attr 'unsafe-inline'",
  "font-src 'self'",
  "img-src 'self' data: blob:",
  "manifest-src 'self'",
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
    { route: '/robots.txt', headers: noCache },
    { route: '/sitemap.xml', headers: noCache },
    // Not hashed: link previews need a stable address. A day is long enough to spare the bandwidth, short enough to change it.
    { route: '/og.png', headers: { 'Cache-Control': 'public, max-age=86400' } },
    { route: '/icons/*', headers: { 'Cache-Control': 'public, max-age=86400' } },
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
  mimeTypes: { '.wasm': 'application/wasm', '.json': 'application/json', '.glsl': 'text/plain', '.txt': 'text/plain', '.xml': 'application/xml', '.webmanifest': 'application/manifest+json', '.ico': 'image/x-icon', '.svg': 'image/svg+xml' },
};
put('staticwebapp.config.json', JSON.stringify(config, null, 2) + '\n');

// ---- guard: nothing outside the expected list ships, and nothing that looks like game data ----
const ALLOWED = [/^inn\.html$/, /^site\.json$/, /^og\.png$/, /^icons\/(favicon\.ico|icon\.svg|apple-touch-icon\.png|icon-(192|512)\.png|manifest\.webmanifest)$/, /^robots\.txt$/, /^sitemap\.xml$/, /^staticwebapp\.config\.json$/, /^legal\/(terms|privacy|copyright)\.html$/,
  /^assets\/(app|inn|legal)\.[0-9a-f]{10}\.(js|css|wasm)$/, /^assets\/vt323\.[0-9a-f]{10}\.ttf$/, /^assets\/worklet\.[0-9a-f]{10}\.js$/, /^assets\/(scenes|music)\/\w+\.[0-9a-f]{10}\.js$/,
  /^assets\/shaders\/[\w+-]+\.[0-9a-f]{10}\.(glsl|png)$/];
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
