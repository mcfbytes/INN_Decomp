// Anonymous diagnostics for the operator's Azure Application Insights, with no third-party script: this file
// builds the v2 envelopes and posts them to <IngestionEndpoint>/v2/track itself. It is on only when site.json
// has telemetry.connectionString, and off when the browser says Global Privacy Control or Do Not Track, or the
// visitor turned the Settings switch off. It sends the things listed in docs/web.md ("Diagnostics") and nothing
// else: no cookie, no persistent ID (the session ID is random per page load and lives in memory), no query
// string or hash, no server address, account number, handle, chat or file name. The scrubbing and the envelope
// building are plain functions so tools/test_telemetry.js can run them under Node. Loaded before everything else.
'use strict';

const Telemetry = (() => {
  const PREF_KEY = 'inn.diag';           // '0' = the visitor turned diagnostics off; anything else = on
  const SDK = 'web-diag:1';
  const FLUSH_MS = 5000, BATCH_MAX = 20, QUEUE_MAX = 60, BODY_MAX = 56000;   // fetch keepalive bodies may not pass 64 KB
  const STACK_MAX = 2048, MESSAGE_MAX = 1024, NAME_MAX = 64, PROP_MAX = 64;
  const GUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

  // ---- pure helpers ----

  // "InstrumentationKey=<guid>;IngestionEndpoint=https://host/;..." -> { iKey, url } or null. The endpoint must be
  // https (http only for a loopback address, which the local test page uses); the track URL is its origin + /v2/track.
  function parseConnectionString(s) {
    if (typeof s !== 'string') return null;
    const parts = {};
    for (const piece of s.split(';')) {
      const i = piece.indexOf('=');
      if (i > 0) parts[piece.slice(0, i).trim().toLowerCase()] = piece.slice(i + 1).trim();
    }
    const iKey = parts.instrumentationkey || '';
    if (!GUID.test(iKey)) return null;
    let u;
    try { u = new URL(parts.ingestionendpoint || ''); } catch { return null; }
    const loopback = u.hostname === '127.0.0.1' || u.hostname === 'localhost';
    if (u.protocol !== 'https:' && !(u.protocol === 'http:' && loopback)) return null;
    return { iKey: iKey.toLowerCase(), url: `${u.origin}/v2/track` };
  }

  // Text that may hold addresses, account numbers or paths from the player's install, made safe to send.
  // Script URLs become file names (no host, directory, query or hash); ws, wss and any other address becomes
  // [address]; digit runs of five or more (account numbers) become [number]; paths on the C: drive are cut.
  const URL_RE = /\b[a-z][a-z0-9+.-]*:\/\/[^\s'"<>()]+/gi;
  const HOSTPORT_RE = /\b(?:\d{1,3}(?:\.\d{1,3}){3}|localhost|[a-z0-9-]+(?:\.[a-z0-9-]+)+):\d{2,5}\b/gi;
  const IPV4_RE = /\b\d{1,3}(?:\.\d{1,3}){3}\b/g;
  function scrubPlain(t) {
    return t
      .replace(HOSTPORT_RE, '[address]')
      .replace(IPV4_RE, '[address]')
      .replace(/[^\s@'"<>]+@[^\s@'"<>]+\.[a-z]{2,}/gi, '[email]')
      .replace(/(^|[\s'"(])\/c\/[^\s'"():]*/gi, '$1/c/[path]')
      .replace(/(^|[\s'"(])[a-z]:[\\/][^\s'"():]*/gi, '$1[path]')
      .replace(/\b\d{5,}\b/g, '[number]');
  }
  function scrubText(text) {
    const s = String(text == null ? '' : text);
    let out = '', last = 0, m;
    URL_RE.lastIndex = 0;
    while ((m = URL_RE.exec(s))) {
      out += scrubPlain(s.slice(last, m.index));
      const u = m[0];
      if (/^https?:/i.test(u)) {
        // Keep ":line:col" so the frame still points somewhere, but only the file name of the address.
        const tail = /(:\d+){1,2}$/.exec(u);
        const bare = u.slice(0, tail ? tail.index : u.length).replace(/[?#].*$/, '');
        const name = bare.replace(/^[a-z]+:\/\/[^/]*\/?/i, '').split('/').pop();
        out += name ? name + (tail ? tail[0] : '') : '[address]';
      } else if (/^wasm:/i.test(u)) out += 'wasm:' + u.split('/').pop();   // a frame inside the runtime's own code
      else out += '[address]';
      last = m.index + u.length;
    }
    return out + scrubPlain(s.slice(last));
  }
  const clip = (s, n) => (s.length > n ? s.slice(0, n) : s);
  const scrubMessage = (t) => clip(scrubText(t), MESSAGE_MAX);
  const scrubStack = (t) => clip(scrubText(t), STACK_MAX);

  // Property values are short labels chosen by this code. Anything else is cut down to a safe label.
  const label = (v) => clip(String(v == null ? '' : v).replace(/[^\w.+-]/g, '_'), PROP_MAX);

  // The page address without query string or hash (the sign-in redirect carries a token in the hash).
  function pathOf(loc) {
    const p = (loc && loc.pathname) || '/';
    return p.startsWith('/') ? p : `/${p}`;
  }

  function duration(ms) {
    ms = Math.max(0, Math.round(Number.isFinite(ms) ? ms : 0));
    const p = (n, w) => String(n).padStart(w, '0');
    return `${p(Math.floor(ms / 3600000), 2)}:${p(Math.floor(ms / 60000) % 60, 2)}:${p(Math.floor(ms / 1000) % 60, 2)}.${p(ms % 1000, 3)}`;
  }

  // How long something took, in a few coarse buckets, so a connect result says roughly how long and no more.
  function bucket(ms) {
    const s = ms / 1000;
    return s < 2 ? 'under-2s' : s < 5 ? '2-5s' : s < 15 ? '5-15s' : s < 60 ? '15-60s' : s < 300 ? '1-5min' : s < 1800 ? '5-30min' : 'over-30min';
  }

  // One Application Insights v2 envelope. No user, device or location tags: the service derives browser and OS
  // from the User-Agent header itself, and the session ID is the random in-memory one.
  function envelope(conn, sessionId, path, type, base, now = new Date()) {
    const names = { EventData: 'Event', PageviewData: 'Pageview', ExceptionData: 'Exception' };
    return {
      name: `Microsoft.ApplicationInsights.${conn.iKey.replace(/-/g, '')}.${names[type]}`,
      time: now.toISOString(),
      iKey: conn.iKey,
      tags: { 'ai.session.id': sessionId, 'ai.operation.name': path, 'ai.internal.sdkVersion': SDK },
      data: { baseType: type, baseData: Object.assign({ ver: 2 }, base) },
    };
  }

  const pageviewBase = (path, ms) => ({ name: path, url: path, duration: duration(ms) });   // the path is the whole address we send
  const eventBase = (name, props, measures) => ({ name: label(name).slice(0, NAME_MAX), properties: props || {}, measurements: measures || {} });
  function exceptionBase(error, extra) {
    const isErr = error && typeof error === 'object';
    const type = label(isErr && error.name ? error.name : 'Error');
    const msg = scrubMessage(isErr && 'message' in error ? error.message : error);
    const stack = isErr && typeof error.stack === 'string' ? scrubStack(error.stack) : '';
    return { exceptions: [{ typeName: type, message: msg || type, hasFullStack: !!stack, stack }], severityLevel: 3, properties: extra || {} };
  }

  const randomId = () => {
    const b = new Uint8Array(8);
    (typeof crypto !== 'undefined' && crypto.getRandomValues ? crypto : { getRandomValues: (a) => a.map(() => Math.floor(Math.random() * 256)) }).getRandomValues(b);
    return Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
  };

  const pure = { parseConnectionString, scrubText, scrubMessage, scrubStack, label, pathOf, duration, bucket, envelope, pageviewBase, eventBase, exceptionBase };

  // ---- the live module (browser only) ----

  if (typeof window === 'undefined') {
    if (typeof module !== 'undefined' && module.exports) module.exports = pure;
    return pure;
  }

  const sessionId = randomId();   // per page load, in memory only
  let conn = null;                // set by init() when site.json has a usable connection string
  let started = false;           // init() has run
  let on = false;                 // diagnostics are being sent
  let queue = [];
  let timer = 0;
  const early = [];               // things that happened before init(): kept only until it knows whether to send

  const blockedBy = () => {
    try { if (navigator.globalPrivacyControl === true) return 'Global Privacy Control'; } catch { /* unreadable */ }
    try { if (navigator.doNotTrack === '1' || window.doNotTrack === '1') return 'Do Not Track'; } catch { /* unreadable */ }
    return '';
  };
  function preference() {
    try { return localStorage.getItem(PREF_KEY) !== '0'; } catch { return true; }
  }
  function setPreference(want) {
    try { localStorage.setItem(PREF_KEY, want ? '1' : '0'); } catch { /* kept for this page only */ }
    mem.pref = !!want;
    refresh();
  }
  const mem = { pref: preference() };
  const available = () => !!conn;
  const effective = () => available() && !blockedBy() && mem.pref;

  function refresh() {
    on = started && effective();
    if (!on) { queue = []; clearTimeout(timer); timer = 0; }
  }

  function add(type, base) {
    if (!started) { if (early.length < QUEUE_MAX) early.push([type, base, new Date()]); return; }
    if (!on) return;
    queue.push(envelope(conn, sessionId, pathOf(location), type, base));
    if (queue.length >= BATCH_MAX) flush();
    else if (!timer) timer = setTimeout(flush, FLUSH_MS);
  }

  function take() {
    const list = [];
    let size = 2;
    while (queue.length) {
      const n = JSON.stringify(queue[0]).length + 1;
      if (list.length && size + n > BODY_MAX) break;
      size += n;
      list.push(queue.shift());
    }
    return list;
  }

  // Posts what is queued. The text/plain type keeps it a simple request (no preflight); Application Insights
  // reads the body as JSON whatever the type. A failed post is dropped: diagnostics are never worth a retry loop.
  function flush(final) {
    clearTimeout(timer);
    timer = 0;
    while (on && queue.length) {
      const body = JSON.stringify(take());
      let sent = false;
      if (final && navigator.sendBeacon) {
        try { sent = navigator.sendBeacon(conn.url, new Blob([body], { type: 'text/plain;charset=UTF-8' })); } catch { /* try fetch */ }
      }
      if (!sent) {
        try {
          fetch(conn.url, { method: 'POST', body, keepalive: true, credentials: 'omit', mode: 'cors', referrerPolicy: 'no-referrer',
            headers: { 'Content-Type': 'text/plain;charset=UTF-8' } }).catch(() => {});
        } catch { /* dropped */ }
      }
    }
  }

  // ---- what gets sent ----

  const t0 = performance.now();
  const api = {
    // Starts (or declines to start) once site.json is known. Sends the page view, then whatever happened earlier.
    init(cfg) {
      if (started) return;
      conn = parseConnectionString(cfg && cfg.connectionString);
      started = true;
      refresh();
      const held = early.splice(0);
      if (!on) return;
      for (const [type, base, when] of held) queue.push(envelope(conn, sessionId, pathOf(location), type, base, when));
      api.pageView();
      if (queue.length >= BATCH_MAX) flush();
      else if (queue.length && !timer) timer = setTimeout(flush, FLUSH_MS);
    },
    pageView() {
      const nav = performance.getEntriesByType && performance.getEntriesByType('navigation')[0];
      const done = (nav && nav.loadEventEnd > 0) ? nav.loadEventEnd : null;
      const send = (ms) => add('PageviewData', pageviewBase(pathOf(location), ms));
      if (done !== null) return send(done);
      // The page is still loading: send once it has, so the duration is the real load time.
      const after = () => setTimeout(() => {
        const n = performance.getEntriesByType('navigation')[0];
        send(n && n.loadEventEnd > 0 ? n.loadEventEnd : performance.now() - t0);
      }, 0);
      if (document.readyState === 'complete') after(); else addEventListener('load', after, { once: true });
    },
    event(name, props, measures) {
      const p = {};
      for (const k of Object.keys(props || {})) p[label(k)] = label(props[k]);
      const m = {};
      for (const k of Object.keys(measures || {})) if (Number.isFinite(measures[k])) m[label(k)] = Math.round(measures[k]);
      add('EventData', eventBase(name, p, m));
    },
    error(error) { add('ExceptionData', exceptionBase(error)); },
    // The online-session result: ok (the line opened), failed (it did not), closed (an open line ended).
    // Only the result and a coarse duration; never the address.
    connect(result, ms) { api.event('connect', { result, duration: bucket(ms) }); },
    capabilities(info) { api.event('capabilities', { webgl2: !!info.webgl2, jspi: !!info.jspi }); },
    available, preference, setPreference, blockedBy,
    enabled: () => on,
    flush: () => flush(true),
    sessionId: () => sessionId,
    pure,
  };

  addEventListener('error', (e) => {
    // Resource load errors (a missing script or image) bubble nowhere and carry no message; skip them.
    if (e.target && e.target !== window) return;
    api.error(e.error || e.message);
  });
  addEventListener('unhandledrejection', (e) => api.error(e.reason));
  // pagehide is the last reliable moment on every browser; hidden covers a tab that is switched away from and killed.
  addEventListener('pagehide', () => flush(true));
  document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'hidden') flush(true); });

  return api;
})();
