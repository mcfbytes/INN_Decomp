// Sign-in, page side: the session token, the calls to the server's /auth/* endpoints, the popup
// handshake, and the files the signed-in account is written into. The contract with the server is
// "Sign-in (server contract)" in docs/web.md; page.js and adv.js own the screens. The encoder and the
// config editor are plain functions so tools/test_auth.js can run them under Node.
'use strict';

const InnAuth = (() => {
  const SESSION_KEY = 'inn.session', LINE_KEY = 'inn.line', PENDING_KEY = 'inn.signing';
  const POPUP_POLL_MS = 400, POPUP_GRACE_MS = 600;
  // Name rule. The innkeeper docs set none for personas (docs/protocol/messages.md section 4.1 only
  // fixes the password), so this is the page's own: 1 to 10 of A-Z, a-z and 0-9. The server may be stricter.
  const PERSONA_RE = /^[A-Za-z0-9]{1,10}$/;
  // docs/protocol/messages.md section 4.1 item 2 (script.095 export 4); same table as PASSWORD_KEY in
  // TSN-INN-Decomp tools/dosbox/install_client.py.
  const PASSWORD_KEY = 'M34546788S';
  const PASSWORD_RE = /^[A-Za-z0-9]{6,10}$/;
  const PASSWORD_FILE_MAGIC = 'AB';

  const area = (kind) => { try { return kind === 'local' ? localStorage : sessionStorage; } catch { return null; } };
  const get = (kind, key) => { try { return area(kind).getItem(key); } catch { return null; } };
  const set = (kind, key, value) => { try { area(kind).setItem(key, value); } catch { /* kept in memory only */ } };
  const del = (kind, key) => { try { area(kind).removeItem(key); } catch { /* nothing to remove */ } };

  // ---------- the credential files ----------

  // The stored password form: uppercase, XOR byte i with PASSWORD_KEY[i], zero-pad to 10 bytes, then for
  // i = 0..n-1 XOR byte (i + 1) mod n with byte i (n is the password length).
  function encodePassword(password) {
    if (!PASSWORD_RE.test(password)) throw new Error('The password must be 6 to 10 letters or digits.');
    const plain = password.toUpperCase();
    const n = plain.length;
    const field = new Uint8Array(PASSWORD_KEY.length);
    for (let i = 0; i < n; i++) field[i] = plain.charCodeAt(i) ^ PASSWORD_KEY.charCodeAt(i);
    for (let i = 0, target = 1; i < n; i++, target = (target + 1) % n) field[target] ^= field[i];
    return field;
  }

  // PASS_SET.DTA: "AB", then the 10 encoded bytes (docs/dosbox.md section 2; messages.md section 4.1 item 1).
  function passwordFile(password) {
    const out = new Uint8Array(PASSWORD_FILE_MAGIC.length + PASSWORD_KEY.length);
    out[0] = PASSWORD_FILE_MAGIC.charCodeAt(0);
    out[1] = PASSWORD_FILE_MAGIC.charCodeAt(1);
    out.set(encodePassword(password), 2);
    return out;
  }

  // LSCI.CFG key ` id = <decimal>` is the account number (messages.md section 4.1 item 3). Replaces the
  // line when there is one, otherwise appends it, keeping the file's line ending.
  function setCfgId(text, account) {
    if (!Number.isInteger(account) || account < 1 || account > 0xffffffff) throw new Error('The account number is not valid.');
    const eol = /\r\n/.test(text) ? '\r\n' : '\n';
    const line = ` id = ${account}`;
    if (/^[ \t]*id[ \t]*=/im.test(text)) return text.replace(/^[ \t]*id[ \t]*=[^\r\n]*/im, line);
    return text + (text === '' || text.endsWith('\n') ? '' : eol) + line + eol;
  }

  // ---------- the persona files ----------
  // What the client's "Select Player" screen reads (docs/web.md, "The persona on the drive"). PLAYER.DIR is
  // the list of names, one per line, each ended by CR LF. A persona is <first 8 characters, upper case>.DTA,
  // all little-endian: the word 12 (the format marker), the home town as a word length (counting a
  // terminating zero byte), the text and the zero; eight words (shape, face features, misc features, two
  // hobby words, age, two words for the mail box number); the skills as a word 16 and 16 bytes; eight
  // preference bytes; nine words (two cursor positions and seven zeros).
  const PERSONA_MARK = 12, SKILLS = 16, SKILL_DEFAULT = 3, HOBBY_NONE = 0xfeff;
  const PREF_BYTES = [12, 0, 0, 7, 15, 3, 0, 0];
  const CURSOR_WORDS = [999, 997];
  const DEFAULT_CITY = 'Town', DEFAULT_AGE = 18;
  // The look is three words of bit fields, which the character editor limits to 0 .. count - 1:
  // [word, name, lowest bit, width, count]
  const LOOK = [
    ['shape', 'female', 0, 1, 2], ['shape', 'skin', 1, 3, 5], ['shape', 'head', 4, 3, 8],
    ['shape', 'faceHair', 7, 3, 8], ['shape', 'eyebrows', 10, 3, 8],
    ['faceFeature', 'glasses', 0, 3, 8], ['faceFeature', 'eyes', 3, 3, 8], ['faceFeature', 'nose', 6, 3, 8],
    ['faceFeature', 'mouth', 9, 3, 8], ['faceFeature', 'hairColor', 12, 4, 14],
    ['miscFeature', 'clothes', 0, 4, 13], ['miscFeature', 'hairStyle', 4, 5, 25], ['miscFeature', 'hats', 9, 4, 12],
    ['miscFeature', 'clothesColor', 13, 3, 8],
  ];

  // A small seeded generator (mulberry32) so one account always gets the same face.
  function rng(seed) {
    let a = seed >>> 0;
    return () => {
      a = (a + 0x6d2b79f5) >>> 0;
      let t = a;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }

  // Packs choices ({ name: value }, missing ones 0) into the three words.
  function packLook(choices) {
    const words = { shape: 0, faceFeature: 0, miscFeature: 0 };
    for (const [word, name, shift, , count] of LOOK) {
      const v = choices[name] || 0;
      if (!Number.isInteger(v) || v < 0 || v >= count) throw new Error(`The look "${name}" must be 0 to ${count - 1}.`);
      words[word] |= v << shift;
    }
    return words;
  }

  function unpackLook(words) {
    const out = {};
    for (const [word, name, shift, bits] of LOOK) out[name] = (words[word] >> shift) & ((1 << bits) - 1);
    return out;
  }

  const randomLook = (seed) => {
    const next = rng(seed), choices = {};
    for (const [, name, , , count] of LOOK) choices[name] = Math.floor(next() * count);
    return packLook(choices);
  };

  // The first 8 characters, upper case: the client's file name for a persona.
  const personaStem = (persona) => persona.slice(0, 8).toUpperCase();
  const playerDir = (persona) => new TextEncoder().encode(`${persona}\r\n`);

  function personaFile(persona, account, opts = {}) {
    const city = new TextEncoder().encode(opts.city === undefined ? DEFAULT_CITY : opts.city);
    const look = opts.look || randomLook(account);
    const out = new Uint8Array(4 + city.length + 1 + 16 + 2 + SKILLS + PREF_BYTES.length + 18);
    const view = new DataView(out.buffer);
    let at = 0;
    const word = (v) => { view.setUint16(at, v, true); at += 2; };
    const byte = (v) => { out[at++] = v; };
    word(PERSONA_MARK);
    word(city.length + 1);
    out.set(city, at); at += city.length; byte(0);
    [look.shape, look.faceFeature, look.miscFeature, HOBBY_NONE, HOBBY_NONE, opts.age || DEFAULT_AGE, 0, 0].forEach(word);
    word(SKILLS);
    for (let i = 0; i < SKILLS; i++) byte(SKILL_DEFAULT);
    PREF_BYTES.forEach(byte);
    CURSOR_WORDS.forEach(word);
    for (let i = 0; i < 7; i++) word(0);
    return out;
  }

  const personaProblem = (name) => (PERSONA_RE.test(name) ? '' : 'Use 1 to 10 letters or digits, with no spaces.');

  // ---------- the session ----------

  const st = { base: '', origin: '', token: '', attest: null, identity: null, provider: '', pending: false, fragmentError: '' };

  // A redirect login comes back with the result in the address fragment. Take it and clean the address.
  function readFragment() {
    const h = new URLSearchParams(location.hash.replace(/^#/, ''));
    const session = h.get('inn-session'), error = h.get('inn-error');
    if (!session && !error) return;
    if (session) { st.token = session; st.pending = true; saveToken(); }
    if (error) st.fragmentError = error;
    try { history.replaceState(null, '', location.pathname + location.search); } catch { /* address left as it is */ }
  }

  // sessionStorage always; localStorage also when "Remember on this device" is on.
  function saveToken(remember) {
    set('session', SESSION_KEY, st.token);
    if (remember === true) set('local', SESSION_KEY, st.token);
    else if (remember === false) del('local', SESSION_KEY);
  }

  function loadToken() {
    if (st.token) return st.token;
    st.token = get('session', SESSION_KEY) || get('local', SESSION_KEY) || '';
    return st.token;
  }

  function forget() {
    st.token = '';
    st.identity = null;
    st.provider = '';
    del('session', SESSION_KEY);
    del('local', SESSION_KEY);
    del('session', LINE_KEY);
  }

  function configure(base) {
    st.base = base;
    try { st.origin = new URL(base).origin; } catch { st.origin = ''; }
  }

  // The page's own address without the fragment: where the server sends the player back to.
  const returnUrl = () => location.origin + location.pathname + location.search;

  function loginUrl(href, mode) {
    const url = new URL(href);
    url.searchParams.set('return', returnUrl());
    if (mode) url.searchParams.set('mode', mode);
    // "<age>+v<terms>": the player's answer to the age and terms notice (docs/web.md, Sign-in).
    const attest = st.attest && st.attest();
    if (attest) url.searchParams.set('attest', attest);
    return url.href;
  }

  class AuthError extends Error {
    constructor(kind, message, status, code) { super(message); this.kind = kind; this.status = status || 0; this.code = code || ''; }
  }

  // kind: 'network' (no answer), 'expired' (401), 'closed' (403 enrolment_closed), 'attest' (403
  // attest_required: the server wants the age and terms answer first), 'limit' (429: rate_limited), 'refused'
  // (other 4xx with the server's words), 'server' (5xx, such as 503 busy). `code` is the server's error code.
  async function call(method, path, body, opts = {}) {
    let res;
    try {
      res = await fetch(`${st.base}${path}`, {
        method,
        headers: Object.assign({ accept: 'application/json' }, st.token ? { authorization: `Bearer ${st.token}` } : {},
          body === undefined ? {} : { 'content-type': 'application/json' }),
        body: body === undefined ? undefined : JSON.stringify(body),
      });
    } catch { throw new AuthError('network', 'The sign-in server did not answer.'); }
    if (res.status === 204 && opts.empty !== false) return null;
    let data = null;
    try { data = await res.json(); } catch { /* no body */ }
    if (res.ok) return data;
    const code = data && data.error;
    const words = (data && data.message) || `The server answered ${res.status}.`;
    if (res.status === 401) throw new AuthError('expired', words, 401, code);
    if (res.status === 403 && code === 'enrolment_closed') throw new AuthError('closed', words, 403, code);
    if (res.status === 403 && code === 'attest_required') throw new AuthError('attest', words, 403, code);
    if (res.status === 429) throw new AuthError('limit', words, 429, code);
    if (res.status >= 500) throw new AuthError('server', words, res.status, code);
    throw new AuthError('refused', words, res.status, code);
  }

  async function fetchSession() {
    const s = await call('GET', '/auth/session');
    if (!s || !s.identity || typeof s.identity.handle !== 'string' || !Array.isArray(s.accounts)) throw new AuthError('server', 'The session answer was not understood.', 200);
    st.identity = s.identity;
    st.provider = s.identity.provider || '';
    const at = s.attested;
    return {
      identity: s.identity,
      attested: at && Number.isInteger(at.age) && Number.isInteger(at.terms) ? { age: at.age, terms: at.terms } : null,
      accounts: s.accounts.filter((a) => a && Number.isInteger(a.id) && typeof a.persona === 'string'),
      canCreate: !!s.canCreate,
    };
  }

  // Records the player's answer on the identity when the server holds an older (or no) attestation.
  const attestNow = (value) => call('POST', '/auth/attest', { attest: value });
  const createAccount = (persona) => call('POST', '/auth/accounts', { persona });
  const credential = (id) => call('POST', `/auth/accounts/${id}/credential`);

  async function logout() {
    try { await call('POST', '/auth/logout'); } catch { /* the token is forgotten either way */ }
    forget();
  }

  // Opens the login in a popup. Call it straight from the click so the browser allows it. Resolves
  // { session }, or { error: '<code>' } when the server reports one ('access_denied' for Deny), or
  // { closed: true } when the window goes away. Resolves null when the popup is blocked.
  function popupLogin(href) {
    let win = null;
    try { win = window.open(loginUrl(href, 'popup'), 'inn-auth', 'popup=yes,width=520,height=660'); } catch { win = null; }
    if (!win) return null;
    let done = null;
    const result = new Promise((resolve) => { done = resolve; });
    let poll = 0, grace = 0;
    const finish = (value) => {
      clearInterval(poll); clearTimeout(grace);
      window.removeEventListener('message', onMessage);
      done(value);
    };
    function onMessage(e) {
      const d = e.data;
      if (e.origin !== st.origin || e.source !== win || !d || d.type !== 'inn-auth') return;
      if (typeof d.session === 'string' && d.session) {
        st.token = d.session;
        finish({ session: d.session });
      } else finish({ error: typeof d.error === 'string' ? d.error : 'failed' });
    }
    window.addEventListener('message', onMessage);
    poll = setInterval(() => {
      if (win.closed && !grace) grace = setTimeout(() => finish({ closed: true }), POPUP_GRACE_MS);
    }, POPUP_POLL_MS);
    result.cancel = () => { try { win.close(); } catch { /* already gone */ } finish({ closed: true }); };
    return result;
  }

  // The fallback: leave the page for the login and come back with #inn-session=<token>.
  function redirectLogin(href) {
    set('session', PENDING_KEY, '1');
    location.href = loginUrl(href, 'redirect');
  }

  if (typeof location !== 'undefined') readFragment();

  const api = {
    encodePassword, passwordFile, setCfgId, personaProblem, PERSONA_RE,
    personaFile, playerDir, personaStem, packLook, unpackLook, randomLook, LOOK,
    configure, setAttest: (fn) => { st.attest = fn; }, attestNow, loadToken, saveToken, forget, loginUrl, call, fetchSession, createAccount, credential, logout,
    popupLogin, redirectLogin, AuthError,
    state: st,
    token: () => st.token || loadToken(),
    line: () => get('session', LINE_KEY) || '',
    setLine: (v) => set('session', LINE_KEY, String(v)),
    takeFragmentError: () => { const e = st.fragmentError; st.fragmentError = ''; return e; },
    takePending: () => { const p = st.pending; st.pending = false; del('session', PENDING_KEY); return p; },
  };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  return api;
})();
