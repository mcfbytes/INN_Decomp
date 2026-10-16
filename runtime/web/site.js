// Operator settings (site.json) and the age and terms notice. Loaded before page.js. The legal pages
// (legal/*.html) read the same site.json through legal/legal.js. Nothing here is verification: the
// notice is a self-declaration, and docs/legal.md says so. Not legal advice.
'use strict';

const Site = (() => {
  const CONSENT_KEY = 'inn.consent', REFUSED_KEY = 'inn.refused';
  const DEFAULTS = {
    siteName: '', operatorName: '', contactEmail: '', privacyEmail: '',
    dmcaAgent: { name: '', address: '', email: '', phone: '' },
    governingLaw: '', serverLocation: '', effectiveDate: '', termsVersion: 1, minimumAge: 18,
    auditRetentionDays: 90, authServer: '',
  };
  const LINKS = [
    { label: 'Terms of Use', short: 'Terms', href: 'legal/terms.html' },
    { label: 'Privacy Notice', short: 'Privacy', href: 'legal/privacy.html' },
    { label: 'Copyright & takedown', short: 'Copyright', href: 'legal/copyright.html' },
  ];
  const cfg = JSON.parse(JSON.stringify(DEFAULTS));
  const $ = (id) => document.getElementById(id);
  const mk = (tag, props = {}, ...kids) => {
    const n = Object.assign(document.createElement(tag), props);
    n.append(...kids);
    return n;
  };

  // ---- site.json ----

  const str = (v) => (typeof v === 'string' ? v.trim() : '');
  const posInt = (v, fallback) => (Number.isInteger(v) && v >= 1 ? v : fallback);

  function merge(d) {
    if (!d || typeof d !== 'object') return;
    for (const k of ['siteName', 'operatorName', 'contactEmail', 'privacyEmail', 'governingLaw', 'serverLocation', 'effectiveDate', 'authServer'])
      cfg[k] = str(d[k]);
    if (d.dmcaAgent && typeof d.dmcaAgent === 'object')
      for (const k of Object.keys(cfg.dmcaAgent)) cfg.dmcaAgent[k] = str(d.dmcaAgent[k]);
    cfg.termsVersion = posInt(d.termsVersion, DEFAULTS.termsVersion);
    cfg.minimumAge = posInt(d.minimumAge, DEFAULTS.minimumAge);
    cfg.auditRetentionDays = posInt(d.auditRetentionDays, DEFAULTS.auditRetentionDays);
  }

  const ready = fetch('site.json', { cache: 'no-cache' })
    .then((r) => (r.ok ? r.json() : null)).catch(() => null)
    .then(merge);

  // ---- remembered answers (every access guarded; without storage the notice is asked each visit) ----

  let memoryOk = false, memoryRefused = false;
  const today = () => new Date().toISOString().slice(0, 10);

  function storedConsent() {
    try { return JSON.parse(localStorage.getItem(CONSENT_KEY) || 'null'); } catch { return null; }
  }
  const validConsent = (d) => !!d && Number.isInteger(d.age) && Number.isInteger(d.terms)
    && d.age >= cfg.minimumAge && d.terms >= cfg.termsVersion;
  const isRefused = () => {
    if (memoryRefused) return true;
    try { return sessionStorage.getItem(REFUSED_KEY) === '1'; } catch { return false; }
  };

  // ---- the dialog ----

  const st = { phase: 'pending', root: null, box: null, inerted: [], waiters: [], opener: null };
  const blocking = () => st.phase === 'open' || st.phase === 'refused';

  function holdPage(on) {
    if (on) {
      for (const n of document.body.children) {
        if (n === st.root || n.tagName === 'SCRIPT' || n.inert) continue;
        n.inert = true;
        st.inerted.push(n);
      }
    } else {
      for (const n of st.inerted) n.inert = false;
      st.inerted = [];
    }
    document.body.classList.toggle('consent-open', on);
  }

  const focusables = () => (st.box ? [...st.box.querySelectorAll('button, a[href]')] : []);

  function linkRow() {
    const row = mk('p', { className: 'links' });
    LINKS.forEach((l, i) => {
      if (i) row.append(' · ');
      row.append(mk('a', { href: l.href, target: '_blank', rel: 'noopener', textContent: l.label }));
    });
    return row;
  }

  function render(kind) {
    st.root.replaceChildren();
    const box = mk('div', { className: 'box', tabIndex: -1 });
    box.setAttribute('role', 'dialog');
    box.setAttribute('aria-modal', 'true');
    box.setAttribute('aria-labelledby', 'consent-title');
    box.setAttribute('aria-describedby', 'consent-desc');
    st.box = box;
    if (kind === 'refused') {
      box.append(
        mk('h2', { id: 'consent-title', textContent: 'Another time, perhaps' }),
        mk('p', { id: 'consent-desc', className: 'lead', textContent: `This place is for adults (${cfg.minimumAge} or older), so the door stays shut for now. Thank you for being straight with us.` }),
        mk('p', { className: 'aside', textContent: 'The doorman tips his hat, which is the polite thing to do when closing a door.' }),
        mk('p', { className: 'small', textContent: 'Pressed the wrong button? Close this tab and open the page again.' }));
    } else {
      const age = cfg.minimumAge;
      const yes = mk('button', { type: 'button', className: 'abtn', textContent: `I am ${age} or older` });
      const no = mk('button', { type: 'button', className: 'abtn', textContent: 'I am not' });
      yes.addEventListener('click', () => answer(true));
      no.addEventListener('click', () => answer(false));
      const agree = mk('p', { className: 'small' }, 'By continuing you agree to the ');
      agree.append(mk('a', { href: LINKS[0].href, target: '_blank', rel: 'noopener', textContent: 'Terms of Use' }), ' and the ',
        mk('a', { href: LINKS[1].href, target: '_blank', rel: 'noopener', textContent: 'Privacy Notice' }), '.');
      const more = mk('p', { className: 'small' }, 'Copyright and takedown information is in the ');
      more.append(mk('a', { href: LINKS[2].href, target: '_blank', rel: 'noopener', textContent: 'Copyright & takedown' }), ' policy.');
      box.append(
        mk('h2', { id: 'consent-title', textContent: 'Before you go in…' }),
        mk('p', { className: 'aside', textContent: 'A doorman looks up from his crossword and clears his throat.' }),
        mk('p', { id: 'consent-desc', className: 'lead', textContent: `This service is for adults (${age} or older).` }),
        agree, more,
        mk('div', { className: 'acts' }, yes, no),
        mk('p', { className: 'tiny', textContent: 'This is a statement, not an ID check. We do not collect your birthdate, and nothing on this page is pre-ticked.' }));
    }
    st.root.append(box);
    box.focus({ preventScroll: true });
  }

  function ensureRoot() {
    if (st.root) return;
    st.root = mk('div', { id: 'consent', className: 'consent' });
    document.body.append(st.root);
  }

  function open(kind) {
    ensureRoot();
    if (!blocking()) st.opener = document.activeElement;
    st.phase = kind === 'refused' ? 'refused' : 'open';
    holdPage(true);
    render(kind);
  }

  function release() {
    st.phase = 'ok';
    holdPage(false);
    if (st.root) st.root.replaceChildren();
    st.box = null;
    const back = st.opener;
    st.opener = null;
    if (back && back.focus && document.contains(back)) back.focus();
    const w = st.waiters.splice(0);
    for (const fn of w) fn(true);
  }

  function answer(adult) {
    if (!adult) {
      memoryRefused = true;
      try { sessionStorage.setItem(REFUSED_KEY, '1'); } catch { /* kept in memory for this page */ }
      open('refused');
      return;
    }
    memoryOk = true;
    try { localStorage.setItem(CONSENT_KEY, JSON.stringify({ age: cfg.minimumAge, terms: cfg.termsVersion, at: today() })); }
    catch { /* no storage: it is asked again next visit */ }
    release();
  }

  // Resolves true once the notice for the current terms version has been answered yes. Asks if it has not
  // been. Never resolves while the refusal screen is up (the page stays closed).
  function ensure() {
    return ready.then(() => new Promise((resolve) => {
      if (st.phase === 'refused') return;
      if (st.phase === 'open') { st.waiters.push(resolve); return; }
      if (memoryOk || validConsent(storedConsent())) { memoryOk = true; st.phase = 'ok'; resolve(true); return; }
      st.waiters.push(resolve);
      open(isRefused() ? 'refused' : 'ask');
    }));
  }

  // ---- keeping everything else quiet while the notice is up ----

  const swallow = (e) => { if (blocking()) e.stopImmediatePropagation(); };

  addEventListener('keydown', (e) => {
    if (!blocking()) return;
    e.stopImmediatePropagation();
    if (e.key === 'Tab') {
      e.preventDefault();
      const list = focusables();
      if (!list.length) { st.box.focus(); return; }
      const i = list.indexOf(document.activeElement);
      const next = i < 0 ? (e.shiftKey ? list.length - 1 : 0) : (i + (e.shiftKey ? list.length - 1 : 1)) % list.length;
      list[next].focus();
    } else if (e.key === 'Escape' || e.key === 'F10') {
      e.preventDefault();
    }
  }, true);
  addEventListener('keyup', swallow, true);
  addEventListener('keypress', swallow, true);
  document.addEventListener('focusin', (e) => {
    if (blocking() && st.box && !st.root.contains(e.target)) st.box.focus();
  }, true);

  // The login URL's attestation: "<age>+v<terms>", once the notice has been answered.
  const attest = () => (st.phase === 'ok' ? `${cfg.minimumAge}+v${cfg.termsVersion}` : '');

  ensure();

  return {
    cfg, ready, LINKS, ensure, attest, blocking,
    whenClear: () => ensure(),
    contact: () => cfg.contactEmail,
  };
})();
