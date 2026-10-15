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

const params = new URLSearchParams(location.search);
const $ = (id) => document.getElementById(id);
const page = { drive: 'none', persistent: false, syncing: false, started: false, server: '', fileCount: 0 };

function storageGet(key) {
  try { return localStorage.getItem(key); } catch { return null; }
}

function storageSet(key, value) {
  try { localStorage.setItem(key, value); } catch { /* private window: settings are not kept */ }
}

function setStatus(text) {
  $('status').textContent = text;
}

function toast(text, kind = '') {
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
  if (/^uart: websocket open/.test(text)) setLink('online');
  else if (/^uart: websocket (closed|error)/.test(text) && $('link').dataset.state !== 'offline') {
    setLink('offline');
    toast('Lost the connection to the server. Reload the page to reconnect.', 'bad');
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
  if (!hit) throw new Error(`${PROGRAM.exe} not found: pick the INN directory or a zip of it`);
  return hit.slice(0, hit.length - PROGRAM.exe.length);
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
    createVirtualDirs(findConfigs(DRIVE));
    return;
  }
  if (!exists(DRIVE)) FS.mkdir(DRIVE);
  if (typeof indexedDB === 'undefined') return;
  FS.mount(FS.filesystems.IDBFS, {}, DRIVE);
  page.persistent = true;
  await syncDrive(true);
  if (exists(programPath())) {
    page.drive = 'saved';
    createVirtualDirs(findConfigs(DRIVE));
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
  if (page.started) return;
  const entries = [...e.dataTransfer.items].map((item) => item.webkitGetAsEntry && item.webkitGetAsEntry()).filter(Boolean);
  const zip = [...e.dataTransfer.files].find((f) => /\.zip$/i.test(f.name));
  if (entries.some((entry) => entry.isDirectory)) pick(filesFromDrop, entries);
  else if (zip) pick(filesFromZip, zip);
  else toast('Drop the INN folder itself, or a .zip of it.', 'bad');
}

function wireDrop() {
  const zone = $('drop');
  window.addEventListener('dragover', (e) => {
    e.preventDefault();
    if (!page.started) zone.classList.add('over');
  });
  window.addEventListener('dragleave', (e) => { if (!e.relatedTarget) zone.classList.remove('over'); });
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
  setStatus(page.started ? statusRunning() : ready ? 'Ready to connect' : 'Add your INN installation');
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
  for (const b of $('scale').querySelectorAll('button')) b.setAttribute('aria-checked', String(b.dataset.scale === scale));
  storageSet(SCALE_KEY, scale);
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
    status: (text) => { $('saves-note').textContent = text; },
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
  $('setup').hidden = true;
  $('stage').hidden = false;
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
  $('sound').addEventListener('click', () => setMuted(!page.muted));
  $('fullscreen').addEventListener('click', toggleFullscreen);
  document.addEventListener('fullscreenchange', () =>
    document.body.classList.toggle('fullscreen', !!document.fullscreenElement));
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
