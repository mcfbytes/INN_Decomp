// Page side of the browser build: puts the user's INN install into the C: drive (/c), then starts
// the runtime with COM1 on a WebSocket. Query parameters are listed in docs/web.md.
'use strict';

const DRIVE = '/c';
const PROGRAM = { exe: 'TSNEXEC.EXE', dir: 'INN', args: ['DEFAULT'] };
const SYNC_EVERY_MS = 15000;
const LOG_LINES_MAX = 400;
const WS_PORT_DEFAULT = 2316;
const REMEMBER_KEY = 'inn.remember', SERVER_KEY = 'inn.com1', SCALE_KEY = 'inn.scale', SCAN_KEY = 'inn.scanlines';
const TOAST_MS = 6000;
// The one place for a site name. Empty means no wordmark and the neutral title below.
const SITE_NAME = '';
// Sign-in providers: '' asks no server; a URL asks that server's /auth/providers. '?auth=<url>' in the
// page's address overrides it, and any other non-empty '?auth=' value asks the game server's address.
const AUTH_SERVER = '';
const NEUTRAL_TITLE = 'Online world';
const TAGLINE = 'a 1990s online world in your browser';   // the packager writes the same words into the static title
const STRIP_EDGE_PX = 8, STRIP_HIDE_MS = 1500, CONNECT_HOLD_MS = 1400;
const FRAME_W = 960, SNAP_KEEP = 0.96;

const params = new URLSearchParams(location.search);
// The landing's adventure layer (adv.js). Every call is a no-op under ?classic=1, which keeps the old card.
const advOn = () => typeof Adv !== 'undefined' && Adv.active;
const adv = (name, arg) => { if (advOn() && Adv[name]) return Adv[name](arg); };
if (params.has('classic')) document.body.classList.add('classic');
const $ = (id) => document.getElementById(id);
const page = { view: 'setup', drive: 'none', persistent: false, syncing: false, started: false, server: '', fileCount: 0 };

function storageGet(key) {
  try { return localStorage.getItem(key); } catch { return null; }
}

function storageSet(key, value) {
  try { localStorage.setItem(key, value); } catch { /* private window: settings are not kept */ }
}

function setStatus(text) {
  $('status').textContent = text;
}

function setView(view) {
  page.view = view;
  document.body.dataset.view = view;
  layoutFrame();
}

// The frame is the largest 4:3 box that fits the window. Sharp snaps to a whole multiple of the 960x720
// buffer when that costs under 4% of the size, so pixels stay even; otherwise it is a nearest-neighbour fit.
function layoutFrame() {
  const fit = Math.min(innerWidth, innerHeight * 4 / 3);
  let width = fit;
  const k = Math.floor(fit / FRAME_W);
  if ($('stage').dataset.scale === 'sharp' && k >= 1 && k * FRAME_W >= fit * SNAP_KEEP) width = k * FRAME_W;
  else width = Math.floor(fit);
  $('frame').style.setProperty('--fw', `${width}px`);
  $('frame').style.setProperty('--fh', `${width * 3 / 4}px`);
  Filters.layout(width, width * 3 / 4, window.devicePixelRatio || 1);
}

// site.json's siteName wins over the constant above when it is set.
function showName() {
  const name = Site.cfg.siteName || SITE_NAME;
  if (!name) { document.title = `${NEUTRAL_TITLE}: ${TAGLINE}`; return; }
  document.title = `${name}: ${TAGLINE}`;
  $('site-title').textContent = document.title;
  $('wordmark').textContent = name;
  $('wordmark').hidden = false;
}

// ---- dial-up status: the COM1 state lines of the runtime, shown like a modem ----

const dial = { lines: [], timer: 0, t0: 0, onlineAt: 0 };
const NO_CARRIER_QUIPS = [
  'Your call has met an untimely end. Try again?',
  'The line went quiet. It may be sulking.',
  'Nobody picked up. Perhaps they are out walking the cable.',
  'The other end sends its regrets, and a busy signal.',
  'Somewhere a switchboard is shrugging. Try again?',
];

function dialShow() {
  adv('modem', dial.lines.map((l) => l.text).join('\n'));
  const pre = $('modem');
  pre.textContent = dial.lines.map((l) => l.text).join('\n');
  pre.append(document.createTextNode('\n'));
  const cur = document.createElement('span');
  cur.className = 'cur';
  pre.append(cur);
}

function dialLine(text, bad) {
  dial.lines.push({ text, bad: !!bad });
  dialShow();
}

function dialBegin(server) {
  dial.lines = [];
  dial.t0 = performance.now();
  dial.onlineAt = 0;
  $('dial-actions').hidden = true;
  $('light-tx').className = 'on';
  $('light-cd').className = '';
  $('dial-title').textContent = 'Dialing';
  dialLine('ATZ');
  dialLine('OK');
  if (!rt.ok) dialLine('; WARMING UP THE TUBES...');
  if (page.musicNote) dialLine(page.musicNote);
  if (page.prefetchNote) dialLine(page.prefetchNote);
  if (page.cardId) dialLine(`AT+CARD=${page.cardId}`);
  dialLine(`ATDT ${server.replace(/^wss?:\/\//, '')}`);
  adv('dialBegin');
}

function dialOnline() {
  if (page.view !== 'dialing') return;
  $('light-cd').className = 'on';
  $('dial-title').textContent = 'Connected';
  dialLine('CONNECT');
  adv('online');
  clearTimeout(dial.timer);
  const enter = () => {
    setView('play');
    $('canvas').focus();
  };
  dial.timer = setTimeout(() => (advOn() ? Adv.transition(enter) : enter()), CONNECT_HOLD_MS);
}

function dialFailed() {
  if (page.view !== 'dialing') return;
  clearTimeout(dial.timer);
  $('light-tx').className = 'err';
  $('dial-title').textContent = 'No answer';
  dialLine('NO CARRIER');
  dialLine(NO_CARRIER_QUIPS[Math.floor(Math.random() * NO_CARRIER_QUIPS.length)]);
  $('dial-actions').hidden = false;
  if (advOn()) adv('failed'); else $('retry').focus();
}

// ---- sign-in providers (shown only if the server lists some) ----

function httpBase(server) {
  try {
    const url = new URL(server);
    // TLS stays TLS: an https:// sign-in server (site.json authServer in production) as well as a wss:// line.
    url.protocol = url.protocol === 'wss:' || url.protocol === 'https:' ? 'https:' : 'http:';
    return url.origin;
  } catch { return ''; }
}

async function loadProviders() {
  const list = $('provider-list');
  await Site.ready;
  // Only a server that has sign-in is asked (innkeeper PLAN Phase 7): innkeeperd's WebSocket port does
  // not answer HTTP, so probing it on every load only fills both consoles with errors.
  const configured = new URLSearchParams(location.search).get('auth') ?? (Site.cfg.authServer || AUTH_SERVER);
  const base = configured === '' ? '' : httpBase(configured) || httpBase($('com1').value.trim() || defaultServer());
  let found = [], required = false;
  try {
    if (base) {
      const traced = Telemetry.traceFetch('GET', '/auth/providers');
      const res = await fetch(`${base}/auth/providers`, { headers: Object.assign({ accept: 'application/json' }, traced ? { traceparent: traced.header } : {}) })
        .catch((error) => { if (traced) traced.done(0); throw error; });
      if (traced) traced.done(res.status);
      // {signInRequired, providers}; a bare list is an older server's answer, where sign-in was optional.
      if (res.ok) {
        const answer = await res.json();
        found = Array.isArray(answer) ? answer : (answer && answer.providers) || [];
        required = !!(answer && answer.signInRequired === true);
      }
    }
  } catch { /* no providers: the section stays hidden */ }
  found = Array.isArray(found) ? found.filter((p) => p && typeof p.name === 'string' && typeof p.url === 'string') : [];
  InnAuth.configure(base);
  list.replaceChildren();
  const usable = [];
  for (const p of found) {
    let href;
    try { href = new URL(p.url, base).href; } catch { continue; }
    // The login must live on the auth server's origin: the popup's message is accepted only from there.
    if (!/^https?:/.test(href) || new URL(href).origin !== new URL(base).origin) continue;
    // The provider's own sign-in button (signin.js), as a link.
    const a = InnSignIn.button({ id: typeof p.id === 'string' ? p.id : '', name: p.name }, { href: InnAuth.loginUrl(href, 'redirect') });
    // The attestation is added when the age notice has been answered, so refresh the address on use.
    a.addEventListener('click', () => { a.href = InnAuth.loginUrl(href, 'redirect'); });
    list.append(a);
    usable.push({ id: typeof p.id === 'string' ? p.id : p.name, name: p.name, href });
  }
  $('providers').hidden = !list.children.length;
  adv('providers', { list: usable, required: required && usable.length > 0 });
}

// ---- the signed-in account: its number and password go into the browser's copy of the install ----

// Writes the account number into every LSCI.CFG (the root one and the land copies LSCIGET made) and
// the stored password into INN\PASS_SET.DTA, so the client's own logon runs unchanged (innkeeper
// docs/protocol/messages.md section 4.1; the encoder is in auth.js). Then the account's own view of the persona
// files (below): the account has no persona until the client's Select Player screen makes one.
async function writeCredentials(account, password) {
  const file = InnAuth.passwordFile(password);
  const configs = findConfigs(DRIVE);
  if (!configs.length) throw new Error('LSCI.CFG not found in the installation.');
  for (const cfg of configs) {
    const text = new TextDecoder('latin1').decode(FS.readFile(cfg));
    FS.writeFile(cfg, InnAuth.setCfgId(text, account));
  }
  const dir = `${DRIVE}/${PROGRAM.dir}`;
  const existing = FS.readdir(dir).find((n) => /^pass_set\.dta$/i.test(n)) || 'PASS_SET.DTA';
  FS.writeFile(`${dir}/${existing}`, file);
  log(`signed-in account ${account} written to LSCI.CFG and ${existing}`);
  InnSaves.init({ fs: FS, drive: DRIVE, log });
  InnSaves.useView(account);
  await prefetchSaves(account);
  // Without GUIDE.CFG the client opens with its welcome and INN Guide questions, which belong to a first run.
  if (!FS.readdir(dir).some((n) => n.toUpperCase() === 'GUIDE.CFG')) FS.writeFile(`${dir}/GUIDE.CFG`, new Uint8Array(2));
  return syncDrive(false);
}

// The client reads its personas once, at Select Player, before the host's manifest exists, so a browser that has
// never held this account would show an empty list and its Create would write over the host's copy.
// So the account's files are fetched over HTTP first (innkeeperd `GET /auth/accounts/<id>/saves`, authorised by
// the session token) and written into the drive, recorded as synced. If that fails the line is dialed anyway,
// as it used to be, with a narrator note and a line in the modem output. Format and reasons: docs/web.md.
async function prefetchSaves(account) {
  page.prefetchNote = '';
  let why = '';
  try {
    const listing = await InnAuth.saves(account);
    if (!listing || !Array.isArray(listing.files)) throw new InnAuth.AuthError('server', 'The saves answer was not understood.', 200);
    const done = await InnSaves.prefetch(account, listing, (path) => InnAuth.saveFile(account, path));
    log(`saves: ${done.restored} of ${listing.files.length} file${listing.files.length === 1 ? '' : 's'} fetched before dialing${done.failed ? `, ${done.failed} failed` : ''}`);
    if (done.failed) why = `${done.failed} file${done.failed === 1 ? '' : 's'} would not come`;
  } catch (e) {
    why = e.kind === 'network' ? 'the sign-in server did not answer'
      : e.status ? `the server answered ${e.status}` : String(e.message || e);
    log(`saves: not fetched before dialing: ${why}`);
  }
  if (!why) return;
  page.prefetchNote = `; SAVES NOT FETCHED (${why})`;
  adv('say', `Your saved files could not be fetched from the server (${why}). Dialing anyway with what this browser holds, so Select Player may start out empty. The operator suggests patience.`);
  const reduced = window.matchMedia && matchMedia('(prefers-reduced-motion: reduce)').matches;
  await new Promise((resolve) => setTimeout(resolve, reduced ? 1200 : 2800));
}

function toast(text, kind = '') {
  // On the landing the narrator box tells the player what went wrong.
  if (kind === 'bad' && advOn() && (page.view === 'setup' || page.view === 'dialing') && /^Could not use that|^Lost the/.test(text)) return adv('error', text);
  const el = document.createElement('div');
  el.className = `toast ${kind}`;
  el.append(document.createElement('i'), text);
  $('toasts').append(el);
  setTimeout(() => el.remove(), TOAST_MS);
}

// The runtime reports the COM1 socket in its log; the pill mirrors it.
function setLink(state) {
  const text = { idle: 'Not connected', connecting: 'Connecting', online: 'Connected', offline: 'Disconnected' }[state];
  $('link').dataset.state = state;
  $('link-text').textContent = text;
  $('link').title = page.server ? `${text}: ${page.server}` : text;
  $('server-now').textContent = page.server ? `${text} to ${page.server}` : 'Not started yet.';
}

function watchLog(text) {
  if (/^uart: websocket open/.test(text)) {
    dial.onlineAt = performance.now();
    Telemetry.connect('ok', dial.onlineAt - dial.t0);
    setLink('online');
    dialOnline();
  } else if (/^uart: websocket (closed|error)/.test(text) && $('link').dataset.state !== 'offline') {
    // failed: the line never opened; closed: an open line ended. Only the result and a coarse duration go out.
    if (dial.onlineAt) Telemetry.connect('closed', performance.now() - dial.onlineAt);
    else Telemetry.connect('failed', performance.now() - dial.t0);
    dial.onlineAt = 0;
    setLink('offline');
    if (page.view === 'dialing') dialFailed();
    else toast('Lost the connection to the server. Reload the page to reconnect.', 'bad');
  }
}

// ?audiotap=1: the runtime logs "audio: tap" lines (per source: frames, peak, RMS per second; audio.c) and this
// page adds what the worklet received. window.innAudioTap is the latest of both (docs/web.md, "Sound").
const AUDIO_TAP = params.get('audiotap') === '1';
const audioTap = { sources: {}, lines: 0, worklet: null, posted: null };
if (AUDIO_TAP) {
  window.innAudioTap = audioTap;
  setInterval(() => {
    const s = Module.innAudio;
    if (!s || !s.tap) return;
    audioTap.worklet = { ...s.tap, peakSec: s.tapWin || 0 };
    s.tapWin = 0;
    audioTap.posted = { messages: s.posts || 0, frames: s.postedFrames || 0 };
    log(`audio: tap worklet messages=${s.tap.messages} received=${s.tap.received} peak=${s.tap.peak} peaksec=${audioTap.worklet.peakSec} posted=${s.posts || 0}/${s.postedFrames || 0}`);
  }, 1000);
}

function tapLine(text) {
  const m = /^audio: tap t=(\d+)(.*)$/.exec(text);
  if (!m) return;
  audioTap.lines++;
  audioTap.t = Number(m[1]);
  for (const f of m[2].matchAll(/(\w+) frames=(\d+) peak=(\d+) rms=(\d+) total=(\d+) max=(\d+)/g))
    audioTap.sources[f[1]] = { frames: +f[2], peak: +f[3], rms: +f[4], totalFrames: +f[5], maxPeak: +f[6] };
}

function log(text) {
  if (AUDIO_TAP) tapLine(text);
  console.log(text);
  watchLog(text);
  const pre = $('log');
  pre.textContent += text + '\n';
  const lines = pre.textContent.split('\n');
  if (lines.length > LOG_LINES_MAX) pre.textContent = lines.slice(-LOG_LINES_MAX).join('\n');
}

// The game line: site.json's gameServer, else the sign-in server's origin as ws(s) (innkeeperd answers both
// on one listener, and in production one TLS host fronts it), else this page's host on the default port.
function defaultServer() {
  const set = Site.cfg.gameServer || wsBase(params.get('auth') ?? (Site.cfg.authServer || AUTH_SERVER));
  if (set) return set;
  const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
  return `${scheme}://${location.hostname || '127.0.0.1'}:${WS_PORT_DEFAULT}`;
}

function wsBase(server) {
  try {
    const url = new URL(server);
    return /^https?:$/.test(url.protocol) ? url.origin.replace(/^http/, 'ws') : '';
  } catch { return ''; }
}

// ---- the runtime (inn.js and inn.wasm) is fetched when it is wanted (docs/web.md, "Loading the runtime") ----

// Preload builds (dev) know their drive only once the runtime runs, so they, and ?autostart, load it at once.
const EAGER = document.documentElement.dataset.runtime === 'eager' || params.has('autostart');
const IDLE_START_MS = 8000;
const rt = { state: 'idle', fs: false, ok: false, held: null, promise: null, t0: 0, scriptAt: 0 };   // idle > loading > ready

// Starts the load once. why is 'intent' (the player is reaching for the disks or the phone), 'saved' or 'idle'
// (nobody asked: low priority). Returns a promise that settles when the drive is mounted.
function wantRuntime(why = 'intent') {
  if (rt.state !== 'idle') return rt.promise;
  rt.state = 'loading';
  rt.t0 = performance.now();
  rt.promise = new Promise((resolve, reject) => { rt.done = resolve; rt.fail = reject; });
  rt.promise.catch(() => {});
  const s = document.createElement('script');
  const alt = document.documentElement.dataset.asyncify;
  const useAlt = alt && (params.get('suspend') === 'asyncify' || typeof WebAssembly.Suspending !== 'function');
  s.src = innAsset(useAlt ? alt : 'inn.js');
  s.async = true;
  if (why !== 'intent') s.fetchPriority = 'low';
  s.onload = () => { rt.scriptAt = performance.now(); };
  s.onerror = () => { rt.state = 'idle'; s.remove(); rt.fail(new Error('the runtime did not download')); };
  document.head.append(s);
  return rt.promise;
}

// A saved install, seen without the runtime: IDBFS keeps /c in the IndexedDB database "/c" (object store
// FILE_DATA, keyed by path, one entry per file and directory).
function savedInstall() {
  const look = new Promise((resolve) => {
    let req;
    try { req = indexedDB.open(DRIVE); } catch { return resolve(false); }
    req.onupgradeneeded = () => req.transaction.abort();   // there is no such database: do not create one
    req.onerror = (e) => { e.preventDefault(); resolve(false); };
    req.onsuccess = () => {
      const db = req.result;
      try {
        const dir = `${DRIVE}/${PROGRAM.dir}`;
        const n = db.transaction('FILE_DATA').objectStore('FILE_DATA').count(IDBKeyRange.bound(`${dir}/`, `${dir}0`));
        n.onsuccess = () => { db.close(); resolve(n.result > 0); };
        n.onerror = () => { db.close(); resolve(false); };
      } catch { db.close(); resolve(false); }
    };
  });
  return Promise.race([look, new Promise((resolve) => setTimeout(resolve, 3000, false))]);
}

// Nothing asked for the runtime: fetch it anyway, at idle, once the page is showing.
function wantRuntimeLater(ms, why) {
  const run = () => {
    if (document.hidden) return document.addEventListener('visibilitychange', run, { once: true });
    (window.requestIdleCallback || ((f) => setTimeout(f, 0)))(() => wantRuntime(why), { timeout: 2000 });
  };
  setTimeout(run, ms);
}

async function startUp() {
  if (EAGER) return wantRuntime('intent');
  page.persistent = typeof indexedDB !== 'undefined';
  const saved = page.persistent && await savedInstall();
  if (saved) page.drive = 'saved';
  showDriveState();
  adv('ready');
  if (saved) wantRuntimeLater(0, 'saved'); else wantRuntimeLater(IDLE_START_MS, 'idle');
}

// ---- the C: drive ----

function exists(path) {
  return FS.analyzePath(path).exists;
}

function programPath() {
  return `${DRIVE}/${PROGRAM.dir}/${PROGRAM.exe}`;
}

function clearDir(dir) {
  for (const name of FS.readdir(dir)) {
    if (name === '.' || name === '..') continue;
    const path = `${dir}/${name}`;
    if (FS.isDir(FS.stat(path).mode)) {
      clearDir(path);
      FS.rmdir(path);
    } else {
      FS.unlink(path);
    }
  }
}

// The directory that holds TSNEXEC.EXE becomes C:\INN, whatever it was called when packed.
function installRoot(paths) {
  const pattern = new RegExp(`(^|/)${PROGRAM.exe.replace('.', '\\.')}$`, 'i');
  const hit = paths.filter((p) => pattern.test(p)).sort((a, b) => a.length - b.length)[0];
  if (!hit) throw new Error(`${PROGRAM.exe} not found: pick the folder that contains it, or a zip of it`);
  return hit.slice(0, hit.length - PROGRAM.exe.length);
}

// Native builds play music through adl.drv only (Sound Blaster digital audio is not native yet), so
// there a music line that names another driver is rewritten in the browser's copy of the file.
function ensureAdlib(configPaths) {
  if (!Module.innNative) return;
  let noted = false;
  for (const cfg of configPaths) {
    const text = new TextDecoder('latin1').decode(FS.readFile(cfg));
    const next = text.replace(/^(\s*music\s*=\s*)(\S+)/gim, (line, prefix, value) => {
      if (value.toLowerCase() === 'adl.drv') return line;
      // Said in the modem output when dialing, not as a toast over the landing's narrator box.
      if (!noted) {
        page.musicNote = `; MUSIC ${value.toUpperCase()} -> ADL.DRV`;
        noted = true;
      }
      return `${prefix}adl.drv`;
    });
    if (next !== text) FS.writeFile(cfg, next);
  }
}

// Folder pickers and zip.js drop empty directories, but the interpreter needs its swap directory to exist. The
// path is relative to the interpreter's working directory: the LSCI.CFG folder in the 1994 client, but every land
// folder (the ones with a RESOURCE.MAP) in the 1995 one, which starts the Clubhouse in CC\ and reads ..\LSCI.CFG.
function createVirtualDirs(configPaths) {
  const landDirs = findResourceDirs(`${DRIVE}/${PROGRAM.dir}`);
  for (const cfg of configPaths) {
    const text = new TextDecoder('latin1').decode(FS.readFile(cfg));
    const match = /^\s*virtualDir\s*=\s*(\S+)/im.exec(text);
    if (!match || match[1].includes('..')) continue;
    const name = match[1].replace(/\\/g, '/');
    for (const base of [cfg.slice(0, cfg.lastIndexOf('/')), ...landDirs]) {
      const dir = `${base}/${name}`;
      if (!exists(dir)) FS.mkdirTree(dir);
    }
  }
}

function findResourceDirs(dir) {
  if (!exists(dir)) return [];
  const found = [];
  for (const name of FS.readdir(dir)) {
    if (name === '.' || name === '..') continue;
    const path = `${dir}/${name}`;
    if (FS.isDir(FS.stat(path).mode)) found.push(...findResourceDirs(path));
    else if (/^resource\.map$/i.test(name)) found.push(dir);
  }
  return found;
}

function findConfigs(dir) {
  const found = [];
  for (const name of FS.readdir(dir)) {
    if (name === '.' || name === '..') continue;
    const path = `${dir}/${name}`;
    if (FS.isDir(FS.stat(path).mode)) found.push(...findConfigs(path));
    else if (/^lsci\.cfg$/i.test(name)) found.push(path);
  }
  return found;
}

// What an install would write, as {dest, data}. Touches no drive, so it also checks disks picked before the runtime is up.
function layoutFiles(files) {
  const root = installRoot(files.map((f) => f.path));
  const laid = [];
  for (const file of files) {
    if (!file.path.startsWith(root)) continue;
    const parts = file.path.slice(root.length).split('/').filter((p) => p && p !== '.');
    if (!parts.length || parts.includes('..')) continue;
    laid.push({ dest: `${DRIVE}/${PROGRAM.dir}/${parts.join('/')}`, data: file.data });
  }
  return laid;
}

function installFiles(laid) {
  clearDir(DRIVE);
  const configs = [];
  for (const { dest, data } of laid) {
    FS.mkdirTree(dest.slice(0, dest.lastIndexOf('/')));
    FS.writeFile(dest, data);
    if (/(^|\/)lsci\.cfg$/i.test(dest)) configs.push(dest);
  }
  createVirtualDirs(configs);
  ensureAdlib(configs);
  return laid.length;
}

function syncDrive(populate) {
  if (!rt.fs || !page.persistent || page.syncing) return Promise.resolve();
  page.syncing = true;
  return new Promise((resolve) => FS.syncfs(populate, (err) => {
    page.syncing = false;
    if (err) log(`storage: sync failed: ${err}`);
    resolve();
  }));
}

async function mountDrive() {
  if (exists(programPath())) {
    page.drive = 'preloaded';
    const configs = findConfigs(DRIVE);
    createVirtualDirs(configs);
    ensureAdlib(configs);
    return;
  }
  if (!exists(DRIVE)) FS.mkdir(DRIVE);
  if (typeof indexedDB === 'undefined') return;
  FS.mount(FS.filesystems.IDBFS, {}, DRIVE);
  page.persistent = true;
  await syncDrive(true);
  if (exists(programPath())) {
    page.drive = 'saved';
    const configs = findConfigs(DRIVE);
    createVirtualDirs(configs);
    ensureAdlib(configs);
  } else if (page.drive === 'saved') page.drive = 'none';   // the early look at IndexedDB was wrong
}

// ---- picking an install ----

async function filesFromZip(file) {
  return InnZip.read(await file.arrayBuffer());
}

async function filesFromFolder(fileList) {
  const files = [];
  for (const file of fileList)
    files.push({ path: file.webkitRelativePath || file.name, data: new Uint8Array(await file.arrayBuffer()) });
  return files;
}

async function pick(readFiles, source) {
  try {
    setStatus('Reading the installation...');
    wantRuntime();
    const laid = layoutFiles(await readFiles(source));
    // Before the runtime is up the parsed files wait in memory; onRuntimeReady writes them.
    const count = rt.ok ? installFiles(laid) : (rt.held = laid).length;
    page.drive = 'picked';
    page.fileCount = count;
    page.changing = false;
    log(`installed ${count} files into C:\\${PROGRAM.dir}`);
    if (rt.ok && $('remember').checked) await syncDrive(false);
    showDriveState();
    adv('picked', count);
  } catch (err) {
    showDriveState();
    toast(`Could not use that installation: ${err.message}`, 'bad');
    log(String(err.stack || err));
  }
}

// Dropped folders arrive as directory entries; read them the way a folder picker would.
function readEntry(entry, path) {
  if (entry.isFile)
    return new Promise((resolve, reject) => entry.file(async (file) =>
      resolve([{ path, data: new Uint8Array(await file.arrayBuffer()) }]), reject));
  const reader = entry.createReader();
  return new Promise((resolve, reject) => {
    const files = [];
    const next = () => reader.readEntries(async (batch) => {
      if (!batch.length) return resolve(files);
      for (const child of batch) files.push(...await readEntry(child, `${path}/${child.name}`));
      next();
    }, reject);
    next();
  });
}

async function filesFromDrop(entries) {
  const files = [];
  for (const entry of entries) files.push(...await readEntry(entry, entry.name));
  return files;
}

function onDrop(e) {
  e.preventDefault();
  $('drop').classList.remove('over');
  document.body.classList.remove('dropping');
  if (page.started || Site.blocking()) return;
  const entries = [...e.dataTransfer.items].map((item) => item.webkitGetAsEntry && item.webkitGetAsEntry()).filter(Boolean);
  const zip = [...e.dataTransfer.files].find((f) => /\.zip$/i.test(f.name));
  if (entries.some((entry) => entry.isDirectory)) pick(filesFromDrop, entries);
  else if (zip) pick(filesFromZip, zip);
  else { toast('Drop the install folder itself, or a .zip of it.', 'bad'); return; }
  adv('dropped');
}

function wireDrop() {
  const zone = $('drop');
  window.addEventListener('dragover', (e) => {
    e.preventDefault();
    wantRuntime();
    if (!page.started && !Site.blocking()) { zone.classList.add('over'); document.body.classList.add('dropping'); }
  });
  window.addEventListener('dragleave', (e) => { if (!e.relatedTarget) { zone.classList.remove('over'); document.body.classList.remove('dropping'); } });
  window.addEventListener('drop', onDrop);
}

async function forget() {
  await wantRuntime();
  clearDir(DRIVE);
  await syncDrive(false);
  page.drive = 'none';
  showDriveState();
  toast('The saved installation was removed from this browser.', 'ok');
}

function showDriveState() {
  const text = {
    none: '',
    preloaded: 'Packaged with this build',
    saved: 'Saved in this browser',
    picked: `${page.fileCount} files loaded`,
  }[page.drive];
  const ready = page.drive !== 'none';
  $('install-state').textContent = text;
  $('install').hidden = !ready || page.changing;
  $('drop').hidden = ready && !page.changing;
  $('change').hidden = page.drive === 'preloaded';
  $('start').disabled = !ready;
  $('forget').hidden = !(page.persistent && (page.drive === 'saved' || page.drive === 'picked'));
  $('remember').disabled = !page.persistent;
  $('storage-note').textContent = !page.persistent ? 'This build keeps nothing between visits.'
    : page.drive === 'saved' || (page.drive === 'picked' && $('remember').checked)
      ? 'Your installation and player files are saved in this browser.' : 'Nothing is saved in this browser.';
  setStatus(page.started ? statusRunning() : ready ? 'Ready to connect' : 'Add your installation');
  adv('drive');
}

// ---- running ----

function runtimeArgs(server) {
  // The session token rides on the WebSocket address as ?session= (docs/web.md, Sign-in), and with diagnostics on
  // the trace as ?traceparent= ("Diagnostics"). page.server, the modem lines and the status text keep the plain address.
  let spec = server;
  const traceparent = Telemetry.lineTraceparent();
  if (page.session || traceparent) {
    const url = new URL(server);
    if (page.session) url.searchParams.set('session', page.session);
    if (traceparent) url.searchParams.set('traceparent', traceparent);
    spec = url.href;
  }
  const args = ['--com1', spec];
  if (params.get('perf')) args.push('--perf');
  if (AUDIO_TAP) args.push('--audiotap');
  if (params.get('ips')) args.push('--ips', params.get('ips'));
  if (params.get('idle') === '0') args.push('--no-idle-skip');
  return args.concat([DRIVE, `${PROGRAM.dir}\\${PROGRAM.exe}`], PROGRAM.args);
}

function statusRunning() {
  return `Running · COM1 ${page.server}`;
}

// The runtime's AudioWorklet sink (audio_sdl.c) creates this context when main starts.
function audioContext() {
  return Module.innAudio ? Module.innAudio.ctx : null;
}

function setMuted(muted) {
  const ctx = audioContext();
  if (!ctx) return;
  page.muted = muted;
  if (muted) ctx.suspend(); else ctx.resume();
  $('sound').setAttribute('aria-pressed', String(muted));
  $('sound').title = $('sound').ariaLabel = muted ? 'Unmute sound' : 'Mute sound';
  $('sound').querySelector('.mute').toggleAttribute('hidden', !muted);
  $('sound').querySelector('.wave').toggleAttribute('hidden', muted);
}

// The picture filter (filters.js). Sharp and Smooth are the plain canvas; every other choice, and Scanlines over
// either, draws on the GPU. A choice the browser cannot run is greyed out in the list and the picture falls back to
// Sharp, but the stored choice is left alone (it may work on another browser).
let scaleToken = 0;
function fillScaleList() {
  const sel = $('scale');
  if (sel.options.length) return;
  let group = null, groupName = '';
  for (const f of Filters.list) {
    if (f.group !== groupName) { group = document.createElement('optgroup'); group.label = groupName = f.group; sel.append(group); }
    const o = document.createElement('option');
    const reason = Filters.why(f.id);
    o.value = f.id;
    o.textContent = reason ? `${f.label} (unavailable)` : f.label;
    o.disabled = !!reason;
    group.append(o);
  }
}

function usableScales() {
  return Filters.list.filter((f) => !Filters.why(f.id)).map((f) => f.id);
}

function setScale(scale, remember = true) {
  fillScaleList();
  const def = Filters.list.find((f) => f.id === scale);
  if (!def) { scale = 'sharp'; remember = false; }
  const reason = Filters.why(scale);
  const want = scale;
  if (reason) scale = 'sharp';
  const shown = Filters.list.find((f) => f.id === scale);
  const scan = $('scanlines').checked && !shown.noScan;
  $('stage').dataset.scale = scale;
  $('mode-label').textContent = shown.label;
  $('mode').ariaLabel = `Display mode: ${shown.label}`;
  $('scale').value = want;
  $('scale-desc').textContent = reason ? `${Filters.list.find((f) => f.id === want).label} is not available: ${reason}` : shown.desc;
  $('scanlines').disabled = !!shown.noScan;
  layoutFrame();
  if (remember) storageSet(SCALE_KEY, want);
  const token = ++scaleToken;
  Filters.use(scale, scan).then(() => {}, (err) => {
    if (token !== scaleToken) return;
    log(`Display filter ${scale}: ${err.message}`);
    toast(`The ${shown.label} filter could not start. Showing Sharp instead.`, 'bad');
    setScale('sharp', false);
  });
}

function cycleScale() {
  const ids = usableScales();
  setScale(ids[(ids.indexOf($('stage').dataset.scale) + 1) % ids.length]);
  Telemetry.event('display_filter', { filter: $('stage').dataset.scale });
}

// The strip slides in when the pointer touches the top edge and leaves 1.5 s after the pointer does.
function wireStrip() {
  const strip = $('strip');
  let timer = 0;
  const show = () => { clearTimeout(timer); timer = 0; strip.classList.add('show'); };
  // Started once: mouse moves over the game must not keep pushing the hide back.
  const later = () => {
    if (timer) return;
    timer = setTimeout(() => {
      timer = 0;
      if (!strip.matches(':hover')) strip.classList.remove('show');
    }, STRIP_HIDE_MS);
  };
  document.addEventListener('mousemove', (e) => {
    if (page.view !== 'play') return;
    if (e.clientY <= STRIP_EDGE_PX || strip.matches(':hover')) show();
    else if (strip.classList.contains('show')) later();
  });
  document.addEventListener('mouseleave', () => strip.classList.contains('show') && later());
  strip.addEventListener('mouseenter', show);
  strip.addEventListener('mouseleave', later);
  // A mouse click leaves no focus behind: the strip would stay pinned open, and Space or Enter in the
  // game would press the button again. Keyboard use keeps its focus (and the strip) as before.
  strip.addEventListener('click', (e) => {
    if (e.detail === 0) return;
    const button = e.target.closest('button');
    if (button && button.id !== 'open-settings') { button.blur(); $('canvas').focus({ preventScroll: true }); }
  });
  // Ctrl+Alt+F is taken before SDL (which listens on the canvas) sees it; every other key goes to the game.
  window.addEventListener('keydown', (e) => {
    if (!(e.ctrlKey && e.altKey && !e.shiftKey && !e.metaKey && e.code === 'KeyF')) return;
    e.preventDefault();
    e.stopPropagation();
    if (page.view === 'play' && document.fullscreenEnabled) toggleFullscreen();
  }, true);
}

function toggleFullscreen() {
  if (document.fullscreenElement) document.exitFullscreen();
  else document.documentElement.requestFullscreen().then(() => $('canvas').focus()).catch(() => {});
}

function openSettings(open) {
  document.body.classList.toggle('drawer-open', open);
  $('drawer').inert = !open;
  if (open) { showDiagnostics(); $('close-settings').focus(); }
  else if (page.started) $('canvas').focus();
}

function watchFocus() {
  const canvas = $('canvas'), frame = $('frame');
  const update = () => frame.classList.toggle('blurred', document.activeElement !== canvas);
  canvas.addEventListener('focus', update);
  canvas.addEventListener('blur', update);
  canvas.addEventListener('mousedown', () => {
    canvas.focus();
    const ctx = audioContext();
    if (ctx && !page.muted && ctx.state === 'suspended') ctx.resume();
  });
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  update();
}

function keepSaving() {
  if (!page.persistent || !$('remember').checked) return;
  setInterval(() => syncDrive(false), SYNC_EVERY_MS);
  document.addEventListener('visibilitychange', () => document.hidden && syncDrive(false));
  window.addEventListener('pagehide', () => syncDrive(false));
}

// Game saves follow the account to the server (saves.js); this wires them to the runtime and the page.
function keepServerSaves() {
  // Test knobs (milliseconds): ?savedebounce=, ?savemaxwait=, ?savesweep= (0 turns the sweep off).
  const knob = (name) => (params.has(name) ? Number(params.get(name)) : NaN);
  InnSaves.config({ debounce: knob('savedebounce'), maxWait: knob('savemaxwait'), sweep: knob('savesweep') });
  InnSaves.init({
    fs: FS,
    drive: DRIVE,
    log,
    persist: () => syncDrive(false),
    send: (text) => !!Module.sendControl && Module.sendControl(text),
    notify: (text) => toast(text, 'ok'),
    saved: (when) => {
      $('saves-last').textContent = `Last saved to the server: ${new Date(when).toLocaleTimeString()}.`;
      $('saves-now').hidden = false;
    },
    status: (text) => {
      $('saves-note').textContent = text;
      const syncing = /^Syncing/.test(text);
      $('savesync').hidden = false;
      $('savesync').dataset.state = syncing ? 'syncing' : 'online';
      $('savesync-text').textContent = syncing ? 'Saving' : 'Saves synced';
      $('savesync').title = text;
    },
  });
  // Hidden tabs push at once (and skip the periodic sweep); a closing tab sends what it can, best effort.
  document.addEventListener('visibilitychange', () => document.hidden && InnSaves.flushAll());
  window.addEventListener('pagehide', () => InnSaves.flushFinal());
  $('saves-now').addEventListener('click', async () => {
    const button = $('saves-now');
    button.disabled = true;
    const ok = await InnSaves.flushAll();
    button.disabled = false;
    if (!ok) toast('The server is not answering just now, so your saves are staying in this browser. Try again once the line is back.', 'bad');
  });
}

async function start(session) {
  if (page.started || page.drive === 'none' || Site.blocking()) return;
  page.started = true;
  const server = $('com1').value.trim() || defaultServer();
  page.server = server;
  page.session = session || '';
  storageSet(SERVER_KEY, server);
  storageSet(REMEMBER_KEY, $('remember').checked ? '1' : '0');
  $('stage').hidden = false;
  $('dial-title').textContent = 'Dialing';
  $('setup').hidden = true;
  $('dial').hidden = false;
  dialBegin(server);
  setView('dialing');
  $('fullscreen').hidden = !document.fullscreenEnabled;
  watchFocus();
  $('canvas').focus();
  setLink('connecting');
  setStatus(statusRunning());
  try { await wantRuntime(); } catch (e) {
    log(String(e.message || e));
    dialLine(`; THE TUBES WOULD NOT WARM (${e.message || e})`, true);
    return dialFailed();
  }
  // The classic path: the drive shows the install's own personas again.
  if (!session && InnSaves.useView('own')) syncDrive(false);
  keepSaving();
  keepServerSaves();
  Module.callMain(runtimeArgs(server));
  $('sound').hidden = !audioContext();
}

// Download is the request until the wasm has arrived (the browser's own timing for it, else until inn.js ran);
// start is what the runtime then took to come up.
function reportRuntime() {
  const wasm = performance.getEntriesByType('resource').find((e) => /\.wasm$/.test(e.name.split(/[?#]/)[0]));
  const got = wasm && wasm.responseEnd > 0 ? wasm.responseEnd : (rt.scriptAt || performance.now());
  Telemetry.runtimeStarted(got - rt.t0, Math.max(0, performance.now() - got), Module.innNative ? 'native' : 'dosbox');
}

async function onRuntimeReady() {
  rt.fs = true;
  reportRuntime();
  await mountDrive();
  const held = rt.held;
  rt.held = null;
  rt.ok = true;
  if (held) {
    installFiles(held);
    log(`installed ${held.length} files into C:\\${PROGRAM.dir}`);
    if ($('remember').checked) await syncDrive(false);
  }
  rt.state = 'ready';
  showDriveState();
  adv('ready');
  rt.done();
  if (params.get('autostart') && page.drive !== 'none') Site.whenClear().then(() => start());
}

// ---- anonymous diagnostics (telemetry.js; docs/web.md, "Diagnostics") ----

function showDiagnostics() {
  const box = $('diag'), note = $('diag-note');
  const why = Telemetry.blockedBy();
  box.checked = Telemetry.preference() && !why;
  box.disabled = !!why || !Telemetry.available();
  note.textContent = why ? `Off: your browser sends ${why}.`
    : !Telemetry.available() ? 'This site does not collect diagnostics.'
    : 'Error reports and load times, with no account, address or chat in them.';
}

function wireDiagnostics() {
  $('diag').addEventListener('change', () => { Telemetry.setPreference($('diag').checked); showDiagnostics(); });
  showDiagnostics();
  // site.json decides whether there is anything to send, and arrives after this.
  Site.ready.then(() => {
    Telemetry.init(Site.cfg.telemetry);
    if (Telemetry.enabled()) Telemetry.capabilities({ webgl2: Filters.support().webgl2, jspi: typeof WebAssembly === 'object' && typeof WebAssembly.Suspending === 'function' });
    showDiagnostics();
  });
}

function wirePage() {
  $('com1').value = params.get('com1') || storageGet(SERVER_KEY) || defaultServer();
  // site.json arrives after this, and it may name the server.
  Site.ready.then(() => { if (!params.get('com1') && !storageGet(SERVER_KEY)) $('com1').value = defaultServer(); });
  $('remember').checked = storageGet(REMEMBER_KEY) !== '0';
  $('pick-zip').addEventListener('change', (e) => e.target.files[0] && pick(filesFromZip, e.target.files[0]));
  $('pick-dir').addEventListener('change', (e) => e.target.files.length && pick(filesFromFolder, e.target.files));
  $('start').addEventListener('click', () => start());
  $('forget').addEventListener('click', forget);
  $('change').addEventListener('click', () => { page.changing = true; showDriveState(); });
  $('restart').addEventListener('click', () => location.reload());
  $('retry').addEventListener('click', () => location.reload());
  $('mode').addEventListener('click', cycleScale);
  $('com1').addEventListener('change', loadProviders);
  window.addEventListener('resize', layoutFrame);
  $('sound').addEventListener('click', () => setMuted(!page.muted));
  $('fullscreen').addEventListener('click', toggleFullscreen);
  document.addEventListener('fullscreenchange', () => {
    document.body.classList.toggle('fullscreen', !!document.fullscreenElement);
    layoutFrame();
    if (page.started) $('canvas').focus();
  });
  wireStrip();
  wireDiagnostics();
  InnAuth.setAttest(() => Site.attest());
  showName();
  Site.ready.then(showName);
  loadProviders();
  $('open-settings').addEventListener('click', () => openSettings(true));
  $('close-settings').addEventListener('click', () => openSettings(false));
  $('scrim').addEventListener('click', () => openSettings(false));
  $('drawer').addEventListener('keydown', (e) => e.key === 'Escape' && openSettings(false));
  $('scale').addEventListener('change', () => { setScale($('scale').value); Telemetry.event('display_filter', { filter: $('stage').dataset.scale }); });
  $('scanlines').checked = storageGet(SCAN_KEY) === '1';
  $('scanlines').addEventListener('change', () => { storageSet(SCAN_KEY, $('scanlines').checked ? '1' : '0'); setScale($('scale').value, false); });
  Filters.onLost = (why) => { toast(`${why} Showing Sharp instead.`, 'bad'); setScale('sharp', false); };
  $('copy-log').addEventListener('click', () => navigator.clipboard.writeText($('log').textContent)
    .then(() => toast('Log copied.', 'ok'), () => toast('Could not copy the log.', 'bad')));
  setScale(storageGet(SCALE_KEY));
  setLink('idle');
  wireDrop();
  adv('init');
}

var Module = {
  canvas: document.getElementById('canvas'),
  print: log,
  printErr: log,
  innAudioTapOn: AUDIO_TAP,
  onControlText: (text) => InnSaves.onControl(text),
  onFileClosed: (path) => InnSaves.onFileClosed(path),
  // The client is hanging up (Hayes "+++", then ATH0): the host ends the save channel at ATH0.
  onLineEscape: () => InnSaves.flushFinal(),
  setStatus: (text) => text && setStatus(text),
  onRuntimeInitialized: () => onRuntimeReady().catch((err) => log(String(err.stack || err))),
  onGuestExit: (code) => {
    setStatus('Session ended');
    $('ended-text').textContent = code ? `The client exited with code ${code}.` : 'You have signed off.';
    $('ended').hidden = false;
    InnSaves.flushFinal();
    if (document.fullscreenElement) document.exitFullscreen();
    syncDrive(false);
  },
};

wirePage();
startUp();
