// The landing as a small point-and-click adventure: no setup card over the picture. The installation
// is a disk box in the scene, the dial-up is a telephone in the scene, and everything else lives in a
// narrator box at the bottom and a drop-down menu bar at the top (F10 opens it; it does everything
// the picture does, so keyboard and screen-reader users never need the scene). page.js owns the real
// work (the C: drive, the runtime, the modem log) and calls the hooks below; `?classic=1` turns all of
// this off and shows the old setup card. Original text throughout. Query parameters: docs/web.md.
'use strict';

const Adv = (() => {
  const CLASSIC = new URLSearchParams(location.search).has('classic');
  const SCORE_KEY = 'inn.score', ANIM_KEY = 'inn.anim';
  const SCORE_MAX = 1993;
  const POINTS = { disks: 10, dial: 5, connect: 25, gag: 1 };
  const HINT_IDLE_MS = 10000, HINT_MAX = 2, TYPE_MS = 22, SHOW_MS = 7000, BAR_HIDE_MS = 1400;
  const IRIS_CLOSE_MS = 480, IRIS_OPEN_MS = 320;

  const GENERIC = [
    'You can\'t dial a cloud. Well, you can, but nobody answers.',
    'Nothing happens. It is a very tidy kind of nothing.',
    'You poke the scenery. The scenery files a polite complaint.',
    'That is not a thing you can use. It is barely a thing you can look at.',
    'You look closely. It looks back, but only out of manners.',
    'Interesting. Not useful, but interesting.',
    'The narrator clears its throat and says nothing in particular.',
    'You click there with great confidence. The confidence is not returned.',
  ];
  const REFUSAL = 'You pick up the receiver. A voice says: "Disks first, please."';
  const NUDGE = {
    disks: 'Something nearby is glinting. Possibly a clue, possibly a sequin.',
    phone: 'The telephone is twinkling at you. It has been practising.',
  };

  const $ = (id) => document.getElementById(id);
  const el = (tag, props = {}, ...kids) => {
    const n = Object.assign(document.createElement(tag), props);
    n.append(...kids);
    return n;
  };
  const store = {
    get(key, area = sessionStorage) { try { return area.getItem(key); } catch { return null; } },
    set(key, value, area = sessionStorage) { try { area.setItem(key, value); } catch { /* kept in memory only */ } },
  };
  const landing = () => document.body.dataset.view === 'setup' || document.body.dataset.view === 'dialing';
  const reduced = () => InnScenes.reduced();

  const adv = { ready: false, lastAct: Date.now(), hints: 0, welcomed: false, anim: true };
  let ui = null;

  // ---------- score: kept per session, each award once ----------

  const score = (() => {
    let data = { pts: 0, once: [] };
    try { const d = JSON.parse(store.get(SCORE_KEY) || ''); if (d && Array.isArray(d.once)) data = { pts: +d.pts || 0, once: d.once }; } catch { /* new game */ }
    return data;
  })();

  function showScore(gain) {
    ui.scoreText.textContent = `${score.pts} of ${SCORE_MAX}`;
    if (!gain) return;
    barShow();
    setTimeout(barLater, 1200);
    if (reduced()) return;
    const plus = el('span', { className: 'plus', textContent: `+${gain}` });
    ui.score.append(plus);
    setTimeout(() => plus.remove(), 1500);
  }

  function award(key, pts) {
    if (score.once.includes(key)) return;
    score.once.push(key);
    score.pts += pts;
    store.set(SCORE_KEY, JSON.stringify(score));
    showScore(pts);
  }

  // ---------- the narrator box ----------

  const narr = { timer: 0, type: 0, text: '' };

  function narrHide() {
    clearTimeout(narr.timer); clearInterval(narr.type);
    ui.narr.hidden = true;
  }

  // say('text', { actions: [{ label, fn }], sticky, bad }) shows a line in the bottom box. A box with
  // buttons, an error or help stays until dismissed; a plain remark fades after a few seconds.
  function say(text, o = {}) {
    clearTimeout(narr.timer); clearInterval(narr.type);
    ui.term.hidden = ui.term.dataset.open !== '1';
    narr.text = text;
    ui.narr.hidden = false;
    // Keep the box off the object being talked about: it sits on the other side of the picture.
    ui.narr.dataset.side = o.near === undefined ? 'mid' : o.near < 160 ? 'right' : 'left';
    ui.narr.classList.toggle('bad', !!o.bad);
    ui.narrSr.textContent = text;
    ui.acts.replaceChildren(...(o.actions || []).map((a) => {
      const b = el('button', { type: 'button', className: 'abtn', textContent: a.label });
      b.addEventListener('click', (e) => { e.stopPropagation(); a.fn(); });
      return b;
    }));
    const sticky = o.sticky || o.bad || (o.actions && o.actions.length);
    if (reduced()) {
      ui.narrText.textContent = text;
    } else {
      ui.narrText.textContent = '';
      let i = 0;
      narr.type = setInterval(() => {
        ui.narrText.textContent = text.slice(0, ++i);
        if (i >= text.length) clearInterval(narr.type);
      }, TYPE_MS);
    }
    if (!sticky) narr.timer = setTimeout(narrHide, SHOW_MS + (reduced() ? 0 : text.length * TYPE_MS));
    if (ui.term.dataset.open === '1') ui.narr.hidden = true;
    adv.lastAct = Date.now();
  }

  // ---------- the scene's own objects ----------

  const objects = () => (InnScenes.scene() && InnScenes.scene().objects) || {};
  const sceneTitle = () => (InnScenes.scene() && InnScenes.scene().title) || 'the scene';

  function haveDisks() { return page.drive !== 'none'; }

  // The scene column of an object's sparkle box (the middle of the picture when the scene has none).
  function sparkX(role) {
    const s = InnScenes.scene() && InnScenes.scene().spark && InnScenes.scene().spark[role];
    return s ? s[0] + s[2] / 2 : undefined;
  }

  function foundGag(h) {
    award(`gag:${InnScenes.id()}/${h.id}`, POINTS.gag);
  }

  function chooseFolder() {
    if (!adv.ready) return say('The runtime is still unpacking. Give it a moment, then try again.');
    if (page.started) return say('The disks are in use right now. Hang up first if you want to swap them.');
    $('adv-dir').value = '';
    $('adv-dir').click();
  }

  function chooseZip() {
    if (!adv.ready) return say('The runtime is still unpacking. Give it a moment, then try again.');
    if (page.started) return say('The disks are in use right now. Hang up first if you want to swap them.');
    $('adv-zip').value = '';
    $('adv-zip').click();
  }

  const diskButtons = () => [{ label: 'Folder', fn: chooseFolder }, { label: '.zip', fn: chooseZip }];

  function disksClicked() {
    const o = objects().disks || {};
    if (!adv.ready) return say('The box is still being unpacked. One moment.', { near: sparkX('disks') });
    if (page.started) return say('The disks are busy at the moment. They are on the telephone.', { near: sparkX('disks') });
    if (haveDisks()) {
      say(`You already have the disks. ${o.again || 'Swapping them is allowed, if you feel fickle.'}`, { actions: diskButtons(), near: sparkX('disks') });
      return;
    }
    say(`${o.open || 'You open the box.'} Choose the folder that contains TSNEXEC.EXE. Or perhaps a disk image (.zip)?`, { actions: diskButtons(), near: sparkX('disks') });
    chooseFolder();
  }

  function phoneClicked() {
    const o = objects().phone || {};
    if (page.started) return say(o.busy || 'The line is already in use. By you.', { near: sparkX('phone') });
    if (!adv.ready) return say('The telephone is still warming up. Give it a moment.');
    if (!haveDisks()) return say(o.refuse || REFUSAL, { near: sparkX('phone') });
    dial();
  }

  function dial() {
    const o = objects().phone || {};
    if (page.started) return;
    if (!adv.ready) return say('The telephone is still warming up. Give it a moment.');
    if (!haveDisks()) return say(o.refuse || REFUSAL, { near: sparkX('phone') });
    const bad = serverProblem($('com1').value.trim() || defaultServer());
    if (bad) return say(bad, { bad: true, actions: [{ label: 'Server address', fn: openServerDialog }] });
    ui.who.textContent = o.answer ? `"${o.answer}"` : '';
    award('dial', POINTS.dial);
    start();
  }

  function hangUp() {
    if (!page.started) return say('You are not on the line. Hanging up on nobody is rude, but nobody minds.');
    store.set('inn.redial', '', sessionStorage);
    location.reload();
  }

  function redial() {
    store.set('inn.redial', '1');
    location.reload();
  }

  function click(h) {
    adv.lastAct = Date.now();
    if (h && h.role === 'disks') return disksClicked();
    if (h && h.role === 'phone') return phoneClicked();
    if (h) { foundGag(h); return say(InnScenes.nextQuip(h), { near: h.x + h.w / 2 }); }
    say(GENERIC[Math.floor(Math.random() * GENERIC.length)]);
  }

  function hover(h) {
    if (h.role) return;
    foundGag(h);
    say(InnScenes.nextQuip(h), { near: h.x + h.w / 2 });
  }

  // ---------- the server address ----------

  function serverProblem(value) {
    let url;
    try { url = new URL(value); } catch { url = null; }
    if (!url || (url.protocol !== 'ws:' && url.protocol !== 'wss:') || !url.hostname)
      return `The server address "${value}" is not a WebSocket address. Use ws://host:port or wss://host:port, under Options, Server address.`;
    return '';
  }

  let dlgReturn = null;

  function openServerDialog() {
    if (page.started) return say(`The call is already placed (${page.server}). The address can be changed after you hang up.`);
    dlgReturn = document.activeElement;
    ui.srv.value = $('com1').value;
    ui.srv.placeholder = defaultServer();
    ui.srvErr.textContent = '';
    ui.dlg.hidden = false;
    ui.srv.focus();
    ui.srv.select();
  }

  function closeServerDialog() {
    ui.dlg.hidden = true;
    if (dlgReturn && dlgReturn.focus && document.contains(dlgReturn)) dlgReturn.focus();
  }

  function saveServerDialog() {
    const v = ui.srv.value.trim();
    const bad = v ? serverProblem(v) : '';
    if (bad) { ui.srvErr.textContent = bad; ui.srv.focus(); return; }
    $('com1').value = v || defaultServer();
    $('com1').dispatchEvent(new Event('change'));
    closeServerDialog();
    say(`Server address set to ${$('com1').value}. It is stored when you dial.`);
  }

  function toggleRemember() {
    if (!page.persistent) return;
    $('remember').checked = !$('remember').checked;
    say($('remember').checked
      ? 'The disks will be remembered on this device. They have excellent memories.'
      : 'Nothing will be kept on this device after the next dial. Not even the disks.');
  }

  function toggleAnim() {
    adv.anim = !adv.anim;
    store.set(ANIM_KEY, adv.anim ? '1' : '0', localStorage);
    InnScenes.setAnimation(adv.anim);
  }

  // ---------- the menu bar ----------

  let providers = [];

  function menus() {
    const game = [
      { label: 'Load disks…', fn: chooseFolder },
      { label: 'Load disk image (.zip)…', fn: chooseZip },
      { sep: true },
      { label: 'Dial', fn: dial, off: () => page.started },
      { label: 'Hang up', fn: hangUp, off: () => !page.started },
    ];
    if (providers.length) {
      game.push({ sep: true });
      for (const p of providers) game.push({ label: `Sign in with ${p.name}`, fn: () => { location.href = p.href; } });
    }
    return [
      { name: 'Game', items: game },
      { name: 'Options', items: [
        { label: 'Server address…', fn: openServerDialog },
        { label: 'Remember on this device', check: () => $('remember').checked, fn: toggleRemember, off: () => !page.persistent },
        { label: 'Animation', check: () => adv.anim, fn: toggleAnim },
      ] },
      { name: 'Scene', items: InnScenes.ids.map((id) => ({
        label: id.charAt(0).toUpperCase() + id.slice(1), radio: () => InnScenes.id() === id, fn: () => InnScenes.setScene(id, true) })) },
      { name: 'Help', items: [
        { label: 'How do I play?', fn: () => say(
          'Click the glinting things: first the disks, then the telephone. The rest is scenery with opinions. Drop a folder or a .zip anywhere on the picture to hand over the disks. The menu bar lives at the top edge (F10 on the keyboard) and can do everything the picture can.',
          { sticky: true, actions: [{ label: 'OK', fn: narrHide }] }) },
        { label: 'About', fn: () => say(
          'Online world, an unofficial browser port of an old online-game client. It runs your own copy of the original files, which stay on this device. Free software, GPL-3.0-or-later.',
          { sticky: true, actions: [{ label: 'OK', fn: narrHide }] }) },
      ] },
    ];
  }

  const bar = { open: -1, items: [], cur: -1, hideTimer: 0 };

  function barShow() { clearTimeout(bar.hideTimer); bar.hideTimer = 0; ui.bar.classList.add('show'); }

  function barLater() {
    if (bar.hideTimer) return;
    bar.hideTimer = setTimeout(() => {
      bar.hideTimer = 0;
      if (bar.open < 0 && !ui.bar.matches(':hover, :focus-within')) ui.bar.classList.remove('show');
    }, BAR_HIDE_MS);
  }

  const tops = () => [...ui.bar.querySelectorAll('[data-top]')];
  const enabled = (n) => [...ui.pop.querySelectorAll('[role^="menuitem"]')].filter((i) => i.getAttribute('aria-disabled') !== 'true');

  function closeMenu(focusBack) {
    if (bar.open >= 0) tops()[bar.open].setAttribute('aria-expanded', 'false');
    bar.open = -1;
    ui.pop.hidden = true;
    ui.bar.classList.remove('open');
    if (focusBack) { const t = tops()[bar.cur < 0 ? 0 : bar.cur]; if (focusBack === 'page') { document.activeElement.blur(); } else t.focus(); }
    barLater();
  }

  function openMenu(i, focusItem) {
    const spec = menus();
    const top = tops()[i];
    if (bar.open >= 0) tops()[bar.open].setAttribute('aria-expanded', 'false');
    bar.open = i; bar.cur = i;
    for (const t of tops()) t.tabIndex = t === top ? 0 : -1;
    top.setAttribute('aria-expanded', 'true');
    ui.bar.classList.add('open');
    barShow();
    ui.pop.replaceChildren();
    ui.pop.setAttribute('aria-label', spec[i].name);
    for (const item of spec[i].items) {
      if (item.sep) { ui.pop.append(el('div', { role: 'separator', className: 'sep' })); continue; }
      const role = item.check ? 'menuitemcheckbox' : item.radio ? 'menuitemradio' : 'menuitem';
      const on = item.check ? item.check() : item.radio ? item.radio() : null;
      const li = el('div', { role, tabIndex: -1, className: 'mi' });
      if (on !== null) li.setAttribute('aria-checked', String(on));
      li.append(el('span', { className: 'tick', textContent: on ? (item.radio ? '•' : '✓') : '' }), el('span', { textContent: item.label }));
      if (item.off && item.off()) li.setAttribute('aria-disabled', 'true');
      li.addEventListener('click', (e) => { e.stopPropagation(); activate(li, item); });
      li.addEventListener('mouseenter', () => !li.getAttribute('aria-disabled') && li.focus());
      ui.pop.append(li);
    }
    const r = top.getBoundingClientRect();
    ui.pop.hidden = false;
    ui.pop.style.left = `${Math.min(r.left, innerWidth - ui.pop.offsetWidth - 4)}px`;
    ui.pop.style.top = `${ui.bar.offsetHeight}px`;
    if (focusItem) { const first = enabled()[0]; if (first) first.focus(); }
  }

  function activate(li, item) {
    if (li.getAttribute('aria-disabled') === 'true') return;
    closeMenu('page');
    item.fn();
  }

  function moveItem(d) {
    const list = enabled();
    if (!list.length) return;
    const i = list.indexOf(document.activeElement);
    list[(i + d + list.length) % list.length].focus();
  }

  function focusBar() {
    barShow();
    const t = tops()[Math.max(0, bar.cur)];
    t.tabIndex = 0;
    t.focus();
  }

  function wireMenu() {
    ui.bar.addEventListener('pointerenter', barShow);
    ui.bar.addEventListener('pointerleave', barLater);
    document.addEventListener('pointermove', (e) => {
      if (!landing()) return;
      adv.lastAct = Date.now();
      if (e.clientY <= 6) barShow(); else if (ui.bar.classList.contains('show')) barLater();
    });
    tops().forEach((t, i) => {
      t.addEventListener('click', (e) => { e.stopPropagation(); if (bar.open === i) closeMenu(true); else openMenu(i, false); });
      t.addEventListener('pointerenter', () => { if (bar.open >= 0 && bar.open !== i) openMenu(i, false); });
    });
    document.addEventListener('pointerdown', (e) => {
      if (bar.open >= 0 && !ui.bar.contains(e.target) && !ui.pop.contains(e.target)) closeMenu(false);
    });
    let altAlone = false;
    document.addEventListener('keydown', (e) => {
      if (!landing()) return;
      adv.lastAct = Date.now();
      if (!ui.dlg.hidden) return dialogKey(e);
      if (e.key === 'Alt') { altAlone = true; return; }
      altAlone = false;
      const inBar = ui.bar.contains(document.activeElement) || ui.pop.contains(document.activeElement);
      if (e.key === 'F10' && !e.shiftKey) {
        e.preventDefault();
        if (inBar) closeMenu('page'); else focusBar();
        return;
      }
      if (e.key === 'Escape') {
        if (bar.open >= 0) { e.preventDefault(); closeMenu(true); }
        else if (inBar) { e.preventDefault(); closeMenu('page'); }
        else narrHide();
        return;
      }
      if (!inBar) return;
      const n = tops().length;
      const k = e.key;
      const inPop = ui.pop.contains(document.activeElement);
      if (k === 'ArrowRight' || k === 'ArrowLeft') {
        e.preventDefault();
        const next = (Math.max(0, bar.cur) + (k === 'ArrowRight' ? 1 : n - 1)) % n;
        if (bar.open >= 0) openMenu(next, true); else { bar.cur = next; focusBar(); }
      } else if (k === 'ArrowDown' || k === 'ArrowUp') {
        e.preventDefault();
        if (bar.open < 0) openMenu(Math.max(0, bar.cur), true);
        else if (!inPop) { const l = enabled(); (k === 'ArrowDown' ? l[0] : l[l.length - 1]).focus(); }
        else moveItem(k === 'ArrowDown' ? 1 : -1);
      } else if (k === 'Home' || k === 'End') {
        e.preventDefault();
        const l = inPop ? enabled() : tops();
        l[k === 'Home' ? 0 : l.length - 1].focus();
      } else if (k === 'Enter' || k === ' ') {
        e.preventDefault();
        if (inPop) document.activeElement.click();
        else if (bar.open === bar.cur && bar.open >= 0) closeMenu(true);
        else openMenu(Math.max(0, bar.cur), true);
      } else if (k === 'Tab') {
        closeMenu(false);
      }
    }, true);
    document.addEventListener('keyup', (e) => {
      if (e.key === 'Alt' && altAlone && landing() && ui.dlg.hidden) {
        e.preventDefault();
        if (ui.bar.contains(document.activeElement) || ui.pop.contains(document.activeElement)) closeMenu('page'); else focusBar();
      }
      altAlone = false;
    });
    ui.bar.addEventListener('focusin', (e) => {
      barShow();
      const i = tops().indexOf(e.target);
      if (i >= 0) bar.cur = i;
    });
    ui.bar.addEventListener('focusout', () => barLater());
  }

  function dialogKey(e) {
    if (e.key === 'Escape') { e.preventDefault(); closeServerDialog(); return; }
    if (e.key === 'Enter' && e.target === ui.srv) { e.preventDefault(); saveServerDialog(); return; }
    if (e.key !== 'Tab') return;
    const ctl = [ui.srv, ui.srvOk, ui.srvCancel];
    const i = ctl.indexOf(document.activeElement);
    e.preventDefault();
    ctl[(i + (e.shiftKey ? ctl.length - 1 : 1)) % ctl.length].focus();
  }

  // ---------- the telephone box (the dial-up log) ----------

  // The box sits where the scene says (`termAt`, a scene corner on grass, sky or water that is not
  // the focal art), or docks full width at the bottom on a phone.
  function placeTerm() {
    const t = ui.term;
    const sc = InnScenes.scene();
    const dock = innerWidth < 700 || !sc || !sc.termAt;
    t.classList.toggle('dock', dock);
    t.style.left = t.style.top = '';
    if (dock) return;
    const w = Math.min(340, innerWidth - 32);
    t.style.width = `${w}px`;
    const p = InnScenes.screenRect([sc.termAt[0], sc.termAt[1], 0, 0]);
    t.style.left = `${Math.max(16, Math.min(p.x, innerWidth - w - 16))}px`;
    t.style.top = `${Math.max(40, Math.min(p.y, innerHeight - t.offsetHeight - 16))}px`;
  }

  function termOpen() {
    ui.term.dataset.open = '1';
    ui.term.hidden = false;
    ui.narr.hidden = true;
    placeTerm();
  }

  function termButtons(kind) {
    ui.termActs.replaceChildren();
    const add = (label, fn, primary) => {
      const b = el('button', { type: 'button', className: `abtn${primary ? ' primary' : ''}`, textContent: label });
      b.addEventListener('click', fn);
      ui.termActs.append(b);
      return b;
    };
    if (kind === 'failed') { add('Try again', redial, true).focus(); add('Hang up', hangUp); } else add('Hang up', hangUp);
  }

  // ---------- the iris-out into the game ----------

  function transition(done) {
    if (reduced()) return done();
    const sc = InnScenes.scene();
    const r = sc && sc.spark && sc.spark.phone ? InnScenes.screenRect(sc.spark.phone) : null;
    const cx = r ? r.x + r.w / 2 : innerWidth / 2, cy = r ? r.y + r.h / 2 : innerHeight / 2;
    const far = Math.hypot(Math.max(cx, innerWidth - cx), Math.max(cy, innerHeight - cy)) + 4;
    const iris = ui.iris;
    iris.hidden = false;
    const paint = (rad) => { iris.style.background = `radial-gradient(circle at ${cx}px ${cy}px, transparent ${rad}px, #000 ${rad + 1}px)`; };
    const t0 = performance.now();
    const ease = (k) => k * k * (3 - 2 * k);
    const closing = (now) => {
      const k = Math.min(1, (now - t0) / IRIS_CLOSE_MS);
      paint(far * (1 - ease(k)));
      if (k < 1) return requestAnimationFrame(closing);
      done();
      const t1 = performance.now();
      const opening = (n2) => {
        const j = Math.min(1, (n2 - t1) / IRIS_OPEN_MS);
        paint(far * ease(j));
        if (j < 1) return requestAnimationFrame(opening);
        iris.hidden = true;
      };
      requestAnimationFrame(opening);
    };
    requestAnimationFrame(closing);
  }

  // ---------- hooks called by page.js ----------

  function refresh() {
    if (!ui) return;
    const has = haveDisks();
    ui.inv.hidden = !has;
    InnScenes.setSpark('disks', adv.ready && !has && !page.started);
    InnScenes.setSpark('phone', adv.ready && has && !page.started);
  }

  const hooks = {
    active: !CLASSIC,

    ready() {
      adv.ready = true;
      refresh();
      if (adv.welcomed) return;
      adv.welcomed = true;
      if (page.drive === 'saved') say('Welcome back. The disks are right where you left them, and slightly warmer.');
      else if (page.drive === 'preloaded') say('The disks came packed with this build. Convenient. Now, about that telephone.');
      if (store.get('inn.redial') === '1' && haveDisks()) { store.set('inn.redial', ''); dial(); }
    },

    drive: refresh,

    picked(count) {
      refresh();
      award('disks', POINTS.disks);
      const o = objects().disks || {};
      say(`You now have the disks (${count} files). They smell faintly of 1993. ${o.got || 'Now find the telephone.'}`, { near: sparkX('phone') });
    },

    error(text) {
      const tail = /not found/.test(text) ? ' Disks are fussy about their folders.' : '';
      say(`${text}${/[.!?]$/.test(text) ? '' : '.'}${tail}`, { bad: true, actions: diskButtons() });
    },

    dropped(kind) {
      if (kind === 'bad') return;
      say('Disks, from above. Somebody has been thoughtful.');
    },

    dialBegin() {
      refresh();
      ui.termTitle.textContent = 'Dialing';
      ui.termLights.className = 'tlights tx';
      termButtons('dialing');
      termOpen();
    },

    modem(text) {
      ui.termText.textContent = text;
      ui.termText.append(document.createTextNode('\n'), el('span', { className: 'cur' }));
    },

    online() {
      ui.termTitle.textContent = 'Connected';
      ui.termLights.className = 'tlights cd';
      award('connect', POINTS.connect);
    },

    failed() {
      ui.termTitle.textContent = 'No answer';
      ui.termLights.className = 'tlights err';
      termButtons('failed');
    },

    transition,

    providers(list) {
      providers = list;
    },

    providersChanged: () => {},

    sceneChanged() {
      if (!ui) return;
      narrHide();
      refresh();
      if (ui.term.dataset.open === '1') placeTerm();
    },

    click,
    hover,
    say,
  };

  // ---------- building the interface ----------

  function build() {
    const files = el('div', { hidden: true },
      el('input', { id: 'adv-dir', type: 'file', webkitdirectory: true, multiple: true }),
      el('input', { id: 'adv-zip', type: 'file', accept: '.zip,application/zip' }));
    const bar = el('div', { id: 'menubar', className: 'adv', role: 'menubar', ariaLabel: 'Game menu' });
    ui = { bar };
    for (const [i, name] of ['Game', 'Options', 'Scene', 'Help'].entries()) {
      const b = el('button', { type: 'button', role: 'menuitem', textContent: name, tabIndex: i ? -1 : 0 });
      b.dataset.top = '1';
      b.setAttribute('aria-haspopup', 'menu');
      b.setAttribute('aria-expanded', 'false');
      bar.append(b);
    }
    ui.scoreText = el('span', { textContent: `${score.pts} of ${SCORE_MAX}` });
    ui.score = el('span', { className: 'score' }, el('span', { className: 'lbl', textContent: 'Score: ' }), ui.scoreText);
    bar.append(el('span', { className: 'spacer' }), ui.score);
    ui.pop = el('div', { id: 'menupop', className: 'adv', role: 'menu', hidden: true });
    ui.inv = el('div', { id: 'inv', className: 'adv', hidden: true, title: 'You have the disks', role: 'img', ariaLabel: 'Inventory: the disks' });
    ui.inv.innerHTML = '<svg viewBox="0 0 12 12" shape-rendering="crispEdges" aria-hidden="true"><path fill="#1c2260" d="M0 0h10l2 2v10H0z"/>'
      + '<path fill="#3c4cc0" d="M1 1h9l1 1v9H1z"/><path fill="#c8ccd8" d="M3 1h6v3H3z"/><path fill="#5c6074" d="M7 1.5h1v2H7z"/>'
      + '<path fill="#f6f0dc" d="M2 6h8v5H2z"/><path fill="#2c2c44" d="M3 7.5h5v1H3z"/></svg>';
    ui.narrText = el('p', { className: 'text', ariaHidden: 'true' });
    ui.narrSr = el('p', { className: 'sr' });
    ui.acts = el('div', { className: 'acts' });
    ui.narr = el('div', { id: 'narr', className: 'adv', hidden: true, role: 'status' }, ui.narrText, ui.narrSr, ui.acts);
    ui.narr.addEventListener('click', (e) => { if (!e.target.closest('button')) narrHide(); });
    ui.who = el('p', { className: 'who' });
    ui.termTitle = el('span', { textContent: 'Dialing' });
    ui.termLights = el('span', { className: 'tlights', ariaHidden: 'true' }, el('i', { className: 'tx', textContent: 'TX' }), el('i', { className: 'cd', textContent: 'CD' }));
    ui.termText = el('pre', { className: 'modem' });
    ui.termActs = el('div', { className: 'acts' });
    ui.term = el('section', { id: 'term', className: 'adv', hidden: true, role: 'region', ariaLabel: 'Telephone line', ariaLive: 'polite' },
      el('div', { className: 'bar' }, ui.termTitle, ui.termLights), ui.who, ui.termText, ui.termActs);
    ui.srv = el('input', { id: 'adv-srv', className: 'input', type: 'text', spellcheck: false, autocomplete: 'off' });
    ui.srvErr = el('p', { className: 'err', role: 'alert' });
    ui.srvOk = el('button', { type: 'button', className: 'abtn primary', textContent: 'OK' });
    ui.srvCancel = el('button', { type: 'button', className: 'abtn', textContent: 'Cancel' });
    ui.dlg = el('div', { id: 'dlg', className: 'adv', hidden: true },
      el('div', { role: 'dialog', ariaModal: 'true', className: 'box' },
        el('h2', { id: 'dlg-title', textContent: 'Server address' }),
        el('p', { className: 'hint', textContent: 'Where the game server listens, such as ws://host:2316. Leave it empty for the default.' }),
        ui.srv, ui.srvErr, el('div', { className: 'acts' }, ui.srvOk, ui.srvCancel)));
    ui.dlg.firstChild.setAttribute('aria-labelledby', 'dlg-title');
    ui.iris = el('div', { id: 'iris', hidden: true });
    document.body.append(files, bar, ui.pop, ui.inv, ui.narr, ui.term, ui.dlg, ui.iris);
    ui.srvOk.addEventListener('click', saveServerDialog);
    ui.srvCancel.addEventListener('click', closeServerDialog);
    ui.dlg.addEventListener('pointerdown', (e) => { if (e.target === ui.dlg) closeServerDialog(); });
    $('adv-dir').addEventListener('change', (e) => e.target.files.length && pickFiles(globalThis.filesFromFolder, e.target.files));
    $('adv-zip').addEventListener('change', (e) => e.target.files[0] && pickFiles(globalThis.filesFromZip, e.target.files[0]));
  }

  // pick(), filesFromFolder() and filesFromZip() are page.js's loaders.
  const pickFiles = (read, source) => globalThis.pick(read, source);

  hooks.init = function init() {
    document.body.classList.add('adventure');
    build();
    wireMenu();
    adv.anim = store.get(ANIM_KEY, localStorage) !== '0';
    if (!adv.anim) InnScenes.setAnimation(false);
    window.addEventListener('resize', () => { if (ui.term.dataset.open === '1') placeTerm(); });
    setInterval(() => {
      if (!landing() || page.started || adv.hints >= HINT_MAX || bar.open >= 0 || !ui.dlg.hidden) return;
      if (Date.now() - adv.lastAct < HINT_IDLE_MS) return;
      const role = haveDisks() ? 'phone' : 'disks';
      const spark = InnScenes.scene() && InnScenes.scene().spark && InnScenes.scene().spark[role];
      adv.hints++;
      if (spark) InnScenes.panTo(spark[0] + spark[2] / 2);
      say((objects()[role] || {}).hint || NUDGE[role], { near: sparkX(role) });
    }, 1000);
    refresh();
  };

  return hooks;
})();
