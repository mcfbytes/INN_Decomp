// Sign-in, page side: the session token, the calls to the server's /auth/* endpoints, the popup
// handshake, and the files the signed-in account is written into. The contract with the server is
// "Sign-in (server contract)" in docs/web.md; page.js and adv.js own the screens. The encoder and the
// config editor are plain functions so tools/test_auth.js can run them under Node.
'use strict';

const InnAuth = (() => {
  const SESSION_KEY = 'inn.session', LINE_KEY = 'inn.line', PENDING_KEY = 'inn.signing';
  const POPUP_POLL_MS = 400, POPUP_GRACE_MS = 600;
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
        signal: opts.timeout ? AbortSignal.timeout(opts.timeout) : undefined,
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
      accounts: s.accounts.filter((a) => a && Number.isInteger(a.id) && (a.persona === null || typeof a.persona === 'string')),
      canCreate: !!s.canCreate,
    };
  }

  // Records the player's answer on the identity when the server holds an older (or no) attestation.
  const attestNow = (value) => call('POST', '/auth/attest', { attest: value });
  // The account has no persona yet: the client's own Select Player screen makes one (docs/web.md, "The persona on the drive").
  const createAccount = () => call('POST', '/auth/accounts', {});
  const credential = (id) => call('POST', `/auth/accounts/${id}/credential`);

  // The account's saved files, before the dial (innkeeperd `GET /auth/accounts/<id>/saves`): a listing with
  // the bytes (base64) of as many files as fit under 2 MiB, and one file by path for the rest.
  const SAVES_TIMEOUT_MS = 8000;
  const saves = (id) => call('GET', `/auth/accounts/${id}/saves`, undefined, { timeout: SAVES_TIMEOUT_MS });
  const saveFile = (id, path) => call('GET', `/auth/accounts/${id}/saves?path=${encodeURIComponent(path)}`, undefined, { timeout: SAVES_TIMEOUT_MS });

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
    encodePassword, passwordFile, setCfgId,
    configure, setAttest: (fn) => { st.attest = fn; }, attestNow, loadToken, saveToken, forget, loginUrl, call, fetchSession, createAccount, credential, saves, saveFile, logout,
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
