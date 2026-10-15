// Server-side game saves for the browser build (docs/web.md "Game saves", host side in the sibling
// project's docs/server/saves.md). Characters, automaps and persona files in the C: drive are kept per
// account on the host over the same WebSocket as COM1, as JSON text frames. The runtime hands those
// frames to Module.onControlText and sends them with Module.sendControl; closing a file the program
// wrote arrives as Module.onFileClosed.
'use strict';

const InnSaves = (() => {
  const UPLOAD_DEBOUNCE_MS = 1500;
  const REQUEST_TIMEOUT_MS = 15000;
  const STATE_PREFIX = 'inn.saves.';
  const FIXED = ['INN\\YSERBIUS\\MYCHARS.DAT', 'INN\\YSERBIUS\\AUTOMAPS.DAT',
    'INN\\TWINION\\TWINCHAR.DAT', 'INN\\TWINION\\AUTOMAPS.DAT', 'INN\\PLAYER.DIR'];
  const PERSONA = /^INN\\([A-Z0-9_$-]{1,8})\.DTA$/;
  const PASSWORD_STEM = 'PASS_SET';

  let env = null;            // { fs, drive, log, notify, persist, send, status }
  let account = null;        // set by the first manifest of this connection
  let state = {};            // path -> { version, hash }: what this browser last synced
  let nextId = 1;
  const pending = new Map(); // request id -> { resolve, timer }
  const timers = new Map();  // path -> debounce timer
  const busy = new Set();    // paths being uploaded or downloaded
  const again = new Set();   // paths that changed again meanwhile
  let restored = 0;          // downloads during the current manifest pass

  // ---- helpers ----

  // The server's allowlist, mirrored so the page does not offer what the host would refuse.
  function canonical(raw) {
    const path = raw.replace(/\//g, '\\').toUpperCase().replace(/^\\/, '');
    if (FIXED.includes(path)) return path;
    const m = PERSONA.exec(path);
    return m && m[1] !== PASSWORD_STEM ? path : null;
  }

  function toBase64(bytes) {
    let text = '';
    for (let i = 0; i < bytes.length; i += 0x8000) text += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
    return btoa(text);
  }

  function fromBase64(text) {
    const raw = atob(text);
    const bytes = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
    return bytes;
  }

  const K = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]);

  // SHA-256 in plain JS: crypto.subtle exists only on secure origins, and the page may be served over http.
  function sha256Hex(data) {
    const h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
    const padded = new Uint8Array(((data.length + 9 + 63) >> 6) << 6);
    padded.set(data);
    padded[data.length] = 0x80;
    const view = new DataView(padded.buffer);
    view.setUint32(padded.length - 8, Math.floor(data.length / 0x20000000));
    view.setUint32(padded.length - 4, (data.length << 3) >>> 0);
    const w = new Uint32Array(64);
    const rotr = (x, n) => (x >>> n) | (x << (32 - n));
    for (let off = 0; off < padded.length; off += 64) {
      for (let i = 0; i < 16; i++) w[i] = view.getUint32(off + i * 4);
      for (let i = 16; i < 64; i++) {
        const s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
        const s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
      }
      let [a, b, c, d, e, f, g, hh] = h;
      for (let i = 0; i < 64; i++) {
        const t1 = (hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i]) | 0;
        const t2 = ((rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) | 0;
        hh = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
      }
      h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    return [...h].map((n) => n.toString(16).padStart(8, '0')).join('');
  }

  // ---- the local files ----

  // The drive is a host file system: names are case-sensitive there, DOS paths are not.
  function hostPath(rel) {
    let dir = env.drive;
    for (const part of rel.split('\\')) {
      let names;
      try { names = env.fs.readdir(dir); } catch { return null; }
      const hit = names.find((n) => n.toUpperCase() === part);
      if (!hit) return null;
      dir = `${dir}/${hit}`;
    }
    return dir;
  }

  function readLocal(rel) {
    const path = hostPath(rel);
    if (!path) return null;
    try {
      const st = env.fs.stat(path);
      if (env.fs.isDir(st.mode)) return null;
      const data = env.fs.readFile(path);
      return { data, mtime: +st.mtime, hash: sha256Hex(data) };
    } catch { return null; }
  }

  function writeLocal(rel, data, mtime) {
    let path = hostPath(rel);
    if (!path) {
      const parts = rel.split('\\');
      const name = parts.pop();
      const dir = `${env.drive}/${parts.join('/')}`;
      env.fs.mkdirTree(dir);
      path = `${dir}/${name}`;
    }
    env.fs.writeFile(path, data);
    env.fs.utime(path, mtime, mtime);
  }

  // Every allowlisted file the drive has now.
  function localPaths() {
    const found = FIXED.filter((rel) => hostPath(rel));
    const inn = hostPath('INN');
    if (inn) {
      for (const name of env.fs.readdir(inn)) {
        const rel = canonical(`INN\\${name}`);
        if (rel && !found.includes(rel)) found.push(rel);
      }
    }
    return found;
  }

  // ---- what this browser last synced ----

  function loadState() {
    try { state = JSON.parse(localStorage.getItem(STATE_PREFIX + account)) || {}; } catch { state = {}; }
  }

  function remember(path, version, hash) {
    state[path] = { version, hash };
    try { localStorage.setItem(STATE_PREFIX + account, JSON.stringify(state)); } catch { /* kept for this visit only */ }
  }

  // ---- talking to the host ----

  function request(message) {
    return new Promise((resolve, reject) => {
      const id = nextId++;
      const timer = setTimeout(() => { pending.delete(id); reject(new Error('the host did not answer')); }, REQUEST_TIMEOUT_MS);
      pending.set(id, { resolve, timer });
      if (!env.send(JSON.stringify({ ...message, id }))) {
        clearTimeout(timer);
        pending.delete(id);
        reject(new Error('not connected'));
      }
    });
  }

  async function download(path) {
    const reply = await request({ op: 'get', path });
    if (reply.op !== 'file') throw new Error(`${reply.code || reply.op}: ${reply.detail || path}`);
    const data = fromBase64(reply.data);
    if (sha256Hex(data) !== reply.hash) throw new Error(`${path}: the download does not match its hash`);
    writeLocal(path, data, reply.mtime);
    remember(path, reply.version, reply.hash);
    env.persist();
    env.log(`saves: restored ${path} (version ${reply.version})`);
    restored++;
    return reply;
  }

  async function upload(path) {
    const local = readLocal(path);
    if (!local) return;
    const known = state[path];
    if (known && known.hash === local.hash) return;
    const reply = await request({ op: 'put', path, base: known ? known.version : 0, mtime: local.mtime, data: toBase64(local.data) });
    if (reply.op === 'stored') {
      remember(path, reply.version, reply.hash);
      env.log(`saves: uploaded ${path} (version ${reply.version}${reply.replaced ? ', replaced a newer-looking copy' : ''})`);
    } else if (reply.op === 'superseded') {
      env.log(`saves: ${path} is older than the server's copy (version ${reply.version}); keeping the server's, yours is kept there as version ${reply.kept_version}`);
      const got = await download(path);
      env.notify(`${path.split('\\').pop()} was newer on the server and has been restored; your local copy is kept there as an older version.`);
      return got;
    } else {
      throw new Error(`${reply.code}: ${reply.detail}`);
    }
  }

  // One sync action per path at a time; a change that arrives meanwhile runs afterwards.
  async function exclusive(path, action) {
    if (busy.has(path)) { again.add(path); return; }
    busy.add(path);
    try { await action(); } catch (err) { env.log(`saves: ${path}: ${err.message}`); }
    busy.delete(path);
    if (again.delete(path)) schedule(path, 0);
  }

  function schedule(path, delay = UPLOAD_DEBOUNCE_MS) {
    clearTimeout(timers.get(path));
    timers.set(path, setTimeout(() => { timers.delete(path); if (account !== null) exclusive(path, () => upload(path)); }, delay));
  }

  // The rules of saves.md section 5, for one path.
  async function reconcileOne(path, server) {
    const local = readLocal(path);
    if (!server && !local) return;
    if (!server) return upload(path);
    if (!local) return download(path);
    if (local.hash === server.hash) { remember(path, server.version, server.hash); return; }
    const synced = state[path];
    const localChanged = !synced || synced.hash !== local.hash;
    const serverChanged = !synced || synced.version !== server.version;
    if (!localChanged && serverChanged) return download(path);
    return upload(path); // local changed; if the host changed too, it decides by modification time
  }

  async function onManifest(message) {
    if (account !== message.account) { account = message.account; loadState(); }
    const byPath = new Map(message.files.map((f) => [f.path, f]));
    const paths = new Set([...byPath.keys(), ...localPaths()]);
    restored = 0;
    env.status(`Syncing ${paths.size} save file${paths.size === 1 ? '' : 's'} with the server...`);
    for (const path of paths) {
      await exclusive(path, () => reconcileOne(path, byPath.get(path)));
    }
    env.status(`Saves synced with the server for account ${account}${restored ? `; ${restored} restored` : ''}.`);
  }

  // ---- entry points for the runtime and the page ----

  function onControl(text) {
    let message;
    try { message = JSON.parse(text); } catch { env.log('saves: unreadable control message'); return; }
    const waiting = message.id !== undefined && pending.get(message.id);
    if (waiting) {
      clearTimeout(waiting.timer);
      pending.delete(message.id);
      waiting.resolve(message);
    } else if (message.op === 'manifest') {
      onManifest(message).catch((err) => env.log(`saves: ${err.message}`));
    } else if (message.op === 'error') {
      env.log(`saves: ${message.code}: ${message.detail}`);
    }
  }

  // The program closed a file it wrote, or renamed one onto this DOS path.
  function onFileClosed(dosPath) {
    const rel = canonical(dosPath.replace(/^[A-Za-z]:/, ''));
    if (rel && account !== null) schedule(rel);
  }

  // Page hidden, unloading, or the client exited: push whatever differs from the last sync now.
  function flushAll() {
    if (account === null) return;
    for (const timer of timers.values()) clearTimeout(timer);
    timers.clear();
    for (const path of localPaths()) exclusive(path, () => upload(path));
  }

  // A new COM1 connection starts without an account until its manifest arrives.
  function reset() { account = null; }

  function init(environment) { env = environment; }

  return { init, reset, onControl, onFileClosed, flushAll, sha256Hex, canonical };
})();
