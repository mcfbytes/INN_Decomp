#!/usr/bin/env node
// Unit checks for runtime/web/auth.js: the stored-password encoder, the LSCI.CFG id edit, the login nonce
// (redirect fragment and popup message) and the sign-in method calls.
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
  await nonces();
  await popups();
  await methods();
  console.log('test_auth: ok');
})().catch((e) => { console.error(e); process.exit(1); });

// A page in Node: the address, its history and sessionStorage, all in memory.
function fakePage(hash) {
  const stored = new Map();
  const replaced = [];
  globalThis.sessionStorage = {
    getItem: (k) => (stored.has(k) ? stored.get(k) : null), setItem: (k, v) => stored.set(k, String(v)), removeItem: (k) => stored.delete(k),
  };
  globalThis.localStorage = globalThis.sessionStorage;
  globalThis.location = { origin: 'http://127.0.0.1:2781', pathname: '/inn.html', search: '?auth=x', hash };
  globalThis.history = { replaceState: (a, b, url) => replaced.push(url) };
  return { stored, replaced };
}

// A login carries a fresh 24-character base64url nonce, kept in sessionStorage; a redirect result is taken
// only with that nonce, and the address is cleaned either way.
async function nonces() {
  const a = A.newNonce(), b = A.newNonce();
  assert.match(a, /^[A-Za-z0-9_-]{24}$/);
  assert.notEqual(a, b);
  const page = fakePage('');
  const url = new URL(A.loginUrl('http://127.0.0.1:1/auth/login?provider=dev', 'redirect', 'TICKET'));
  const nonce = url.searchParams.get('nonce');
  assert.match(nonce, /^[A-Za-z0-9_-]{24}$/);
  assert.equal(page.stored.get('inn.nonce'), nonce);
  assert.equal(url.searchParams.get('link'), 'TICKET');
  assert.equal(url.searchParams.get('return'), 'http://127.0.0.1:2781/inn.html?auth=x');
  assert.equal(new URL(A.loginUrl('http://127.0.0.1:1/auth/login?provider=dev')).searchParams.has('link'), false);

  const good = fakePage('');
  const sent = new URL(A.loginUrl('http://127.0.0.1:1/auth/login?provider=dev', 'redirect')).searchParams.get('nonce');
  location.hash = `#inn-session=TOK&inn-nonce=${sent}`;
  A.state.token = '';
  A.readFragment();
  assert.equal(A.state.token, 'TOK');
  assert.equal(A.takeFragmentError(), '');
  assert.equal(good.stored.has('inn.nonce'), false, 'the nonce is used once');
  assert.deepEqual(good.replaced, ['/inn.html?auth=x']);
  assert.equal(A.takePending(), true);

  for (const hash of ['#inn-session=EVIL&inn-nonce=someone-elses-nonce-xx', '#inn-session=EVIL', '#inn-error=access_denied']) {
    const bad = fakePage(hash);
    A.state.token = '';
    A.readFragment();
    assert.equal(A.state.token, '', hash);
    assert.equal(A.takeFragmentError(), 'state_mismatch', hash);
    assert.deepEqual(bad.replaced, ['/inn.html?auth=x'], 'the address is cleaned anyway');
  }

  fakePage('');
  const mine = new URL(A.loginUrl('http://127.0.0.1:1/auth/login?provider=dev', 'redirect')).searchParams.get('nonce');
  location.hash = `#inn-link=PROOF&inn-nonce=${mine}`;
  A.readFragment();
  assert.equal(A.takeLinkProof(), 'PROOF');
  assert.equal(A.takeLinkProof(), '');
  fakePage('');
  const theirs = new URL(A.loginUrl('http://127.0.0.1:1/auth/login?provider=dev', 'redirect')).searchParams.get('nonce');
  location.hash = `#inn-error=identity_in_use&inn-nonce=${theirs}`;
  A.readFragment();
  assert.equal(A.takeFragmentError(), 'identity_in_use');
}

// The popup's message counts only from the auth origin, the window opened, and with this login's nonce. A link
// login opens the window blank and sends it on once the ticket is in.
async function popups() {
  fakePage('');
  A.configure('http://127.0.0.1:2780');
  const listeners = [];
  const opened = [];
  globalThis.window = {
    open: (url) => { const win = { closed: false, location: { href: url }, close() { this.closed = true; } }; opened.push(win); return win; },
    addEventListener: (type, fn) => listeners.push(fn),
    removeEventListener: (type, fn) => { const i = listeners.indexOf(fn); if (i >= 0) listeners.splice(i, 1); },
  };
  const send = (win, data, origin = 'http://127.0.0.1:2780') => [...listeners].forEach((fn) => fn({ origin, source: win, data }));
  const href = 'http://127.0.0.1:2780/auth/login?provider=dev';

  let pending = A.popupLogin(href);
  let win = opened.pop();
  const nonce = new URL(win.location.href).searchParams.get('nonce');
  send(win, { type: 'inn-auth', session: 'X', nonce }, 'https://evil.test');
  send({}, { type: 'inn-auth', session: 'X', nonce });
  send(win, { type: 'inn-auth', session: 'WRONG', nonce: 'not-the-nonce-of-this-login' });
  assert.deepEqual(await pending, { error: 'state_mismatch' });
  assert.notEqual(A.state.token, 'WRONG');

  pending = A.popupLogin(href);
  win = opened.pop();
  send(win, { type: 'inn-auth', session: 'GOOD', nonce: new URL(win.location.href).searchParams.get('nonce') });
  assert.deepEqual(await pending, { session: 'GOOD' });
  assert.equal(A.state.token, 'GOOD');

  pending = A.popupLogin(href, Promise.resolve('TICKET'));
  win = opened.pop();
  assert.equal(win.location.href, 'about:blank');
  await new Promise((resolve) => setTimeout(resolve, 0));
  const moved = new URL(win.location.href);
  assert.equal(moved.searchParams.get('link'), 'TICKET');
  assert.equal(moved.searchParams.get('mode'), 'popup');
  send(win, { type: 'inn-auth', link: 'PROOF', nonce: moved.searchParams.get('nonce') });
  assert.deepEqual(await pending, { link: 'PROOF' });

  const refused = new A.AuthError('reauth', 'again', 403, 'reauth_required');
  pending = A.popupLogin(href, Promise.reject(refused));
  win = opened.pop();
  assert.deepEqual(await pending, { failed: refused });
  assert.equal(win.closed, true);
  globalThis.window.open = () => null;
  assert.equal(A.popupLogin(href), null, 'a blocked popup');
}

// The session lists the sign-in methods (provider and date, never a subject); link and unlink answer the list,
// and a stale session's 403 reauth_required is its own kind.
async function methods() {
  const calls = [];
  const listed = [
    { id: '0123456789abcdef', provider: 'github', name: 'GitHub', linkedOn: '2026-10-17', current: true },
    { id: 'fedcba9876543210', provider: 'google', name: null, linkedOn: '2026-10-16', current: false },
    { id: 'short', provider: 'x', linkedOn: '2026-10-16' },
  ];
  globalThis.fetch = async (url, init) => {
    calls.push({ url, method: init.method, body: init.body, auth: init.headers.authorization });
    const json = (status, body) => ({ status, ok: status < 300, json: async () => body });
    if (url.endsWith('/auth/session')) return json(200, { identity: { handle: 'Caller 1', provider: 'github' }, accounts: [], identities: listed });
    if (url.endsWith('/auth/identities/ticket')) return calls.length > 2 ? json(403, { error: 'reauth_required', message: 'Sign in again first.' }) : json(200, { ticket: 'T', expiresIn: 600 });
    if (url.endsWith('/auth/identities/unlink')) return json(409, { error: 'last_identity', message: 'Only one.' });
    return json(200, { identities: listed.slice(0, 1) });
  };
  A.configure('http://127.0.0.1:2780');
  A.state.token = 'TOK';
  const s = await A.fetchSession();
  assert.deepEqual(s.methods, [
    { id: '0123456789abcdef', provider: 'github', name: 'GitHub', linkedOn: '2026-10-17', current: true },
    { id: 'fedcba9876543210', provider: 'google', name: '', linkedOn: '2026-10-16', current: false },
  ]);
  assert.equal(await A.linkTicket(), 'T');
  assert.deepEqual((await A.redeemLink('PROOF')).map((m) => m.id), ['0123456789abcdef']);
  assert.equal(calls[2].body, '{"proof":"PROOF"}');
  assert.equal(calls[2].auth, 'Bearer TOK');
  await assert.rejects(A.linkTicket(), (e) => e.kind === 'reauth' && e.code === 'reauth_required');
  await assert.rejects(A.unlink('fedcba9876543210'), (e) => e.kind === 'refused' && e.code === 'last_identity' && e.status === 409);
  assert.equal(calls[4].body, '{"id":"fedcba9876543210"}');
}
