// Page side of the browser build: puts the user's INN install into the C: drive (/c), then starts
// the runtime with COM1 on a WebSocket. Query parameters are listed in docs/web.md.
'use strict';

const DRIVE = '/c';
const PROGRAM = { exe: 'TSNEXEC.EXE', dir: 'INN', args: ['DEFAULT'] };
const SYNC_EVERY_MS = 15000;
const LOG_LINES_MAX = 400;
const WS_PORT_DEFAULT = 2316;
const REMEMBER_KEY = 'inn.remember', SERVER_KEY = 'inn.com1', SCALE_KEY = 'inn.scale';
const SCALES = ['sharp', 'smooth', 'crt'];
const TOAST_MS = 6000;
// The one place for a site name. Empty means no wordmark and the neutral title below.
const SITE_NAME = '';
// Sign-in providers: '' asks no server; a URL asks that server's /auth/providers. '?auth=<url>' in the
// page's address overrides it, and any other non-empty '?auth=' value asks the game server's address.
const AUTH_SERVER = '';
const NEUTRAL_TITLE = 'Online world';
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
}

function showName() {
  if (!SITE_NAME) { document.title = NEUTRAL_TITLE; return; }
  document.title = SITE_NAME;
  $('wordmark').textContent = SITE_NAME;
  $('wordmark').hidden = false;
}

// ---- dial-up status: the COM1 state lines of the runtime, shown like a modem ----

const dial = { lines: [], timer: 0 };
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
  $('dial-actions').hidden = true;
  $('light-tx').className = 'on';
  $('light-cd').className = '';
  $('dial-title').textContent = 'Dialing';
  dialLine('ATZ');
  dialLine('OK');
  if (page.musicNote) dialLine(page.musicNote);
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
    url.protocol = url.protocol === 'wss:' ? 'https:' : 'http:';
    return url.origin;
  } catch { return ''; }
}

async function loadProviders() {
  const list = $('provider-list');
  // Only a server that has sign-in is asked (innkeeper PLAN Phase 7): innkeeperd's WebSocket port does
  // not answer HTTP, so probing it on every load only fills both consoles with errors.
  const configured = new URLSearchParams(location.search).get('auth') ?? AUTH_SERVER;
  const base = configured === '' ? '' : httpBase(configured) || httpBase($('com1').value.trim() || defaultServer());
  let found = [];
  try {
    if (base) {
      const res = await fetch(`${base}/auth/providers`, { headers: { accept: 'application/json' } });
      if (res.ok) found = await res.json();
    }
  } catch { /* no providers: the section stays hidden */ }
  found = Array.isArray(found) ? found.filter((p) => p && typeof p.name === 'string' && typeof p.url === 'string') : [];
  list.replaceChildren();
  for (const p of found) {
    let href;
    try { href = new URL(p.url, base).href; } catch { continue; }
    if (!/^https?:/.test(href)) continue;
    const a = document.createElement('a');
    a.className = 'btn';
    a.href = href;
    a.textContent = `Sign in with ${p.name}`;
    list.append(a);
  }
  $('providers').hidden = !list.children.length;
  adv('providers', [...list.children].map((a) => ({ name: a.textContent.replace(/^Sign in with /, ''), href: a.href })));
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
    setLink('online');
    dialOnline();
  } else if (/^uart: websocket (closed|error)/.test(text) && $('link').dataset.state !== 'offline') {
    setLink('offline');
    if (page.view === 'dialing') dialFailed();
    else toast('Lost the connection to the server. Reload the page to reconnect.', 'bad');
  }
}

function log(text) {
  console.log(text);
  watchLog(text);
  const pre = $('log');
  pre.textContent += text + '\n';
  const lines = pre.textContent.split('\n');
  if (lines.length > LOG_LINES_MAX) pre.textContent = lines.slice(-LOG_LINES_MAX).join('\n');
}

function defaultServer() {
  const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
  return `${scheme}://${location.hostname || '127.0.0.1'}:${WS_PORT_DEFAULT}`;
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

// Folder pickers drop empty directories, but the interpreter needs its swap directory to exist.
function createVirtualDirs(configPaths) {
  for (const cfg of configPaths) {
    const text = new TextDecoder('latin1').decode(FS.readFile(cfg));
    const match = /^\s*virtualDir\s*=\s*(\S+)/im.exec(text);
    if (!match || match[1].includes('..')) continue;
    const dir = `${cfg.slice(0, cfg.lastIndexOf('/'))}/${match[1].replace(/\\/g, '/')}`;
    if (!exists(dir)) FS.mkdirTree(dir);
  }
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

function installFiles(files) {
  const root = installRoot(files.map((f) => f.path));
  clearDir(DRIVE);
  const configs = [];
  let count = 0;
  for (const file of files) {
    if (!file.path.startsWith(root)) continue;
    const parts = file.path.slice(root.length).split('/').filter((p) => p && p !== '.');
    if (!parts.length || parts.includes('..')) continue;
    const dest = `${DRIVE}/${PROGRAM.dir}/${parts.join('/')}`;
    FS.mkdirTree(dest.slice(0, dest.lastIndexOf('/')));
    FS.writeFile(dest, file.data);
    if (/(^|\/)lsci\.cfg$/i.test(dest)) configs.push(dest);
    count++;
  }
  createVirtualDirs(configs);
  ensureAdlib(configs);
  return count;
}

function syncDrive(populate) {
  if (!page.persistent || page.syncing) return Promise.resolve();
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
  }
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
    const count = installFiles(await readFiles(source));
    page.drive = 'picked';
    page.fileCount = count;
    page.changing = false;
    log(`installed ${count} files into C:\\${PROGRAM.dir}`);
    if ($('remember').checked) await syncDrive(false);
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
  if (page.started) return;
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
    if (!page.started) { zone.classList.add('over'); document.body.classList.add('dropping'); }
  });
  window.addEventListener('dragleave', (e) => { if (!e.relatedTarget) { zone.classList.remove('over'); document.body.classList.remove('dropping'); } });
  window.addEventListener('drop', onDrop);
}

async function forget() {
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
  const args = ['--com1', server];
  if (params.get('perf')) args.push('--perf');
  if (params.get('ips')) args.push('--ips', params.get('ips'));
  return args.concat([DRIVE, `${PROGRAM.dir}\\${PROGRAM.exe}`], PROGRAM.args);
}

function statusRunning() {
  return `Running · COM1 ${page.server}`;
}

function audioContext() {
  return Module.SDL2 && Module.SDL2.audioContext;
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

function setScale(scale) {
  if (!SCALES.includes(scale)) scale = SCALES[0];
  $('stage').dataset.scale = scale;
  const label = { sharp: 'Sharp', smooth: 'Smooth', crt: 'CRT' }[scale];
  $('mode-label').textContent = label;
  $('mode').ariaLabel = `Display mode: ${label}`;
  layoutFrame();
  for (const b of $('scale').querySelectorAll('button')) b.setAttribute('aria-checked', String(b.dataset.scale === scale));
  storageSet(SCALE_KEY, scale);
}

function cycleScale() {
  setScale(SCALES[(SCALES.indexOf($('stage').dataset.scale) + 1) % SCALES.length]);
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
  if (open) $('close-settings').focus();
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
  InnSaves.init({
    fs: FS,
    drive: DRIVE,
    log,
    persist: () => syncDrive(false),
    send: (text) => !!Module.sendControl && Module.sendControl(text),
    notify: (text) => toast(text, 'ok'),
    status: (text) => {
      $('saves-note').textContent = text;
      const syncing = /^Syncing/.test(text);
      $('savesync').hidden = false;
      $('savesync').dataset.state = syncing ? 'syncing' : 'online';
      $('savesync-text').textContent = syncing ? 'Saving' : 'Saves synced';
      $('savesync').title = text;
    },
  });
  document.addEventListener('visibilitychange', () => document.hidden && InnSaves.flushAll());
  window.addEventListener('pagehide', () => InnSaves.flushAll());
}

function start() {
  if (page.started || page.drive === 'none') return;
  page.started = true;
  const server = $('com1').value.trim() || defaultServer();
  page.server = server;
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
  keepSaving();
  keepServerSaves();
  setLink('connecting');
  setStatus(statusRunning());
  Module.callMain(runtimeArgs(server));
  $('sound').hidden = !audioContext();
}

async function onRuntimeReady() {
  await mountDrive();
  showDriveState();
  adv('ready');
  if (params.get('autostart') && page.drive !== 'none') start();
}

function wirePage() {
  $('com1').value = params.get('com1') || storageGet(SERVER_KEY) || defaultServer();
  $('remember').checked = storageGet(REMEMBER_KEY) !== '0';
  $('pick-zip').addEventListener('change', (e) => e.target.files[0] && pick(filesFromZip, e.target.files[0]));
  $('pick-dir').addEventListener('change', (e) => e.target.files.length && pick(filesFromFolder, e.target.files));
  $('start').addEventListener('click', start);
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
  showName();
  loadProviders();
  $('open-settings').addEventListener('click', () => openSettings(true));
  $('close-settings').addEventListener('click', () => openSettings(false));
  $('scrim').addEventListener('click', () => openSettings(false));
  $('drawer').addEventListener('keydown', (e) => e.key === 'Escape' && openSettings(false));
  $('scale').addEventListener('click', (e) => e.target.dataset.scale && setScale(e.target.dataset.scale));
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
  onControlText: (text) => InnSaves.onControl(text),
  onFileClosed: (path) => InnSaves.onFileClosed(path),
  setStatus: (text) => text && setStatus(text),
  onRuntimeInitialized: () => onRuntimeReady().catch((err) => log(String(err.stack || err))),
  onGuestExit: (code) => {
    setStatus('Session ended');
    $('ended-text').textContent = code ? `The client exited with code ${code}.` : 'You have signed off.';
    $('ended').hidden = false;
    InnSaves.flushAll();
    if (document.fullscreenElement) document.exitFullscreen();
    syncDrive(false);
  },
};

wirePage();
