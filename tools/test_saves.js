#!/usr/bin/env node
// Checks for the signed-in line's part of runtime/web/saves.js: the persona filter and the made-up files
// whose server copy wins. Runs against a small in-memory drive and a scripted host.
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
  // Setting the line: another persona's file and the hub's PLAYER.DIR stay out; this persona's goes in.
  put('INN/PLAYER.DIR', 'Elaine\r\n');
  put('INN/GUYBRUSH.DTA', 'someone else');
  put('INN/ELAINE.DTA', 'made up');
  put('INN/PASS_SET.DTA', 'secret');
  InnSaves.setLine('Elaine');
  InnSaves.seed(100002, 'INN\\ELAINE.DTA', hashOf('made up'));

  // The host already has the account's real ELAINE.DTA (another browser) and a PLAYER.DIR of its own.
  const host = { 'INN\\ELAINE.DTA': 'real looks', 'INN\\PLAYER.DIR': 'Elaine\r\nOther\r\n' };
  reply = (m) => {
    if (m.op === 'get') return { id: m.id, op: 'file', path: m.path, data: b64(host[m.path]), hash: hashOf(host[m.path]), version: 3, mtime: 1000 };
    if (m.op === 'put') return { id: m.id, op: 'stored', version: 1, hash: hashOf('x') };
    throw new Error(`unexpected ${m.op}`);
  };
  const manifest = Object.entries(host).map(([p, d]) => ({ path: p, hash: hashOf(d), version: 3, size: d.length, mtime: 1000 }));
  InnSaves.onControl(JSON.stringify({ op: 'manifest', account: 100002, files: manifest }));
  await wait(); await wait();
  assert.equal(get('INN/ELAINE.DTA'), 'real looks', 'the host copy replaces the made-up one');
  assert.equal(get('INN/PLAYER.DIR'), 'Elaine\r\n', 'PLAYER.DIR is not taken from the host');
  assert.equal(get('INN/GUYBRUSH.DTA'), 'someone else');
  assert.ok(sent.every((m) => m.op === 'get' && m.path === 'INN\\ELAINE.DTA'), `only ELAINE.DTA was asked for: ${JSON.stringify(sent)}`);
  assert.equal(JSON.parse(store.get('inn.seed.100002')).hasOwnProperty('INN\\ELAINE.DTA'), false, 'the seed is spent');

  // A new account whose host has nothing: the made-up file is uploaded, the other persona and PLAYER.DIR are not.
  sent.length = 0;
  InnSaves.reset();
  put('INN/ELAINE.DTA', 'made up');
  InnSaves.seed(100005, 'INN\\ELAINE.DTA', hashOf('made up'));
  InnSaves.onControl(JSON.stringify({ op: 'manifest', account: 100005, files: [] }));
  await wait(); await wait();
  const puts = sent.filter((m) => m.op === 'put').map((m) => m.path);
  assert.deepEqual(puts, ['INN\\ELAINE.DTA']);

  // Without a line (the classic "my own number"), everything is in the save space again.
  sent.length = 0;
  InnSaves.reset();
  InnSaves.setLine(null);
  InnSaves.onControl(JSON.stringify({ op: 'manifest', account: 100001, files: [] }));
  await wait(); await wait();
  const all = sent.filter((m) => m.op === 'put').map((m) => m.path).sort();
  assert.deepEqual(all, ['INN\\ELAINE.DTA', 'INN\\GUYBRUSH.DTA', 'INN\\PLAYER.DIR']);
  // Before the dial: the HTTP listing lands in the drive, recorded as synced, and the first manifest does nothing.
  sent.length = 0;
  InnSaves.reset();
  for (const k of [...files.keys()]) files.delete(k);
  put('INN/GUYBRUSH.DTA', 'someone else');
  InnSaves.setLine('Elaine');
  const real = { 'INN\\ELAINE.DTA': 'real looks', 'INN\\YSERBIUS\\MYCHARS.DAT': 'hero', 'INN\\PASS_SET.DTA': 'x', 'INN\\GUYBRUSH.DTA': 'no' };
  const listing = {
    account: 100007,
    files: Object.entries(real).map(([p, d]) => ({ path: p, hash: hashOf(d), version: 4, size: d.length, mtime: 2000, ...(d === 'hero' ? {} : { data: b64(d) }) })),
  };
  const asked = [];
  const done = await InnSaves.prefetch(100007, listing, async (p) => { asked.push(p); return { data: b64(real[p]) }; });
  assert.deepEqual(done, { restored: 2, failed: 0 });
  assert.deepEqual(asked, ['INN\\YSERBIUS\\MYCHARS.DAT'], 'a file listed without bytes is fetched by path');
  assert.equal(get('INN/ELAINE.DTA'), 'real looks');
  assert.equal(get('INN/YSERBIUS/MYCHARS.DAT'), 'hero');
  assert.equal(get('INN/PASS_SET.DTA'), undefined, 'the password file is never taken');
  assert.equal(get('INN/GUYBRUSH.DTA'), 'someone else', 'another persona is left alone');
  assert.deepEqual(JSON.parse(store.get('inn.saves.100007'))['INN\\ELAINE.DTA'], { version: 4, hash: hashOf('real looks') });
  const entries = Object.entries(real).filter(([p]) => !/PASS_SET|GUYBRUSH/.test(p));
  InnSaves.onControl(JSON.stringify({ op: 'manifest', account: 100007, files: entries.map(([p, d]) => ({ path: p, hash: hashOf(d), version: 4, size: d.length, mtime: 2000 })) }));
  await wait(); await wait();
  assert.deepEqual(sent, [], 'the first manifest neither uploads nor downloads');

  // A bad hash is refused and counted; a local file changed since the last sync is left to the manifest.
  InnSaves.reset();
  put('INN/ELAINE.DTA', 'edited here');
  const bad = { files: [{ path: 'INN\\ELAINE.DTA', hash: hashOf('real looks'), version: 5, size: 10, mtime: 1, data: b64('real looks') },
    { path: 'INN\\TWINION\\TWINCHAR.DAT', hash: hashOf('abc'), version: 1, size: 3, mtime: 1, data: b64('abd') }] };
  const second = await InnSaves.prefetch(100007, bad, async () => { throw new Error('unused'); });
  assert.deepEqual(second, { restored: 0, failed: 1 });
  assert.equal(get('INN/ELAINE.DTA'), 'edited here');
  assert.equal(get('INN/TWINION/TWINCHAR.DAT'), undefined);
  // An account this browser has never synced: the install's own character file gives way to the host's and is
  // kept as .OLD; the next account of the same browser, already synced once, would not be touched this way.
  InnSaves.reset();
  put('INN/YSERBIUS/MYCHARS.DAT', 'starter');
  const fresh = { files: [{ path: 'INN\\YSERBIUS\\MYCHARS.DAT', hash: hashOf('mine'), version: 2, size: 4, mtime: 9, data: b64('mine') }] };
  assert.deepEqual(await InnSaves.prefetch(100009, fresh, async () => { throw new Error('unused'); }), { restored: 1, failed: 0 });
  assert.equal(get('INN/YSERBIUS/MYCHARS.DAT'), 'mine');
  assert.equal(get('INN/YSERBIUS/MYCHARS.OLD'), 'starter');
  console.log('test_saves: ok');
})().catch((e) => { console.error(e); process.exit(1); });
