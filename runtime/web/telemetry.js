// Anonymous diagnostics for the operator's Azure Application Insights, with no third-party script: this file
// builds the v2 envelopes and posts them to <IngestionEndpoint>/v2/track itself. It is on only when site.json
// has telemetry.connectionString, and off when the browser says Global Privacy Control or Do Not Track, or the
// visitor turned the Settings switch off. It sends the things listed in docs/web.md ("Diagnostics") and nothing
// else: no cookie, no persistent ID (the session and trace IDs are random per page load and live in memory), no
// query string or hash, no server address, account number, handle, chat or file name. One page load is one trace:
// the page (role web), the runtime (role dosrt) and, through a traceparent on the line and on /auth calls, the
// server's handling of them (docs/web.md, "Diagnostics", "The trace"). The scrubbing and the envelope building are
// plain functions so tools/test_telemetry.js can run them under Node. Loaded before everything else.
'use strict';

const Telemetry = (() => {
  const PREF_KEY = 'inn.diag';           // '0' = the visitor turned diagnostics off; anything else = on
  const SDK = 'web-diag:2';
  const FLUSH_MS = 5000, BATCH_MAX = 20, QUEUE_MAX = 60, BODY_MAX = 56000;   // fetch keepalive bodies may not pass 64 KB
  const STACK_MAX = 2048, MESSAGE_MAX = 1024, NAME_MAX = 64, PROP_MAX = 64;
  const GUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
  const SERVER = 'innkeeperd';           // the dependency target and the server's role on the map; never its address

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
  // from the User-Agent header itself. `at` places it in the trace: { sessionId, path, operationId, role, parentId }.
  function envelope(conn, at, type, base, now = new Date()) {
    const names = { EventData: 'Event', PageviewData: 'Pageview', ExceptionData: 'Exception', RequestData: 'Request', RemoteDependencyData: 'RemoteDependency' };
    const tags = { 'ai.session.id': at.sessionId, 'ai.operation.name': at.path, 'ai.operation.id': at.operationId, 'ai.cloud.role': at.role, 'ai.internal.sdkVersion': SDK };
    if (at.parentId) tags['ai.operation.parentId'] = at.parentId;
    return {
      name: `Microsoft.ApplicationInsights.${conn.iKey.replace(/-/g, '')}.${names[type]}`,
      time: now.toISOString(),
      iKey: conn.iKey,
      tags,
      data: { baseType: type, baseData: Object.assign({ ver: 2 }, base) },
    };
  }

  const pageviewBase = (path, ms, id) => ({ id, name: path, url: path, duration: duration(ms) });   // the path is the whole address we send
  const requestBase = (id, name, ms, code, success, props, measures) =>
    ({ id, name: label(name), duration: duration(ms), responseCode: label(code), success: !!success, properties: props || {}, measurements: measures || {} });
  const dependencyBase = (id, name, type, target, ms, code, success) =>
    ({ id, name, type, target, duration: duration(ms), resultCode: label(code), success: !!success, properties: {} });

  // `00-<trace>-<parent>-01`: what the line's address and the /auth calls carry so the server can join the trace.
  const traceparent = (operationId, parentId) => `00-${operationId}-${parentId}-01`;

  // An /auth path as a route: no query or hash, and numbers (account numbers) or long values become placeholders.
  function routeOf(path) {
    const bare = String(path == null ? '' : path).replace(/[?#].*$/, '');
    return bare.split('/').map((seg) => (/^\d+$/.test(seg) ? '{id}' : seg.length > 32 || /[^\w.-]/.test(seg) ? '{value}' : seg)).join('/') || '/';
  }

  // Errors from inside the runtime (its wasm frames or its loader, inn.js or packaged inn.<hash>.js) belong to dosrt.
  const roleOfError = (error) => (/wasm:|\binn(\.[0-9a-f]+)?\.js\b/.test(error && typeof error.stack === 'string' ? error.stack : '') ? 'dosrt' : 'web');
  const eventBase = (name, props, measures) => ({ name: label(name).slice(0, NAME_MAX), properties: props || {}, measurements: measures || {} });
  function exceptionBase(error, extra) {
    const isErr = error && typeof error === 'object';
    const type = label(isErr && error.name ? error.name : 'Error');
    const msg = scrubMessage(isErr && 'message' in error ? error.message : error);
    const stack = isErr && typeof error.stack === 'string' ? scrubStack(error.stack) : '';
    return { exceptions: [{ typeName: type, message: msg || type, hasFullStack: !!stack, stack }], severityLevel: 3, properties: extra || {} };
  }

  const randomHex = (bytes) => {
    const b = new Uint8Array(bytes);
    (typeof crypto !== 'undefined' && crypto.getRandomValues ? crypto : { getRandomValues: (a) => a.map(() => Math.floor(Math.random() * 256)) }).getRandomValues(b);
    return Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
  };
  const spanId = () => randomHex(8);

  const pure = { parseConnectionString, scrubText, scrubMessage, scrubStack, label, pathOf, duration, bucket, envelope, pageviewBase, eventBase, exceptionBase,
    requestBase, dependencyBase, traceparent, routeOf, roleOfError };

  // ---- the live module (browser only) ----

  if (typeof window === 'undefined') {
    if (typeof module !== 'undefined' && module.exports) module.exports = pure;
    return pure;
  }

  const sessionId = spanId();     // per page load, in memory only
  // The trace's fixed spans: page view, the page starting the runtime, the runtime's start, and its line.
  const trace = { operationId: randomHex(16), page: spanId(), start: spanId(), runtime: spanId(), line: spanId() };
  const WEB = { role: 'web', parentId: trace.page };
  const DOSRT = { role: 'dosrt', parentId: trace.runtime };
  const line = { armed: false, opened: false, sent: false, t0: 0 };
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

  const at = (scope) => ({ sessionId, path: pathOf(location), operationId: trace.operationId, role: scope.role, parentId: scope.parentId });

  function add(type, base, scope = WEB) {
    if (!started) { if (early.length < QUEUE_MAX) early.push([type, base, scope, new Date()]); return; }
    if (!on) return;
    queue.push(envelope(conn, at(scope), type, base));
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
      for (const [type, base, scope, when] of held) queue.push(envelope(conn, at(scope), type, base, when));
      api.pageView();
      if (queue.length >= BATCH_MAX) flush();
      else if (queue.length && !timer) timer = setTimeout(flush, FLUSH_MS);
    },
    pageView() {
      const nav = performance.getEntriesByType && performance.getEntriesByType('navigation')[0];
      const done = (nav && nav.loadEventEnd > 0) ? nav.loadEventEnd : null;
      const send = (ms) => add('PageviewData', pageviewBase(pathOf(location), ms, trace.page), { role: 'web', parentId: '' });
      if (done !== null) return send(done);
      // The page is still loading: send once it has, so the duration is the real load time.
      const after = () => setTimeout(() => {
        const n = performance.getEntriesByType('navigation')[0];
        send(n && n.loadEventEnd > 0 ? n.loadEventEnd : performance.now() - t0);
      }, 0);
      if (document.readyState === 'complete') after(); else addEventListener('load', after, { once: true });
    },
    event(name, props, measures, scope = WEB) {
      const p = {};
      for (const k of Object.keys(props || {})) p[label(k)] = label(props[k]);
      const m = {};
      for (const k of Object.keys(measures || {})) if (Number.isFinite(measures[k])) m[label(k)] = Math.round(measures[k]);
      add('EventData', eventBase(name, p, m), scope);
    },
    error(error) { add('ExceptionData', exceptionBase(error), roleOfError(error) === 'dosrt' ? DOSRT : WEB); },
    // The runtime came up: the page's call to it and the runtime's own start, then the event the dashboards know.
    // kind is dosbox or native.
    runtimeStarted(downloadMs, startMs, kind) {
      add('RemoteDependencyData', dependencyBase(trace.start, 'start dosrt', 'Runtime', 'dosrt', downloadMs + startMs, 'ok', true));
      add('RequestData', requestBase(trace.runtime, 'start', startMs, 'ok', true, { runtime: label(kind) }, { downloadMs: Math.round(downloadMs) }),
        { role: 'dosrt', parentId: trace.start });
      api.event('runtime_started', {}, { runtimeDownloadMs: downloadMs, runtimeStartMs: startMs }, DOSRT);
    },
    // What the line's address carries so the server joins this trace; empty, and nothing reaches the server, when off.
    lineTraceparent() {
      if (!on) return '';
      Object.assign(line, { armed: true, opened: false, sent: false, t0: performance.now() });
      return traceparent(trace.operationId, trace.line);
    },
    // The online-session result: ok (the line opened), failed (it did not), closed (an open line ended).
    // Only the result and a coarse duration; never the address. The line's own span goes out once it is over.
    connect(result, ms) {
      api.event('connect', { result, duration: bucket(ms) }, {}, DOSRT);
      if (result === 'ok') line.opened = true;
      else endLine(result);
    },
    // An /auth call: the header to add (or null when off) and done(status) once it answered (0: no answer).
    traceFetch(method, path) {
      if (!on) return null;
      const id = spanId(), t = performance.now();
      return {
        header: traceparent(trace.operationId, id),
        done: (status) => add('RemoteDependencyData', dependencyBase(id, `${label(method)} ${routeOf(path)}`, 'Fetch', SERVER,
          performance.now() - t, String(status || 0), status > 0 && status < 500)),
      };
    },
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

  function endLine(result) {
    if (!line.armed || line.sent) return;
    line.sent = true;
    add('RemoteDependencyData', dependencyBase(trace.line, 'WS line', 'WebSocket', SERVER, performance.now() - line.t0, result, result !== 'failed'), DOSRT);
  }
  // pagehide is the last reliable moment on every browser; hidden covers a tab that is switched away from and killed.
  addEventListener('pagehide', () => { endLine(line.opened ? 'open' : 'dialing'); flush(true); });
  document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'hidden') flush(true); });

  return api;
})();
