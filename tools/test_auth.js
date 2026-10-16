#!/usr/bin/env node
// Unit checks for runtime/web/auth.js: the stored-password encoder and the LSCI.CFG id edit.
// The known pair is the live capture in TSN-INN-Decomp docs/protocol/captures.md section 4: the
// password SWORDFISH (docs/dosbox.md) went out as `1d 7a 01 66 16 66 18 73 03 00`.
// Run: node tools/test_auth.js
'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const A = require(path.join(__dirname, '..', 'runtime', 'web', 'auth.js'));

const hex = (b) => Buffer.from(b).toString('hex');

assert.equal(hex(A.encodePassword('SWORDFISH')), '1d7a0166166618730300');
assert.equal(hex(A.encodePassword('swordfish')), '1d7a0166166618730300', 'lower case is uppercased');
assert.equal(hex(A.passwordFile('SWORDFISH')), '41421d7a0166166618730300', 'AB + 10 bytes');
assert.equal(A.encodePassword('ABCDEF').length, 10, 'short passwords are zero-padded to 10 bytes');
assert.deepEqual([...A.encodePassword('ABCDEF').slice(6)], [0, 0, 0, 0]);
for (const bad of ['SHORT', 'WAYTOOLONGPW', 'HAS SPACE1', 'BAD-CHARS1', ''])
  assert.throws(() => A.encodePassword(bad), /6 to 10/);

const cfg = ' video = vga320.drv\r\n id = 100001\r\n hostID = SIERRA\r\n';
assert.equal(A.setCfgId(cfg, 123456), ' video = vga320.drv\r\n id = 123456\r\n hostID = SIERRA\r\n');
assert.equal(A.setCfgId('a = b\n', 7), 'a = b\n id = 7\n', 'appended with the file\'s own line ending');
assert.equal(A.setCfgId('a = b', 7), 'a = b\n id = 7\n');
assert.equal(A.setCfgId(' hostid = X\r\n', 7), ' hostid = X\r\n id = 7\r\n', 'hostID is not id');
assert.throws(() => A.setCfgId(cfg, 0));
assert.throws(() => A.setCfgId(cfg, 1.5));

// The page no longer writes persona files: the account has none until the client's Select Player makes one.
for (const gone of ['personaFile', 'playerDir', 'personaProblem', 'randomLook']) assert.equal(A[gone], undefined, gone);

// A session answer carries `persona: null` for an account the client has not named yet, and a new account is
// asked for with an empty body object.
(async () => {
  const calls = [];
  globalThis.fetch = async (url, init) => {
    calls.push({ url, method: init.method, body: init.body });
    const json = (status, body) => ({ status, ok: status < 300, json: async () => body });
    if (url.endsWith('/auth/session')) {
      return json(200, { identity: { handle: 'Caller 7F3A', provider: 'x' }, canCreate: true, attested: null,
        accounts: [{ id: 100003, persona: null }, { id: 100004, persona: 'Zelda' }, { id: 'bad', persona: 'No' }] });
    }
    return json(201, { id: 100005, persona: null });
  };
  A.configure('http://127.0.0.1:1');
  const s = await A.fetchSession();
  assert.deepEqual(s.accounts, [{ id: 100003, persona: null }, { id: 100004, persona: 'Zelda' }]);
  const made = await A.createAccount();
  assert.deepEqual(made, { id: 100005, persona: null });
  assert.equal(calls[1].body, '{}');
  console.log('test_auth: ok');
})().catch((e) => { console.error(e); process.exit(1); });
