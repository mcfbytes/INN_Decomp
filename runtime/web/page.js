// Page side of the browser build: puts the user's INN install into the C: drive (/c), then starts
// the runtime with COM1 on a WebSocket. Query parameters are listed in docs/web.md.
'use strict';

const DRIVE = '/c';
const PROGRAM = { exe: 'TSNEXEC.EXE', dir: 'INN', args: ['DEFAULT'] };
const SYNC_EVERY_MS = 15000;
const LOG_LINES_MAX = 400;
const WS_PORT_DEFAULT = 2316;
const REMEMBER_KEY = 'inn.remember', SERVER_KEY = 'inn.com1';

const params = new URLSearchParams(location.search);
const $ = (id) => document.getElementById(id);
const page = { drive: 'none', persistent: false, syncing: false, started: false };

function storageGet(key) {
  try { return localStorage.getItem(key); } catch { return null; }
}

function storageSet(key, value) {
  try { localStorage.setItem(key, value); } catch { /* private window: settings are not kept */ }
}

function setStatus(text) {
  $('status').textContent = text;
}

function log(text) {
  console.log(text);
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

function installFiles(files) {
  const root = installRoot(files.map((f) => f.path));
  clearDir(DRIVE);
  let count = 0;
  for (const file of files) {
    if (!file.path.startsWith(root)) continue;
    const parts = file.path.slice(root.length).split('/').filter((p) => p && p !== '.');
    if (!parts.length || parts.includes('..')) continue;
    const dest = `${DRIVE}/${PROGRAM.dir}/${parts.join('/')}`;
    FS.mkdirTree(dest.slice(0, dest.lastIndexOf('/')));
    FS.writeFile(dest, file.data);
    count++;
  }
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
    return;
  }
  if (!exists(DRIVE)) FS.mkdir(DRIVE);
  if (typeof indexedDB === 'undefined') return;
  FS.mount(FS.filesystems.IDBFS, {}, DRIVE);
  page.persistent = true;
  await syncDrive(true);
  if (exists(programPath())) page.drive = 'saved';
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
    log(`installed ${count} files into C:\\${PROGRAM.dir}`);
    if ($('remember').checked) await syncDrive(false);
    showDriveState();
  } catch (err) {
    setStatus(`Could not use that installation: ${err.message}`);
    log(String(err.stack || err));
  }
}

async function forget() {
  clearDir(DRIVE);
  await syncDrive(false);
  page.drive = 'none';
  showDriveState();
}

function showDriveState() {
  const text = {
    none: 'No installation loaded.',
    preloaded: 'Using the installation packaged with this build.',
    saved: 'Using the installation saved in this browser.',
    picked: 'Installation loaded.',
  }[page.drive];
  $('install-state').textContent = text;
  $('start').disabled = page.drive === 'none';
  $('forget').hidden = !(page.persistent && page.drive !== 'none');
  $('remember').disabled = !page.persistent;
  setStatus(page.drive === 'none' ? 'Waiting for your INN installation.' : 'Ready.');
}

// ---- running ----

function runtimeArgs(server) {
  const args = ['--com1', server];
  if (params.get('perf')) args.push('--perf');
  if (params.get('ips')) args.push('--ips', params.get('ips'));
  return args.concat([DRIVE, `${PROGRAM.dir}\\${PROGRAM.exe}`], PROGRAM.args);
}

function watchFocus() {
  const canvas = $('canvas'), screen = $('screen');
  const update = () => screen.classList.toggle('blurred', document.activeElement !== canvas);
  canvas.addEventListener('focus', update);
  canvas.addEventListener('blur', update);
  canvas.addEventListener('mousedown', () => canvas.focus());
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  update();
}

function keepSaving() {
  if (!page.persistent || !$('remember').checked) return;
  setInterval(() => syncDrive(false), SYNC_EVERY_MS);
  document.addEventListener('visibilitychange', () => document.hidden && syncDrive(false));
  window.addEventListener('pagehide', () => syncDrive(false));
}

function start() {
  if (page.started || page.drive === 'none') return;
  page.started = true;
  const server = $('com1').value.trim() || defaultServer();
  storageSet(SERVER_KEY, server);
  storageSet(REMEMBER_KEY, $('remember').checked ? '1' : '0');
  $('setup').style.display = 'none';
  $('screen').style.display = 'block';
  watchFocus();
  $('canvas').focus();
  keepSaving();
  setStatus(`Running. COM1 is ${server}`);
  Module.callMain(runtimeArgs(server));
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
}

var Module = {
  canvas: document.getElementById('canvas'),
  print: log,
  printErr: log,
  setStatus: (text) => text && setStatus(text),
  onRuntimeInitialized: () => onRuntimeReady().catch((err) => log(String(err.stack || err))),
  onGuestExit: (code) => {
    setStatus(`The client exited (code ${code}). Reload the page to start again.`);
    syncDrive(false);
  },
};

wirePage();
