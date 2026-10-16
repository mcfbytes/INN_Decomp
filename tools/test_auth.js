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

assert.equal(A.personaProblem('Guybrush'), '');
assert.equal(A.personaProblem('A'), '');
assert.equal(A.personaProblem('Abcdefghij'), '');
for (const bad of ['', 'Abcdefghijk', 'two words', 'a-b', 'é'])
  assert.notEqual(A.personaProblem(bad), '', bad);

// The persona files (docs/web.md, "The persona on the drive").
assert.equal(hex(A.playerDir('Elaine')), Buffer.from('Elaine\r\n').toString('hex'));
assert.equal(A.personaStem('Elaine'), 'ELAINE');
assert.equal(A.personaStem('Abcdefghij'), 'ABCDEFGH', 'the client keeps the first 8 characters');
const mine = A.packLook({ female: 1, skin: 4, head: 7, faceHair: 5, eyebrows: 3, glasses: 2, eyes: 6, nose: 1, mouth: 7,
  hairColor: 13, clothes: 12, hairStyle: 24, hats: 11, clothesColor: 7 });
assert.deepEqual(A.unpackLook(mine), { female: 1, skin: 4, head: 7, faceHair: 5, eyebrows: 3, glasses: 2, eyes: 6, nose: 1, mouth: 7,
  hairColor: 13, clothes: 12, hairStyle: 24, hats: 11, clothesColor: 7 });
assert.throws(() => A.packLook({ hairColor: 14 }), /0 to 13/);
assert.throws(() => A.packLook({ hairStyle: 25 }), /0 to 24/);
for (const account of [100001, 100002, 4294967295, 7]) {
  const look = A.randomLook(account);
  const fields = A.unpackLook(look);
  for (const [, name, , , count] of A.LOOK) assert.ok(fields[name] >= 0 && fields[name] < count, `${name} in range`);
  assert.equal(hex(Buffer.from(JSON.stringify(look))), hex(Buffer.from(JSON.stringify(A.randomLook(account)))), 'same account, same face');
}
assert.notEqual(JSON.stringify(A.randomLook(100001)), JSON.stringify(A.randomLook(100002)));
const dta = A.personaFile('Elaine', 100002, { look: mine, age: 21, city: 'Maine' });
assert.equal(dta.length, 65 + 'Maine'.length, 'a persona file is 65 bytes plus the home town');
const dv = new DataView(dta.buffer);
assert.equal(dv.getUint16(0, true), 12, 'format marker');
assert.equal(dv.getUint16(2, true), 6, 'town length counts the zero');
assert.equal(Buffer.from(dta.subarray(4, 10)).toString('latin1'), 'Maine\0');
assert.equal(dv.getUint16(10, true), mine.shape);
assert.equal(dv.getUint16(12, true), mine.faceFeature);
assert.equal(dv.getUint16(14, true), mine.miscFeature);
assert.equal(dv.getUint16(16, true), 0xfeff);
assert.equal(dv.getUint16(18, true), 0xfeff);
assert.equal(dv.getUint16(20, true), 21, 'age');
assert.equal(dv.getUint16(26, true), 16, 'skills');
assert.deepEqual([...dta.subarray(28, 44)], Array(16).fill(3));
assert.deepEqual([...dta.subarray(44, 52)], [12, 0, 0, 7, 15, 3, 0, 0]);
assert.equal(dv.getUint16(52, true), 999);
assert.equal(dv.getUint16(54, true), 997);
assert.equal(A.personaFile('Elaine', 1).length, 69, 'the default home town is four letters');

console.log('test_auth: ok');
