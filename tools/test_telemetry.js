#!/usr/bin/env node
// Unit checks for runtime/web/telemetry.js: the connection string, the scrubbing of messages and stacks, and the
// shape of the envelopes. Nothing here touches a network or a browser.
// Run: node tools/test_telemetry.js
'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const T = require(path.join(__dirname, '..', 'runtime', 'web', 'telemetry.js'));

const KEY = '00000000-1111-2222-3333-444444444444';

// ---- the connection string ----
assert.deepEqual(T.parseConnectionString(`InstrumentationKey=${KEY};IngestionEndpoint=https://eastus-8.in.applicationinsights.azure.com/;LiveEndpoint=https://eastus.livediagnostics.monitor.azure.com/;ApplicationId=abc`),
  { iKey: KEY, url: 'https://eastus-8.in.applicationinsights.azure.com/v2/track' });
assert.equal(T.parseConnectionString(`InstrumentationKey=${KEY};IngestionEndpoint=http://127.0.0.1:2860/`).url, 'http://127.0.0.1:2860/v2/track', 'loopback http is for the local test page');
for (const bad of ['', null, undefined, 42, 'InstrumentationKey=nope;IngestionEndpoint=https://x.test/', `InstrumentationKey=${KEY}`,
  `InstrumentationKey=${KEY};IngestionEndpoint=http://example.test/`, `InstrumentationKey=${KEY};IngestionEndpoint=javascript:alert(1)`])
  assert.equal(T.parseConnectionString(bad), null, String(bad));

// ---- scrubbing ----
const stack = `TypeError: x is not a function
    at Object.dial (https://play.example.org/assets/app.0123456789.js:1:23456)
    at https://play.example.org/assets/app.0123456789.js?token=abc#frag:1:99
    at wasm://wasm/0a1b2c3d:wasm-function[12]:0x1a2b
    at ws://127.0.0.1:2316/line (<anonymous>)`;
const s = T.scrubStack(stack);
assert.match(s, /at Object\.dial \(app\.0123456789\.js:1:23456\)/, 'URLs shrink to the file name, line and column stay');
assert.match(s, /at app\.0123456789\.js:1:99/, 'query string and hash are gone');
assert.match(s, /wasm:0a1b2c3d:wasm-function\[12\]:0x1a2b/);
assert.ok(!/play\.example|127\.0\.0\.1|2316|token|frag|https?:|wss?:/.test(s), s);
assert.match(s, /\[address\]/);

const m = T.scrubMessage("WebSocket connection to 'wss://game.example.org:2316/x?k=1' failed; account 123456789 on 10.1.2.3, 192.168.0.9:2316, host.test:443; mail a.b@c.org; read /c/INN/PLAYER/FOO.DTA and C:\\GAME\\X.CFG");
assert.ok(!/game\.example|2316|123456789|10\.1\.2\.3|host\.test|a\.b@|FOO\.DTA|GAME|k=1/.test(m), m);
assert.match(m, /\[number\]/);
assert.match(m, /\/c\/\[path\]/);
assert.equal(T.scrubMessage('plain text 12 and 1234 stay'), 'plain text 12 and 1234 stay');
assert.equal(T.scrubMessage(undefined), '');
assert.equal(T.scrubStack('a'.repeat(5000)).length, 2048, 'a stack is cut to 2 KB');
assert.equal(T.scrubMessage('b'.repeat(5000)).length, 1024);
assert.ok(T.scrubText('https://x.test/').includes('[address]'), 'a bare origin has no file name to keep');

// ---- the pieces ----
assert.equal(T.pathOf({ pathname: '/legal/privacy.html', search: '?com1=ws://h:1', hash: '#inn-session=SECRET' }), '/legal/privacy.html');
assert.equal(T.pathOf({}), '/');
assert.equal(T.duration(1234), '00:00:01.234');
assert.equal(T.duration(3723004), '01:02:03.004');
assert.equal(T.duration(NaN), '00:00:00.000');
assert.deepEqual([1000, 3000, 10000, 30000, 120000, 600000, 4e6].map(T.bucket), ['under-2s', '2-5s', '5-15s', '15-60s', '1-5min', '5-30min', 'over-30min']);
assert.equal(T.label('a b/c?d=e'), 'a_b_c_d_e');

// ---- envelopes ----
const conn = { iKey: KEY, url: 'https://x.test/v2/track' };
const when = new Date('2026-01-02T03:04:05.678Z');
const OP = '0123456789abcdef0123456789abcdef';
const page = { sessionId: 'abcd', path: '/', operationId: OP, role: 'web', parentId: '' };
const pv = T.envelope(conn, page, 'PageviewData', T.pageviewBase('/', 1234, '1111222233334444'), when);
assert.deepEqual(pv, {
  name: 'Microsoft.ApplicationInsights.00000000111122223333444444444444.Pageview',
  time: '2026-01-02T03:04:05.678Z', iKey: KEY,
  tags: { 'ai.session.id': 'abcd', 'ai.operation.name': '/', 'ai.operation.id': OP, 'ai.cloud.role': 'web', 'ai.internal.sdkVersion': 'web-diag:2' },
  data: { baseType: 'PageviewData', baseData: { ver: 2, id: '1111222233334444', name: '/', url: '/', duration: '00:00:01.234' } },
});
const child = { sessionId: 'abcd', path: '/', operationId: OP, role: 'dosrt', parentId: '1111222233334444' };
const ev = T.envelope(conn, child, 'EventData', T.eventBase('connect', { result: 'ok', duration: '2-5s' }, { n: 1 }), when);
assert.equal(ev.name.split('.').pop(), 'Event');
assert.equal(ev.tags['ai.operation.parentId'], '1111222233334444');
assert.equal(ev.tags['ai.cloud.role'], 'dosrt');
assert.deepEqual(ev.data.baseData, { ver: 2, name: 'connect', properties: { result: 'ok', duration: '2-5s' }, measurements: { n: 1 } });
const ex = T.envelope(conn, page, 'ExceptionData', T.exceptionBase(Object.assign(new Error('boom at wss://h.test:1/x'), { stack: stack })), when);
const d = ex.data.baseData.exceptions[0];
assert.equal(ex.name.split('.').pop(), 'Exception');
assert.equal(d.typeName, 'Error');
assert.equal(d.message, 'boom at [address]');
assert.ok(d.hasFullStack && d.stack.length <= 2048 && !/play\.example/.test(d.stack));
assert.equal(T.exceptionBase('just a string').exceptions[0].message, 'just a string');

// ---- the trace ----
const rq = T.envelope(conn, child, 'RequestData', T.requestBase('5555666677778888', 'start', 2500, 'ok', true, { runtime: 'dosbox' }, { downloadMs: 900 }), when);
assert.equal(rq.name.split('.').pop(), 'Request');
assert.deepEqual(rq.data.baseData, { ver: 2, id: '5555666677778888', name: 'start', duration: '00:00:02.500', responseCode: 'ok', success: true,
  properties: { runtime: 'dosbox' }, measurements: { downloadMs: 900 } });
const dep = T.envelope(conn, child, 'RemoteDependencyData', T.dependencyBase('9999aaaabbbbcccc', 'WS line', 'WebSocket', 'innkeeperd', 60000, 'failed', false), when);
assert.equal(dep.name.split('.').pop(), 'RemoteDependency');
assert.deepEqual(dep.data.baseData, { ver: 2, id: '9999aaaabbbbcccc', name: 'WS line', type: 'WebSocket', target: 'innkeeperd', duration: '00:01:00.000',
  resultCode: 'failed', success: false, properties: {} });
assert.equal(T.traceparent(OP, '9999aaaabbbbcccc'), `00-${OP}-9999aaaabbbbcccc-01`);
assert.match(T.traceparent(OP, '9999aaaabbbbcccc'), /^00-[0-9a-f]{32}-[0-9a-f]{16}-01$/);
assert.equal(T.routeOf('/auth/accounts/123456/credential'), '/auth/accounts/{id}/credential', 'an account number never goes out');
assert.equal(T.routeOf('/auth/accounts/7/saves?path=PLAYER%2FFOO.DTA'), '/auth/accounts/{id}/saves', 'nor a query (a save file name)');
assert.equal(T.routeOf(`/auth/x/${'z'.repeat(40)}`), '/auth/x/{value}');
assert.equal(T.routeOf('/auth/identities/unlink'), '/auth/identities/unlink');
assert.equal(T.roleOfError({ stack: 'RuntimeError: unreachable\n    at wasm://wasm/0a1b:wasm-function[3]:0x10' }), 'dosrt');
assert.equal(T.roleOfError({ stack: 'Error: x\n    at f (https://h.test/inn.js:1:2)' }), 'dosrt');
assert.equal(T.roleOfError({ stack: 'Error: x\n    at f (https://h.test/page.js:1:2)' }), 'web');
assert.equal(T.roleOfError({ stack: 'Error: x\n    at f (https://h.test/assets/inn.0123abcd.js:1:2)' }), 'dosrt', 'the packaged loader');
assert.equal(T.roleOfError({ stack: 'Error: x\n    at f (https://h.test/assets/app.0123abcd.js:1:2)' }), 'web');
assert.equal(T.roleOfError('a string'), 'web');
// No user, device or location tag ever goes out; the trace tags are the only additions.
const allowed = ['ai.cloud.role', 'ai.internal.sdkVersion', 'ai.operation.id', 'ai.operation.name', 'ai.operation.parentId', 'ai.session.id'];
for (const e of [pv, ev, ex, rq, dep]) for (const k of Object.keys(e.tags)) assert.ok(allowed.includes(k), k);
assert.ok(!('ai.operation.parentId' in pv.tags), 'the page view is the root');

console.log('telemetry: ok');
