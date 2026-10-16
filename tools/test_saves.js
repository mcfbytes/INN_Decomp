#!/usr/bin/env node
// Checks for the signed-in line's part of runtime/web/saves.js: one view of the persona files on the drive at
// a time (an account's, or the install's own), the HTTP prefetch, and PLAYER.DIR as the authority for which
// .DTA files come back. Runs against a small in-memory drive and a scripted host.
// Run: node tools/test_saves.js
'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');

const store = new Map();
globalThis.localStorage = {
  getItem: (k) => (store.has(k) ? store.get(k) : null),
  setItem: (k, v) => store.set(k, String(v)),
};
const InnSaves = require(path.join(__dirname, '..', 'runtime', 'web', 'saves.js'));

// A drive: path -> Uint8Array. Directories are implied.
const files = new Map();
const D = '/C';
const fs = {
  readdir: (dir) => {
    const seen = new Set(['.', '..']);
    let any = dir === D;
    for (const p of files.keys()) {
      if (p.startsWith(`${dir}/`)) { any = true; seen.add(p.slice(dir.length + 1).split('/')[0]); }
    }
    if (!any) throw new Error('ENOENT');
    return [...seen];
  },
  stat: (p) => {
    if (files.has(p)) return { mode: 'file', mtime: new Date(1000) };
    return { mode: 'dir', mtime: new Date(1000) };
  },
  isDir: (mode) => mode === 'dir',
  readFile: (p) => files.get(p),
  writeFile: (p, data) => files.set(p, Uint8Array.from(data)),
  mkdirTree: () => {},
  utime: () => {},
  unlink: (p) => { if (!files.delete(p)) throw new Error('ENOENT'); },
};
const put = (rel, text) => files.set(`${D}/${rel.replace(/\\/g, '/')}`, Buffer.from(text));
const get = (rel) => { const d = files.get(`${D}/${rel.replace(/\\/g, '/')}`); return d && Buffer.from(d).toString(); };

const sent = [];
let reply = () => null;
InnSaves.init({
  fs, drive: D, log: () => {}, persist: () => {}, notify: () => {}, saved: () => {}, status: () => {},
  send: (text) => {
    const m = JSON.parse(text);
    sent.push(m);
    setImmediate(() => InnSaves.onControl(JSON.stringify(reply(m))));
    return true;
  },
});
InnSaves.config({ sweep: 0 });

const hashOf = (text) => InnSaves.sha256Hex(Buffer.from(text));
const b64 = (text) => Buffer.from(text).toString('base64');
const wait = () => new Promise((r) => setTimeout(r, 50));

(async () => {
  const reset = () => { sent.length = 0; InnSaves.reset(); };
  // The persona files among what was sent (the character files are not this test's concern).
  const persona = () => sent.filter((m) => /PLAYER\.DIR$|\.DTA$/.test(m.path || ''));
  const snapshot = () => [...files.keys()].map((k) => k.slice(D.length + 1)).sort();

  // ---- views: the install's own personas, then a brand-new account, then back ----
  put('INN/PLAYER.DIR', 'Guybrush\r\n');
  put('INN/GUYBRUSH.DTA', 'own looks');
  put('INN/PASS_SET.DTA', 'secret');
  put('INN/LL/PLAYER.DIR', 'Guybrush\r\n');
  put('INN/YSERBIUS/MYCHARS.DAT', 'starter');
  assert.equal(InnSaves.activeView(), 'own');
  assert.equal(InnSaves.useView('own'), false, 'already showing the install\'s own personas');
  assert.equal(InnSaves.useView(100003), true);
  assert.equal(InnSaves.activeView(), '100003');
  assert.deepEqual(snapshot(), [
    'INN/PASS_SET.DTA', 'INN/YSERBIUS/MYCHARS.DAT', 'INNVIEWS/ACTIVE',
    'INNVIEWS/own/INN/GUYBRUSH.DTA', 'INNVIEWS/own/INN/LL/PLAYER.DIR', 'INNVIEWS/own/INN/PLAYER.DIR',
  ], 'a new account sees no PLAYER.DIR and no persona; the password and character files stay');
  // The client creates its persona for this account.
  put('INN/PLAYER.DIR', 'Zelda\r\n');
  put('INN/ZELDA.DTA', 'zelda looks');
  // Another account on the same install must not see Zelda; she waits for her account.
  assert.equal(InnSaves.useView(100004), true);
  assert.equal(get('INN/ZELDA.DTA'), undefined);
  assert.equal(get('INN/PLAYER.DIR'), undefined);
  assert.equal(get('INNVIEWS/100003/INN/ZELDA.DTA'), 'zelda looks');
  assert.equal(InnSaves.useView(100003), true);
  assert.equal(get('INN/ZELDA.DTA'), 'zelda looks');
  assert.equal(get('INN/PLAYER.DIR'), 'Zelda\r\n');
  assert.equal(get('INN/GUYBRUSH.DTA'), undefined, 'the install\'s persona is not in her view');
  // "My own number": the install's personas are back, byte for byte, and Zelda is stashed.
  assert.equal(InnSaves.useView('own'), true);
  assert.equal(get('INN/GUYBRUSH.DTA'), 'own looks');
  assert.equal(get('INN/PLAYER.DIR'), 'Guybrush\r\n');
  assert.equal(get('INN/LL/PLAYER.DIR'), 'Guybrush\r\n');
  assert.equal(get('INN/ZELDA.DTA'), undefined);
  assert.equal(get('INN/PASS_SET.DTA'), 'secret');

  // ---- a signed-in manifest syncs the persona files of the view, and only those ----
  InnSaves.useView(100003);
  const host = { 'INN\\ZELDA.DTA': 'zelda looks', 'INN\\PLAYER.DIR': 'Zelda\r\n' };
  reply = (m) => {
    if (m.op === 'get') return { id: m.id, op: 'file', path: m.path, data: b64(host[m.path]), hash: hashOf(host[m.path]), version: 3, mtime: 1000 };
    if (m.op === 'put') return { id: m.id, op: 'stored', version: 1, hash: hashOf('x') };
    throw new Error(`unexpected ${m.op}`);
  };
  const entries = (h, version = 3) => Object.entries(h).map(([p, d]) => ({ path: p, hash: hashOf(d), version, size: d.length, mtime: 1000 }));
  reset();
  InnSaves.onControl(JSON.stringify({ op: 'manifest', account: 100003, files: entries(host) }));
  await wait(); await wait();
  assert.deepEqual(persona(), [], 'the account already holds what the host has');

  // A brand-new account: the client's new persona is uploaded at the first manifest.
  InnSaves.useView(100005);
  put('INN/PLAYER.DIR', 'Link\r\n');
  put('INN/LINK.DTA', 'link looks');
  reset();
  InnSaves.onControl(JSON.stringify({ op: 'manifest', account: 100005, files: [] }));
  await wait(); await wait();
  assert.deepEqual(persona().filter((m) => m.op === 'put').map((m) => m.path).sort(), ['INN\\LINK.DTA', 'INN\\PLAYER.DIR']);

  // ---- PLAYER.DIR is authoritative: a persona the client deleted does not come back ----
  InnSaves.useView(100006);
  const full = { 'INN\\PLAYER.DIR': 'Elaine\r\n', 'INN\\ELAINE.DTA': 'elaine looks', 'INN\\OLDPERS.DTA': 'deleted in the client' };
  const listing = { account: 100006, files: entries(full, 4).map((e, i) => ({ ...e, data: b64(Object.values(full)[i]) })) };
  const done = await InnSaves.prefetch(100006, listing, async () => { throw new Error('unused'); });
  assert.deepEqual(done, { restored: 2, failed: 0 });
  assert.equal(get('INN/ELAINE.DTA'), 'elaine looks');
  assert.equal(get('INN/PLAYER.DIR'), 'Elaine\r\n');
  assert.equal(get('INN/OLDPERS.DTA'), undefined, 'a .DTA PLAYER.DIR does not name is not written');
  reset();
  InnSaves.onControl(JSON.stringify({ op: 'manifest', account: 100006, files: entries(full, 4) }));
  await wait(); await wait();
  assert.deepEqual(persona(), [], 'the manifest does not fetch it either');
  assert.equal(get('INN/OLDPERS.DTA'), undefined);
  // The client deletes Elaine (PLAYER.DIR is rewritten, the .DTA removed): nothing returns, and the new
  // empty list is uploaded.
  files.delete(`${D}/INN/ELAINE.DTA`);
  put('INN/PLAYER.DIR', '');
  reset();
  InnSaves.onControl(JSON.stringify({ op: 'manifest', account: 100006, files: entries(full, 4) }));
  await wait(); await wait();
  assert.equal(get('INN/ELAINE.DTA'), undefined, 'a deleted persona is not restored');
  assert.deepEqual(persona().map((m) => `${m.op} ${m.path}`), ['put INN\\PLAYER.DIR']);

  // ---- prefetch details ----
  // Files outside the allowlist or the password file are never taken; a file listed without bytes is fetched.
  InnSaves.useView(100007);
  reset();
  const real = { 'INN\\PLAYER.DIR': 'Elaine\r\n', 'INN\\ELAINE.DTA': 'real looks', 'INN\\YSERBIUS\\MYCHARS.DAT': 'hero', 'INN\\PASS_SET.DTA': 'x' };
  const list7 = {
    account: 100007,
    files: Object.entries(real).map(([p, d]) => ({ path: p, hash: hashOf(d), version: 4, size: d.length, mtime: 2000, ...(d === 'hero' ? {} : { data: b64(d) }) })),
  };
  const asked = [];
  const got = await InnSaves.prefetch(100007, list7, async (p) => { asked.push(p); return { data: b64(real[p]) }; });
  assert.deepEqual(got, { restored: 3, failed: 0 });
  assert.deepEqual(asked, ['INN\\YSERBIUS\\MYCHARS.DAT']);
  assert.equal(get('INN/ELAINE.DTA'), 'real looks');
  assert.equal(get('INN/PASS_SET.DTA'), 'secret', 'the password file is never taken');
  assert.deepEqual(JSON.parse(store.get('inn.saves.100007'))['INN\\ELAINE.DTA'], { version: 4, hash: hashOf('real looks') });
  const sync7 = Object.entries(real).filter(([p]) => !/PASS_SET/.test(p));
  InnSaves.onControl(JSON.stringify({ op: 'manifest', account: 100007, files: entries(Object.fromEntries(sync7), 4) }));
  await wait(); await wait();
  assert.deepEqual(sent, [], 'the first manifest neither uploads nor downloads');

  // A bad hash is refused and counted; a local file changed since the last sync is left to the manifest.
  InnSaves.reset();
  put('INN/ELAINE.DTA', 'edited here');
  const bad = { files: [{ path: 'INN\\ELAINE.DTA', hash: hashOf('real looks'), version: 5, size: 10, mtime: 1, data: b64('real looks') },
    { path: 'INN\\TWINION\\TWINCHAR.DAT', hash: hashOf('abc'), version: 1, size: 3, mtime: 1, data: b64('abd') }] };
  assert.deepEqual(await InnSaves.prefetch(100007, bad, async () => { throw new Error('unused'); }), { restored: 0, failed: 1 });
  assert.equal(get('INN/ELAINE.DTA'), 'edited here');
  assert.equal(get('INN/TWINION/TWINCHAR.DAT'), undefined);
  // An account this browser has never synced: the install's own character file gives way to the host's and is
  // kept as .OLD.
  InnSaves.reset();
  put('INN/YSERBIUS/MYCHARS.DAT', 'starter');
  const fresh = { files: [{ path: 'INN\\YSERBIUS\\MYCHARS.DAT', hash: hashOf('mine'), version: 2, size: 4, mtime: 9, data: b64('mine') }] };
  assert.deepEqual(await InnSaves.prefetch(100009, fresh, async () => { throw new Error('unused'); }), { restored: 1, failed: 0 });
  assert.equal(get('INN/YSERBIUS/MYCHARS.DAT'), 'mine');
  assert.equal(get('INN/YSERBIUS/MYCHARS.OLD'), 'starter');
  console.log('test_saves: ok');
})().catch((e) => { console.error(e); process.exit(1); });
