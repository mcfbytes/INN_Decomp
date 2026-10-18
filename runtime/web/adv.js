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
  const POINTS = { disks: 10, dial: 5, connect: 25, signin: 5, gag: 1 };
  const HINT_IDLE_MS = 10000, HINT_MAX = 2, TYPE_MS = 22, SHOW_MS = 7000, BAR_HIDE_MS = 1400;
  const IRIS_CLOSE_MS = 480, IRIS_OPEN_MS = 320, NOW_MS = 3000;

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
    get(key, area) { try { return (area === 'local' ? localStorage : sessionStorage).getItem(key); } catch { return null; } },
    set(key, value, area) { try { (area === 'local' ? localStorage : sessionStorage).setItem(key, value); } catch { /* kept in memory only */ } },
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
      if (a.href) return el('a', { className: 'abtn link', href: a.href, target: '_blank', rel: 'noopener', textContent: a.label });
      const b = el('button', { type: 'button', className: 'abtn', textContent: a.label });
      b.addEventListener('click', (e) => { e.stopPropagation(); a.fn(); });
      return b;
    }));
    showCards(o.cards || []);
    ui.cards.hidden = !(o.cards && o.cards.length);
    ui.hold.hidden = !o.hold;
    const sticky = o.sticky || o.bad || o.hold || (o.actions && o.actions.length) || (o.cards && o.cards.length);
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
    if (o.focus && !ui.narr.hidden) { const first = ui.narr.querySelector('button'); if (first) first.focus(); }
  }

  // ---------- calling cards ----------

  // A calling card is a button: real lettering over a face painted with the scenes' kit
  // (InnScenes.paintSmall). Each provider id has a card of its own, the same on every scene: a band with
  // a made-up series name, a motif and a fine-print gag. The scene adds its paper, its ink, a postmark and
  // one line under the row, and the faces are repainted at dusk. Original art throughout: a provider is
  // only its configured name in plain lettering, never a logo, a mark or its colours (docs/web.md).
  const PX = 2;   // CSS pixels per painted pixel
  const CARDS = {
    microsoft: ['#6e2a36', 'LAMP ROOM EXCHANGE', 'Do not fold, spindle or feed to serpents.', artLighthouse],
    google: ['#5c4630', 'PIGEON POST & CO.', 'Delivered by pigeon. Tip in crumbs only.', artPigeon],
    discord: ['#1f5a5a', 'UP & AWAY PARTY LINE', 'Altitude may vary. Ballast is not refundable.', artBalloon],
    github: ['#8a4a1c', 'PATCH CORD SOCIETY', 'Valid for one (1) identity. Pointy end in.', artPlug],
    dev: ['#7c3e52', 'CACTUS JUNCTION', 'Test line. The cactus is not a real operator.', artCactus],
    own: ['#6b5a42', 'PRIVATE LINE', 'Unlisted. The operator pretends not to know you.', artKey],
    s0: ['#2c3a6c', 'STRING & CAN CO.', 'Keep the string taut. Do not knot.', artTinCans],
    s1: ['#2e5a3a', 'TICK-TOCK TRUNK LINE', 'Billed by the minute. The watch runs slow.', artWatch],
    s2: ['#5a3a6c', 'ROUND ROBIN EXCHANGE', 'Batteries not included. Operator not included either.', artDial],
    s3: ['#3a4a5c', 'DULY STAMPED & SONS', 'Void where prohibited. Also on Tuesdays.', artStamp],
  };
  // The postmarks' pictures, 7 x 7 pixels of ink in the middle of the rings.
  const anchor = '0011100 0010100 0011100 1111111 0001000 1001001 0111110';
  const pie = '0101000 1010000 0000001 0000111 0011111 1111111 1111111';
  const moon = '0011100 0110000 1100010 1100111 1100010 0110000 0011100';
  const clockFace = '0011100 0100010 1001001 1001111 1000001 0100010 0011100';
  const volcano = '0110000 0110110 0000110 0011100 0111110 1111111 1111111';

  // Per scene: ink, paper by day and at dusk, the dusk tint, the line, the postmark's picture.
  const POST = {
    harbor: ['#1e4a78', '#f1ede0', '#c3c9dc', '#34488a', 'Waterproof to a depth of one (1) small splash.', anchor],
    diner: ['#a02a2a', '#f8eccf', '#e2bcb2', '#8a3a6a', 'Good for one call or one slice of pie, not both.', pie],
    tower: ['#4a2a7a', '#eef0d8', '#bfc5da', '#2a3a6a', 'Not redeemable for newts, or by them.', moon],
    town: ['#2a2a3a', '#f6f0dc', '#dccaa8', '#5a3a5a', 'Postmarked a quarter past something.', clockFace],
    valley: ['#7a3a12', '#f6e8cc', '#d8bcaa', '#6a2a3a', 'Fireproof up to one (1) polite rumble.', volcano],
  };
  const post = () => POST[InnScenes.id()] || POST.town;

  function cardKey(c) {
    if (c.own) return 'own';
    if (c.id !== 'own' && Object.hasOwn(CARDS, c.id)) return c.id;
    let h = 7;
    for (const ch of String(c.id)) h = (Math.imul(h, 31) + ch.charCodeAt(0)) >>> 0;
    return `s${h % 4}`;
  }

  const hx = (a, b, k) => {
    const p = (s) => [1, 3, 5].map((i) => parseInt(s.substr(i, 2), 16)), B = p(b);
    return '#' + p(a).map((v, i) => Math.round(v + (B[i] - v) * k).toString(16).padStart(2, '0')).join('');
  };

  const faces = new Map();
  const cardSeen = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver((es) => es.forEach((e) => paintCard(e.target)));

  function paintCard(b) {
    const cv = b.firstChild, w = Math.round(b.clientWidth / PX), h = Math.round(b.clientHeight / PX);
    if (w < 24 || h < 24 || !InnScenes.paintSmall) return;
    const k = [b.dataset.card, InnScenes.id(), InnScenes.dark(), w, h].join();
    let f = faces.get(k);
    if (!f) {
      if (faces.size > 40) faces.clear();
      faces.set(k, f = cardFace(CARDS[b.dataset.card], w, h));
    }
    cv.width = w; cv.height = h;
    cv.getContext('2d').drawImage(f, 0, 0);
  }

  function cardFace(d, w, h) {
    const [ink, day, dusk, night, , mark] = post(), dk = InnScenes.dark();
    return InnScenes.paintSmall(w, h, dk, (g, P) => {
      const T = (c) => (dk ? hx(hx(c, night, 0.3), '#000000', 0.08) : c);
      const paper = dk ? dusk : day, band = T(d[0]);
      P.rect(g, -2, -2, w + 4, h + 4, [hx(paper, '#ffffff', 0.4), paper, hx(paper, '#7a6040', 0.16)], { wob: 0 });
      P.dabs(g, 0, 12, w, h - 12, [hx(paper, '#ffffff', 0.5), hx(paper, '#6a5030', 0.18)], (w * h) >> 5, 1.3, 5, 0.3);
      P.shape(g, [[-2, -2], [w + 2, -2], [w + 2, 10], [-2, 10.4]], [hx(band, '#ffffff', 0.18), band, hx(band, '#000000', 0.3)], { wob: 0.5, seed: 3 });
      P.dabs(g, 0, 0, w, 9, [hx(band, '#ffffff', 0.2), hx(band, '#000000', 0.25)], w >> 2, 1.5, 9, 0.3);
      P.line(g, [[2, 11], [w - 2, 11]], hx(band, paper, 0.5), 1);
      P.line(g, [[2, h - 2.5], [w - 2, h - 2.5]], band, 1, { wob: 0.4 });
      const m = Math.min(36, h - 16), s = m / 36;
      g.save(); g.translate(w - m - 3, h - m - 4); g.scale(s, s);
      d[3](g, P, T, dk);
      g.restore();
      // the postmark: two rings with worn lettering, the scene's picture, three wavy cancel lines
      const cx = w - 15, cy = 22, inkc = T(ink);
      g.save();
      g.strokeStyle = g.fillStyle = P.alpha(inkc, 0.82); g.lineWidth = 1;
      for (const r of [9.5, 6.5]) { g.beginPath(); g.arc(cx + 0.5, cy + 0.5, r, 0, 7); g.stroke(); }
      for (let i = 0; i < 18; i++) if (i % 6 < 4) { const a = i * 0.349 - 2.6; g.fillRect(Math.round(cx + Math.cos(a) * 8), Math.round(cy + Math.sin(a) * 8), 1, 1); }
      mark.split(' ').forEach((row, y) => { for (let x = 0; x < 7; x++) if (row[x] === '1') g.fillRect(cx - 3 + x, cy - 3 + y, 1, 1); });
      for (let k = -1; k < 2; k++) {
        g.beginPath();
        for (let x = cx - 29; x <= cx - 10; x++) g.lineTo(x, cy + k * 3.6 + Math.sin(x * 0.55) * 1.1);
        g.stroke();
      }
      g.restore();
      P.grain(g, cx - 33, cy - 10, 44, 21, paper, 50, 11, 0.7);
      if (dk) { g.fillStyle = P.radial(g, w * 0.4, h * 0.4, 4, w * 0.75, [[0, P.alpha('#ffe0a0', 0.12)], [1, P.alpha('#0c0820', 0.3)]]); g.fillRect(0, 0, w, h); }
    });
  }

  // The motifs, each in a 36 x 36 box. T tones a colour for the scene's dusk.
  function artLighthouse(g, P, T, dk) {
    P.shape(g, [[-2, 29], [38, 28.5], [38, 37], [-2, 37]], [T('#8aa4b0'), T('#4a6478')], { wob: 0.6, seed: 4, v: true });
    P.line(g, [[2, 32], [8, 31.6]], T('#e0eaec'), 1); P.line(g, [[25, 33], [33, 32.6]], T('#e0eaec'), 1);
    P.blob(g, 18, 30, 11, 3.6, [T('#9a9488'), T('#4c463c')], { seed: 5, edge: T('#2a241c'), ea: 0.6 });
    g.fillStyle = P.alpha('#fff4c0', dk ? 0.5 : 0.32);
    g.beginPath(); g.moveTo(17, 6); g.lineTo(-4, 1); g.lineTo(-4, 11); g.moveTo(19, 6); g.lineTo(40, 1); g.lineTo(40, 11); g.fill();
    const tw = [[13.5, 29], [22.5, 29], [20.5, 10], [15.5, 10]];
    P.shape(g, tw, [T('#fbf4e0'), T('#c8bca0')], { wob: 0.3, seed: 6 });
    P.within(g, tw, { wob: 0.3, seed: 6 }, () => {
      for (const y of [12.5, 19, 25.5]) P.rect(g, 10, y, 16, 3.2, [T('#b04450'), T('#6e2430')], { wob: 0.3 });
      g.fillStyle = P.alpha('#2a1810', 0.25); g.fillRect(19.5, 10, 4, 20);
    });
    P.trace(g, tw, 0.3, 6); g.strokeStyle = P.alpha(T('#2a1e18'), 0.7); g.stroke();
    P.rect(g, 12.5, 8.6, 11, 1.8, T('#3a2c28'), { wob: 0.2 });
    P.rect(g, 15.5, 4, 5, 4.6, ['#fffbe0', '#f0c860'], { wob: 0.2, edge: T('#2a1e18'), ea: 0.7 });
    P.shape(g, [[14.5, 4.4], [21.5, 4.4], [18, 0.4]], [T('#6a4038'), T('#2a1a18')], { wob: 0.2, seed: 7 });
    P.glow(g, 18, 6.3, 7, '#fff0b0', dk ? 0.7 : 0.45);
  }

  function artPigeon(g, P, T) {
    P.rect(g, -1, 30, 38, 2.8, [T('#b0804e'), T('#6a4628')], { wob: 0.4, seed: 2, edge: T('#2a180a'), ea: 0.5 });
    P.line(g, [[15, 25], [14, 30.5]], T('#c86a5a'), 1); P.line(g, [[19.5, 25], [19.5, 30.5]], T('#c86a5a'), 1);
    P.shape(g, [[2, 16], [9, 15.5], [10, 21.5], [3.5, 22.5]], [T('#8a8698'), T('#4c485c')], { wob: 0.4, seed: 3 });
    P.blob(g, 17, 19.5, 10, 6.5, [T('#d0ccd8'), T('#8a8698'), T('#5c586a')], { seed: 4, rot: -0.25, edge: T('#2a2834'), ea: 0.6 });
    P.blob(g, 14, 18, 6.5, 3.6, [T('#aca8b8'), T('#6c687c')], { seed: 5, rot: -0.2 });
    P.line(g, [[9.5, 18], [16.5, 17]], P.alpha(T('#3c384c'), 0.8), 1); P.line(g, [[10.5, 20.3], [17.5, 19.3]], P.alpha(T('#3c384c'), 0.8), 1);
    P.blob(g, 25, 13.5, 3.6, 4.4, [T('#c09ab2'), T('#7a6288')], { seed: 6, rot: 0.3 });
    P.blob(g, 27, 9, 3.7, 3.3, [T('#dad6e2'), T('#8a8698')], { seed: 7, edge: T('#2a2834'), ea: 0.6 });
    P.shape(g, [[30.2, 8.6], [33.8, 10], [30.2, 10.8]], T('#d8a070'), { wob: 0.1, seed: 8 });
    g.fillStyle = T('#e07a2a'); g.fillRect(27.5, 7.5, 2, 2); g.fillStyle = T('#140c10'); g.fillRect(28, 8, 1, 1);
    // the satchel: a strap across the breast, a leather bag, a letter, a brass buckle
    P.line(g, [[24.5, 12], [17, 24]], T('#5a3418'), 1.2);
    P.rect(g, 21, 18.6, 4, 2.6, T('#fbf6e8'), { wob: 0.1, edge: T('#8a7a60'), ea: 0.6 });
    P.rect(g, 14.5, 21, 8, 6, [T('#c07c40'), T('#7a4620')], { wob: 0.3, seed: 9, edge: T('#2a160a'), ea: 0.7 });
    P.rect(g, 14.5, 21, 8, 2.4, T('#8a5228'), { wob: 0.2, seed: 10 });
    g.fillStyle = T('#f0d070'); g.fillRect(18, 23, 1, 1);
  }

  function artBalloon(g, P, T, dk) {
    for (const [x, y, r] of [[5, 30, 6], [32, 24, 4.5]]) P.blob(g, x, y, r, r * 0.4, [T('#ffffff'), T('#c8d0e0')], { seed: x, wob: 0.6 });
    const env = [[18, 0.6], [25, 2.2], [29.6, 7], [30, 13], [27, 19], [23, 23.5], [13, 23.5], [9, 19], [6, 13], [6.4, 7], [11, 2.2]];
    P.shape(g, env, T('#f0e2c0'), { wob: 0.3, seed: 3 });
    P.within(g, env, { wob: 0.3, seed: 3 }, () => {
      const hw = (y) => (y < 13 ? 12.5 * Math.sqrt(Math.max(0, 1 - ((13 - y) / 12.6) ** 2)) : 12.5 - (y - 13) * 0.6);
      for (let i = 0; i < 7; i++) {
        if (i % 2) continue;
        const pts = [];
        for (let y = 0; y <= 24; y += 2) pts.push([18 + hw(y) * (i / 3.5 - 1), y]);
        for (let y = 24; y >= 0; y -= 2) pts.push([18 + hw(y) * ((i + 1) / 3.5 - 1), y]);
        P.shape(g, pts, i % 4 ? T('#c8643c') : T('#2a8a8a'), { wob: 0.2, seed: i });
      }
      g.fillStyle = P.gradRaw(g, 6, 0, 30, 0, [[0, P.alpha('#ffffff', 0.3)], [0.45, P.alpha('#ffffff', 0)], [1, P.alpha('#140a20', 0.4)]]);
      g.fillRect(4, 0, 28, 25);
    });
    P.trace(g, env, 0.3, 3); g.strokeStyle = P.alpha(T('#2a1a14'), 0.6); g.stroke();
    for (const [a, b] of [[13.5, 15], [18, 18], [22.5, 21]]) P.line(g, [[a, 23.5], [b, 28.6]], T('#5a4030'), 0.8);
    P.glow(g, 18, 25, 4, '#ffb040', dk ? 0.8 : 0.5);
    P.rect(g, 14.2, 28.4, 7.6, 5, [T('#d09c54'), T('#7a5228')], { wob: 0.3, seed: 5, edge: T('#2a180a'), ea: 0.7 });
    g.fillStyle = P.alpha(T('#3a2210'), 0.4);
    for (let x = 15.5; x < 21; x += 2) g.fillRect(x, 29, 1, 4);
    P.line(g, [[21.8, 30], [24, 32.5]], T('#5a4030'), 0.8);
    P.blob(g, 24.4, 33.4, 1.4, 1.6, [T('#d8c08a'), T('#8a7040')], { seed: 6, wob: 0.1, n: 8 });
  }

  function artPlug(g, P, T) {
    P.rect(g, 19, -2, 19, 17, [T('#9a6a3e'), T('#4a2c16')], { wob: 0.3, seed: 2, edge: T('#1c0e06'), ea: 0.7 });
    for (const [x, y] of [[25, 4.5], [32, 4.5], [25.5, 10.5], [32, 10.5]]) {
      P.blob(g, x, y, 2.2, 2.2, [T('#ffe08a'), T('#a87a24')], { wob: 0.1, n: 8, seed: x + y });
      P.blob(g, x, y, 0.9, 0.9, T('#140a04'), { wob: 0, n: 6 });
    }
    const cord = [[-2, 35], [5, 35], [9.5, 33], [7, 30], [3.5, 31.5], [6, 34], [10.5, 31.5], [10.5, 27]];
    P.line(g, cord, T('#6a2418'), 2.4, { wob: 0.2 });
    P.line(g, cord.map(([x, y]) => [x - 0.4, y - 0.6]), T('#b8584a'), 0.7, { wob: 0.2 });
    // the plug, on its way to a jack: bakelite handle, brass ferrule, sleeve, ring and tip
    g.save(); g.translate(10.5, 27); g.rotate(-0.83);
    P.rect(g, -1, -2.7, 10.5, 5.4, [T('#8a4434'), T('#2a1410')], { wob: 0.2, seed: 3, edge: '#0c0604', ea: 0.8 });
    g.fillStyle = P.alpha('#ffffff', 0.35); g.fillRect(0, -1.9, 8, 1);
    const br = [T('#fff0b8'), T('#b8882c')];
    P.rect(g, 9.5, -2.2, 2.8, 4.4, [T('#ffe8a0'), T('#a87a24')], { wob: 0.1, seed: 4, edge: T('#3a2808'), ea: 0.6 });
    P.rect(g, 12.3, -1.1, 5, 2.2, br, { wob: 0.05, seed: 5 });
    g.fillStyle = T('#1c140c'); g.fillRect(17.3, -1, 0.9, 2);
    P.rect(g, 18.2, -0.8, 2.4, 1.6, br, { wob: 0.05, seed: 6 });
    P.blob(g, 20.8, 0, 1.2, 1.2, br, { wob: 0.05, n: 8 });
    g.restore();
  }

  function artCactus(g, P, T) {
    P.blob(g, 18, 33.5, 16, 3.6, [T('#f4d898'), T('#c89a58')], { seed: 2 });
    const G = [T('#86b462'), T('#4c7c3a'), T('#2e5a2c')], o = { wob: 0.3, seed: 3, edge: T('#1a3018'), ea: 0.7 };
    const parts = [[[14, 33], [14, 9], [15, 6.5], [18, 5.5], [21, 6.5], [22, 9], [22, 33]],
      [[14.5, 22], [9, 22], [7, 20], [7, 12], [8.2, 10.6], [9.8, 10.6], [11, 12], [11, 18], [14.5, 18]],
      [[21.5, 19], [26, 19], [28, 17], [28, 9], [26.8, 7.6], [25.2, 7.6], [24, 9], [24, 15], [21.5, 15]]];
    for (const p of parts.slice(1)) P.shape(g, p, G, o);
    P.shape(g, parts[0], G, o);
    for (const [x, y0, y1] of [[16.4, 9, 32], [19.6, 9, 32], [9, 12, 20], [26, 9, 17]]) P.line(g, [[x, y0], [x, y1]], P.alpha(T('#1e3e1c'), 0.55), 0.8);
    for (const p of parts) P.within(g, p, o, () => P.grain(g, 6, 5, 23, 28, '#fbf4c8', 40, 4, 0.9));
    g.strokeStyle = T('#2a2a34'); g.lineWidth = 1.3;
    g.beginPath(); g.arc(18, 10.5, 5.4, 3.3, 6.12); g.stroke();
    for (const x of [12.8, 23.2]) P.blob(g, x, 11, 1.6, 2.4, [T('#6a6a7a'), T('#1c1c24')], { wob: 0.1, n: 8, seed: 6 });
    P.line(g, [[23, 12.5], [23.4, 15.5], [20.6, 17]], T('#2a2a34'), 1);
    P.blob(g, 20, 17, 1.2, 1.1, T('#1c1c24'), { wob: 0, n: 6 });
    g.fillStyle = '#ff5a40'; g.fillRect(23, 9, 1, 1);
  }

  function artKey(g, P, T) {
    const tag = [[2, 18], [13, 15], [16.5, 27], [5.5, 30], [1.5, 24]];
    P.shape(g, tag, [T('#fffbea'), T('#d8caa0')], { wob: 0.3, seed: 2, edge: T('#5a4a30'), ea: 0.7 });
    P.line(g, [[5, 23], [12, 21]], P.alpha(T('#2a3a6a'), 0.6), 0.8); P.line(g, [[6, 26], [13, 24]], P.alpha(T('#2a3a6a'), 0.6), 0.8);
    P.blob(g, 4.6, 21, 1, 1, T('#8a7a5a'), { wob: 0, n: 6 });
    P.line(g, [[4.6, 21], [10, 13], [18, 9], [23.5, 9.5]], T('#c8b890'), 0.8, { wob: 0.3 });
    g.save(); g.translate(26.5, 8.5); g.rotate(2.0);
    const B = [T('#ffe8a0'), T('#c8962c'), T('#7a5414')];
    P.blob(g, 0, 0, 4.6, 4.2, B, { wob: 0.15, n: 12, seed: 3, edge: T('#3a2808'), ea: 0.7 });
    P.blob(g, -0.6, 0, 1.6, 1.4, T('#5a3c0c'), { wob: 0, n: 8 });
    P.rect(g, 3.6, -1.9, 2.4, 3.8, B, { wob: 0.1, seed: 4 });
    P.rect(g, 5.6, -1, 16, 2, B, { wob: 0.1, seed: 5, edge: T('#3a2808'), ea: 0.6 });
    for (const [x, d] of [[15, 2.6], [17.2, 1.6], [19.2, 3], [21, 2]]) P.rect(g, x, 0.6, 1.6, d, B, { wob: 0.05, seed: x });
    g.restore();
  }

  function artTinCans(g, P, T) {
    const can = (x, y, r, sd) => {
      g.save(); g.translate(x, y); g.rotate(r);
      P.rect(g, -4.5, -6.5, 9, 13, [T('#eef0f4'), T('#a0a6b2'), T('#5c6270')], { wob: 0.2, seed: sd, edge: T('#24262c'), ea: 0.7 });
      g.fillStyle = P.alpha(T('#24262c'), 0.3);
      for (let y = -4.5; y < 6; y += 2.2) g.fillRect(-4.5, y, 9, 0.7);
      g.fillStyle = P.alpha('#ffffff', 0.5); g.fillRect(-3.4, -5.5, 1.2, 11);
      P.blob(g, 0, -6.5, 4.6, 1.8, [T('#2a2c34'), T('#0c0e14')], { wob: 0.1, n: 10, seed: sd + 2, edge: T('#c8ccd8'), ea: 0.8 });
      g.restore();
    };
    P.wire(g, [13.2, 26.1], [22.8, 11.9], 2, T('#a08860'), 0.9);
    can(7, 28, 1.27, 3);
    can(29, 10, -1.875, 7);
  }

  function artWatch(g, P, T) {
    g.fillStyle = T('#c8962c');
    for (let i = 0; i < 12; i++) { const t = i / 11; g.fillRect(-1 + 18 * t, 23 - 18 * t * (1.6 - t) + 2, 2 - (i % 2), 1 + (i % 2)); }
    g.strokeStyle = T('#c8962c'); g.lineWidth = 1;
    g.beginPath(); g.arc(20, 3.4, 2.4, 0, 7); g.stroke();
    P.rect(g, 18.4, 5, 3.2, 2.6, [T('#ffe08a'), T('#a87a24')], { wob: 0.1 });
    P.blob(g, 20, 19.5, 12, 12, [T('#ffe8a0'), T('#d8a840'), T('#8a6418')], { wob: 0.15, n: 20, seed: 3, edge: T('#3a2808'), ea: 0.8 });
    P.blob(g, 20, 19.5, 9.4, 9.4, [T('#fffbea'), T('#ddd2b6')], { wob: 0.1, n: 18, seed: 4 });
    g.fillStyle = T('#3a2c1c');
    for (let i = 0; i < 12; i++) { const a = i * 0.5236; g.fillRect(Math.round(20 + Math.cos(a) * 7.8) - 0.5, Math.round(19.5 + Math.sin(a) * 7.8) - 0.5, 1, 1); }
    P.line(g, [[20, 19.5], [19.4, 13.2]], T('#1c140c'), 1); P.line(g, [[20, 19.5], [24.6, 21.5]], T('#1c140c'), 1);
    g.fillStyle = P.alpha('#ffffff', 0.8); g.fillRect(13, 13, 2, 1); g.fillRect(12, 14, 1, 2);
  }

  function artDial(g, P, T) {
    P.blob(g, 18, 19, 15, 15, [T('#5a5464'), T('#2a2632'), T('#121018')], { wob: 0.15, n: 22, seed: 3, edge: '#08060c', ea: 0.8 });
    for (let i = 0; i < 10; i++) {
      const a = 1.45 + i * 0.524;
      P.blob(g, 18 + Math.cos(a) * 9.8, 19 + Math.sin(a) * 9.8, 2.5, 2.5, [T('#fffbea'), T('#c8bc9c')], { wob: 0.05, n: 9, seed: 10 + i, edge: '#08060c', ea: 0.7 });
      g.fillStyle = P.alpha(T('#2a2632'), 0.8); g.fillRect(Math.round(18 + Math.cos(a) * 9.8), Math.round(19 + Math.sin(a) * 9.8), 1, 1);
    }
    P.blob(g, 18, 19, 4.6, 4.6, [T('#fffbea'), T('#d8ccb0')], { wob: 0.1, n: 12, seed: 6, edge: '#08060c', ea: 0.6 });
    P.line(g, [[15.5, 19], [20.5, 19]], P.alpha(T('#a83a2a'), 0.8), 0.8);
    P.shape(g, [[28, 25], [32, 27.6], [31, 29.4], [27, 27]], [T('#eef0f4'), T('#7a808c')], { wob: 0.1, seed: 7, edge: '#14161c', ea: 0.6 });
    g.fillStyle = P.alpha('#ffffff', 0.5); g.fillRect(8, 9, 2, 1); g.fillRect(7, 10, 1, 2);
  }

  function artStamp(g, P, T) {
    P.rect(g, 0, 28, 26, 7, [T('#8a8f9a'), T('#3a3e48')], { wob: 0.3, seed: 2, edge: T('#14161c'), ea: 0.7 });
    P.rect(g, 2, 28.3, 22, 3, [T('#6a4a8a'), T('#2a1a44')], { wob: 0.2, seed: 3 });
    P.shadow(g, 25, 27, 9, 1.6, 0.25);
    g.save(); g.translate(23, 16); g.rotate(0.22); g.scale(1.3, 1.3);
    P.blob(g, 0, -10, 4, 3.4, [T('#e0a464'), T('#8a5228')], { seed: 4, edge: T('#2a160a'), ea: 0.7 });
    P.rect(g, -1.6, -7.4, 3.2, 4, [T('#d0905a'), T('#7a4620')], { wob: 0.1, seed: 5 });
    P.rect(g, -6.5, -3.6, 13, 5.4, [T('#e0b070'), T('#8a5a2c')], { wob: 0.2, seed: 6, edge: T('#2a160a'), ea: 0.7 });
    P.rect(g, -6, 1.8, 12, 1.8, [T('#a03040'), T('#4a1420')], { wob: 0.1, seed: 7 });
    g.restore();
  }

  function callingCard(c, i) {
    const b = el('button', { type: 'button', className: `ccard${c.own ? ' own' : ''}` });
    const d = CARDS[b.dataset.card = cardKey(c)];
    b.setAttribute('aria-label', c.own ? c.label : `Calling card: ${c.label}`);
    b.setAttribute('aria-describedby', `ccf${i} ccline`);
    const art = el('canvas', { width: 1, height: 1 });
    art.setAttribute('aria-hidden', 'true');
    b.append(art, el('span', { className: 'ct', textContent: d[1] }), el('span', { className: 'cn', textContent: c.label }),
      el('span', { className: 'cf', id: `ccf${i}`, textContent: d[2] }));
    b.addEventListener('click', (e) => { e.stopPropagation(); c.fn(); });
    return b;
  }

  // The row of cards and the scene's line under it; cards repaint when their size, the scene or its mode changes.
  function showCards(list) {
    if (cardSeen) cardSeen.disconnect();
    ui.cards.replaceChildren(...list.map(callingCard));
    if (list.length) ui.cards.append(el('p', { className: 'cline', id: 'ccline', textContent: post()[4] }));
    for (const b of ui.cards.querySelectorAll('.ccard')) if (cardSeen) cardSeen.observe(b); else requestAnimationFrame(() => paintCard(b));
  }

  function repaintCards() {
    if (!ui || ui.cards.hidden) return;
    for (const b of ui.cards.querySelectorAll('.ccard')) paintCard(b);
    const l = $('ccline');
    if (l) l.textContent = post()[4];
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
    wantRuntime();
    if (page.started) return say('The disks are in use right now. Hang up first if you want to swap them.');
    $('adv-dir').value = '';
    $('adv-dir').click();
  }

  function chooseZip() {
    wantRuntime();
    if (page.started) return say('The disks are in use right now. Hang up first if you want to swap them.');
    $('adv-zip').value = '';
    $('adv-zip').click();
  }

  const diskButtons = () => [{ label: 'Folder', fn: chooseFolder }, { label: '.zip', fn: chooseZip }];

  function disksClicked() {
    const o = objects().disks || {};
    if (!adv.ready) return say('The box is still being unpacked. One moment.', { near: sparkX('disks') });
    wantRuntime();
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
    wantRuntime();
    if (page.started) return say(o.busy || 'The line is already in use. By you.', { near: sparkX('phone') });
    if (!adv.ready) return say('The telephone is still warming up. Give it a moment.');
    if (!haveDisks()) return say(o.refuse || REFUSAL, { near: sparkX('phone') });
    if (providers.length) return phoneFlow();
    dial();
  }

  // The checks before any dial. Says why and returns false when the line cannot be placed.
  function canDial() {
    const o = objects().phone || {};
    const no = (text, opts) => { say(text, opts); return false; };
    if (page.started) return false;
    if (!adv.ready) return no('The telephone is still warming up. Give it a moment.');
    if (!haveDisks()) return no(o.refuse || REFUSAL, { near: sparkX('phone') });
    const bad = serverProblem($('com1').value.trim() || defaultServer());
    if (bad) return no(bad, { bad: true, actions: [{ label: 'Server address', fn: openServerDialog }] });
    return true;
  }

  // opts.session is a sign-in token for the WebSocket, opts.card the provider id for the modem line.
  function dial(opts = {}) {
    // A server that requires sign-in refuses a line without a session, so ask for a card instead.
    if (signRequired && !opts.session) return askWho({ own: true });
    if (!canDial()) return;
    const o = objects().phone || {};
    ui.who.textContent = o.answer ? `"${o.answer}"` : '';
    award('dial', POINTS.dial);
    page.cardId = opts.card || '';
    start(opts.session);
  }

  function hangUp() {
    if (!page.started) return say('You are not on the line. Hanging up on nobody is rude, but nobody minds.');
    store.set('inn.redial', '');
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

  // ---------- sign-in: the operator asks who is calling ----------
  // The server side is "Sign-in (server contract)" in docs/web.md; auth.js makes the calls. The flow:
  // cards -> popup (on hold) -> account -> credential written into the install -> dial with the token.

  const sign = { busy: false, cancel: null, last: null, own: false, pick: false, provider: '' };
  const signedIn = () => !!InnAuth.token();
  const providerName = (id) => (providers.find((p) => p.id === id) || {}).name || id;
  const who = () => {
    const i = InnAuth.state.identity;
    return i ? `${i.handle} (${providerName(i.provider)})` : '';
  };
  const SIGN_QUIP = 'The operator recognises your voice. Mostly.';

  function cardChoices(own) {
    const list = providers.map((p) => ({ id: p.id, label: p.name, fn: () => signIn(p) }));
    if (own && !signRequired) list.push({ label: 'I have my own number', own: true, fn: ownNumber });
    return list;
  }

  function askWho(o = {}) {
    sign.own = !!o.own;
    say(o.text || 'Operator. How can I connect you?', {
      cards: cardChoices(sign.own), bad: o.bad, focus: true, near: sparkX('phone'),
      actions: sign.own ? [] : [{ label: 'Never mind', fn: narrHide }],
    });
  }

  // The classic path: whatever account the install already holds, no sign-in.
  function ownNumber() {
    InnAuth.setLine('own');
    dial();
  }

  function phoneFlow() {
    if (sign.busy) return;
    if (!signedIn()) return askWho({ own: true });
    withSession((s) => chooseLine(s, ''));
  }

  async function withSession(next) {
    try {
      const s = await InnAuth.fetchSession();
      sign.last = s;
      refreshInv();
      await syncAttest(s);
      return next(s);
    } catch (e) { return signFail(e); }
  }

  // The server keeps the player's answer to the age and terms notice against the hashed identity. When it
  // holds none, or one for older terms, make sure the notice for the current terms has been answered
  // (asking again if not) and send the answer. Declining ends in the closing screen and never returns.
  async function syncAttest(s) {
    const a = s.attested;
    if (a && a.age >= Site.cfg.minimumAge && a.terms >= Site.cfg.termsVersion) return;
    await Site.ensure();
    try { await InnAuth.attestNow(Site.attest()); } catch { /* the next login carries it too */ }
  }

  // Runs an account call. When the server says it has no current age and terms answer for this sign-in (403
  // attest_required), asks the notice again, sends the answer and tries once more.
  async function attested(call) {
    try { return await call(); } catch (e) {
      if (!(e instanceof InnAuth.AuthError) || e.kind !== 'attest') throw e;
      await Site.ask();
      await InnAuth.attestNow(Site.attest());
      return call();
    }
  }

  function signFail(e) {
    sign.busy = false;
    const kind = e && e.kind;
    if (kind === 'expired') {
      InnAuth.forget();
      refreshInv();
      return askWho({ own: true, bad: true, text: 'Your sign-in has expired (the server answered 401). The operator would like to see some identification.' });
    }
    const tail = { network: ' The operator\'s line is mostly static.', closed: ' The operator suggests a larger switchboard.', server: ' The operator is looking into it.', limit: ' The operator is firm about this.', attest: ' The doorman wants it in writing.', refused: '' }[kind] || '';
    const busy = kind === 'server' && e.code === 'busy';
    const head = {
      network: `Could not reach the sign-in server at ${InnAuth.state.base}. Check that it is running and that the ?auth= address is right.`,
      closed: 'The server is not taking new accounts: enrolment is closed. Existing lines still work.',
      server: busy ? 'The switchboard is busy (the server answered 503). Try again in a moment.' : `The sign-in server had a problem (it answered ${e.status}). Try again in a moment.`,
      limit: 'This card has signed up as many new names as it may in a day (the server answered 429). Play as a name you already have, or try again tomorrow.',
      attest: 'The server has no current answer to the age and terms notice for this sign-in (it answered 403), and it would not take one. You are not signed in for new lines.',
      refused: e.code === 'no_such_account' ? 'That line is not on this card (the server answered 404).' : `${e.message}`,
    }[kind] || `Sign-in failed: ${e && e.message ? e.message : e}.`;
    say(`${head}${/[.!?]$/.test(head) ? '' : '.'}${tail}`, { bad: true, actions: signedIn() ? [{ label: 'OK', fn: narrHide }] : [{ label: 'Pick a card', fn: () => askWho({ own: sign.own }) }, { label: 'OK', fn: narrHide }] });
  }

  // Called straight from the click, so the browser lets the popup open.
  async function signIn(p) {
    if (sign.busy) return;
    const pending = InnAuth.popupLogin(p.href);
    if (!pending) {
      say('The pop-up was blocked, so the operator will transfer you in this window. Your disks come back with you if "Remember on this device" is on.', { sticky: true });
      setTimeout(() => InnAuth.redirectLogin(p.href), 900);
      return;
    }
    sign.busy = true;
    sign.cancel = pending.cancel;
    sign.provider = p.id;
    say('The operator puts you on hold. A sign-in window has opened; finish there.', {
      hold: true, actions: [{ label: 'Hang up on the operator', fn: () => pending.cancel() }], focus: true });
    const r = await pending;
    sign.busy = false;
    sign.cancel = null;
    if (r.session) return signedInNow(SIGN_QUIP);
    if (r.error) return signDeclined(r.error, p.name);
    say('The sign-in window was closed before you signed in. You are not signed in, and nothing was changed. The operator is still holding, out of habit.', {
      bad: true, actions: [{ label: 'Pick a card', fn: () => askWho({ own: sign.own }) }, { label: 'OK', fn: narrHide }] });
  }

  // The codes the server sends with a failed login (docs/web.md "Login handshake"; innkeeperd.md 8.6).
  function signDeclined(code, name) {
    const at = (name || 'the provider').replace(/\.$/, '');
    const what = {
      access_denied: `Sign-in was declined at ${at}`,
      provider_error: `${at} could not sign you in (it reported an error)`,
      server_error: 'The game server could not finish the sign-in (it reported an error)',
    }[code] || `Sign-in did not finish (${code})`;
    say(`${what}. You are not signed in. The operator hangs up with great dignity.`, {
      bad: true, actions: [{ label: 'Pick a card', fn: () => askWho({ own: sign.own }) }, { label: 'OK', fn: narrHide }] });
  }

  // A fresh token: keep it, show the card in the inventory, then pick the line.
  function signedInNow(lead) {
    InnAuth.saveToken($('remember').checked);
    award('signin', POINTS.signin);
    refreshInv();
    return withSession((s) => chooseLine(s, lead));
  }

  function chooseLine(s, lead) {
    const name = s.identity.handle;
    const intro = lead ? `${lead} ` : '';
    if (!haveDisks()) {
      return say(`${intro}Signed in as ${name}. Now the disks, then the telephone.`, { near: sparkX('disks') });
    }
    if (page.started) return;
    if (!s.accounts.length) {
      if (!s.canCreate) return signFail(new InnAuth.AuthError('closed', 'No lines on this card.'));
      return openNewLine(`${intro}Welcome, ${name}!`, true);
    }
    if (s.accounts.length === 1 && !sign.pick) {
      const a = s.accounts[0];
      const who = a.persona ? `Ah, it's ${a.persona}.` : `Line ${a.id}, still unnamed: the client will ask who you are.`;
      say(`${intro}Hello, ${name}. ${who} Putting you through.`, { sticky: true });
      return setTimeout(() => connectAs(a), lead ? (reduced() ? 1600 : 2600) : 0);
    }
    sign.pick = false;
    const actions = s.accounts.map((a) => ({ label: a.persona ? `${a.persona} (${a.id})` : `New caller (${a.id})`, fn: () => connectAs(a) }));
    if (s.canCreate) actions.push({ label: 'Someone new, please', fn: () => openNewLine('', false) });
    if (!signRequired) actions.push({ label: 'My own number', fn: ownNumber });
    say(`${intro}Hello, ${name}. And who's playing today?`, { actions, focus: true, near: sparkX('phone') });
  }

  // A new line is only an account number and a password. Its persona (name, face, hobbies) is made by the
  // client's own Select Player screen after the dial, as in the original; the server takes the name it presents.
  async function openNewLine(lead, first) {
    say(lead ? `${lead} One moment, the operator is finding you a number…` : 'One moment, the operator is finding you a number…', { sticky: true });
    try {
      const a = await attested(() => InnAuth.createAccount());
      if (!a || !Number.isInteger(a.id)) throw new InnAuth.AuthError('server', 'The new line was not understood.', 201);
      a.persona = a.persona || null;
      if (sign.last) sign.last.accounts.push(a);
      const welcome = first
        ? `${lead} Your number is ${a.id}. The client will ask who you are, so dress nicely.`
        : `A new line: number ${a.id}. The client will ask who's calling, so have a name ready.`;
      say(welcome, { sticky: true });
      return setTimeout(() => connectAs(a), reduced() ? 1600 : 2600);
    } catch (e) {
      if (e.kind === 'limit' && sign.last && sign.last.accounts.length) {
        sign.last.canCreate = false;
        sign.pick = true;
        return chooseLine(sign.last, 'No new lines today: this card has reached its limit (the server answered 429).');
      }
      return signFail(e);
    }
  }

  async function connectAs(a) {
    if (!canDial()) return;
    say(`Putting ${a.persona || `line ${a.id}`} through…`, { sticky: true });
    try {
      const c = await attested(() => InnAuth.credential(a.id));
      if (!c || c.account !== a.id || typeof c.password !== 'string') throw new InnAuth.AuthError('server', 'The credential answer was not understood.', 200);
      await wantRuntime();
      await writeCredentials(a.id, c.password);
    } catch (e) {
      if (e instanceof InnAuth.AuthError && e.code === 'no_such_account') {
        sign.pick = true;
        return withSession((s) => chooseLine(s, 'That line is not on this card (the server answered 404).'));
      }
      if (e instanceof InnAuth.AuthError) return signFail(e);
      return say(`Could not prepare the line: ${e.message}`, { bad: true, actions: [{ label: 'OK', fn: narrHide }] });
    }
    InnAuth.setLine(a.id);
    const id = InnAuth.state.identity;
    dial({ session: InnAuth.token(), card: id ? id.provider : sign.provider });
  }

  async function signOut() {
    const name = who();
    narrHide();
    await InnAuth.logout();
    refreshInv();
    say(`Signed out${name ? ` (was ${name})` : ''}. The calling card goes back in the drawer. Goodbye, and mind the cables.`, { actions: [{ label: 'OK', fn: narrHide }] });
  }

  // Choose a different line (or make one) while signed in.
  function menuLine() {
    if (!haveDisks()) return say('Disks first, please. The operator cannot connect an empty box.', { near: sparkX('disks') });
    sign.pick = true;
    withSession((s) => chooseLine(s, ''));
  }

  // ---- coming back: a page load can carry a redirect login, a saved token or a redial ----

  function go() {
    if (adv.went || !adv.ready || !adv.provLoaded) return;
    adv.went = true;
    const err = InnAuth.takeFragmentError();
    const pend = InnAuth.takePending();
    if (!providers.length) { if (store.get('inn.redial') === '1' && haveDisks()) { store.set('inn.redial', ''); dial(); } return; }
    if (err) { signDeclined(err, ''); return; }
    const redial = store.get('inn.redial') === '1' && haveDisks();
    if (redial) store.set('inn.redial', '');
    if (pend) { sign.own = true; signedInNow(SIGN_QUIP); return; }
    if (!signedIn()) { if (redial) dial(); return; }
    // A token from earlier in the session (or remembered): learn who it is, quietly.
    InnAuth.fetchSession().then(async (s) => {
      sign.last = s;
      refreshInv();
      await syncAttest(s);
      if (!redial) return;
      const a = s.accounts.find((x) => x.id === Number(InnAuth.line()));
      if (a) return connectAs(a);
      if (InnAuth.line() === 'own') return dial();
      return chooseLine(s, '');
    }, (e) => {
      if (e.kind === 'expired') { InnAuth.forget(); refreshInv(); }
      if (redial) dial();
    });
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

  function toggleMusic() { InnMusic.toggle(); }

  // Options > Time of day: follow the system (auto), or always day or dusk. The <head> script applies a
  // stored choice before the first paint; scene.js repaints when data-theme changes.
  const THEME_KEY = 'inn.theme';
  const theme = () => document.documentElement.dataset.theme || 'auto';
  function setTheme(t) {
    if (t === 'light' || t === 'dark') document.documentElement.dataset.theme = t;
    else delete document.documentElement.dataset.theme;
    store.set(THEME_KEY, t, 'local');
  }

  function toggleAnim() {
    adv.anim = !adv.anim;
    store.set(ANIM_KEY, adv.anim ? '1' : '0', 'local');
    InnScenes.setAnimation(adv.anim);
    document.body.classList.toggle('still', !adv.anim);
  }

  // ---------- the menu bar ----------

  let providers = [];
  // The server takes only signed-in lines (public mode): no "own number", no unsigned dial.
  let signRequired = false;

  function menus() {
    const game = [
      { label: 'Load disks…', fn: chooseFolder },
      { label: 'Load disk image (.zip)…', fn: chooseZip },
      { sep: true },
      { label: 'Dial', fn: phoneClicked, off: () => page.started },
      { label: 'Hang up', fn: hangUp, off: () => !page.started },
    ];
    if (providers.length) {
      game.push({ sep: true });
      if (signedIn()) {
        game.push({ label: 'Who\'s playing…', fn: menuLine, off: () => page.started });
        game.push({ label: 'Sign out', fn: signOut, off: () => page.started });
      } else game.push({ label: 'Sign in…', fn: () => (wantRuntime(), sign.busy ? say('The operator already has you on hold.') : askWho({ own: false })), off: () => page.started });
    }
    return [
      { name: 'Game', items: game },
      { name: 'Options', items: [
        { label: 'Server address…', fn: openServerDialog },
        { label: 'Remember on this device', check: () => $('remember').checked, fn: toggleRemember, off: () => !page.persistent },
        { label: 'Animation', check: () => adv.anim, fn: toggleAnim },
        { label: `Music: ${InnMusic.enabled() ? 'on' : 'off'}`, fn: toggleMusic },
        { label: 'Send anonymous diagnostics', check: () => Telemetry.preference() && !Telemetry.blockedBy(), fn: () => { Telemetry.setPreference(!Telemetry.preference()); showDiagnostics(); },
          off: () => !!Telemetry.blockedBy() || !Telemetry.available() },
        { sep: true },
        { label: 'Time of day: follow system', radio: () => theme() === 'auto', fn: () => setTheme('auto') },
        { label: 'Time of day: day', radio: () => theme() === 'light', fn: () => setTheme('light') },
        { label: 'Time of day: dusk', radio: () => theme() === 'dark', fn: () => setTheme('dark') },
      ] },
      { name: 'Scene', items: InnScenes.ids.map((id) => ({
        label: id.charAt(0).toUpperCase() + id.slice(1), radio: () => InnScenes.id() === id, fn: () => InnScenes.setScene(id, true) })) },
      { name: 'Help', items: [
        { label: 'How do I play?', fn: () => say(
          'Click the glinting things: first the disks, then the telephone. The rest is scenery with opinions. Drop a folder or a .zip anywhere on the picture to hand over the disks. The menu bar lives at the top edge (F10 on the keyboard) and can do everything the picture can.',
          { sticky: true, actions: [{ label: 'OK', fn: narrHide }] }) },
        { label: 'About', fn: about },
        { sep: true },
        { label: 'Terms of Use', href: Site.LINKS[0].href },
        { label: 'Privacy', href: Site.LINKS[1].href },
        { label: 'Copyright & takedown', href: Site.LINKS[2].href },
      ] },
    ];
  }

  // Help, About: what this is, who runs it, the terms version, and the fine print.
  function about() {
    const c = Site.cfg;
    const by = c.operatorName ? ` Operated by ${c.operatorName}.` : '';
    const contact = c.contactEmail ? ` Contact: ${c.contactEmail}.` : '';
    say(`Online world, an unofficial browser port of an old online-game client. It runs your own copy of the original files, which stay on this device. Free software, GPL-3.0-or-later.${by}${contact} Terms version ${c.termsVersion}.${signedIn() ? ` You are signed in as ${who() || 'a calling card holder'}.` : ''}`,
      { sticky: true, actions: [{ label: 'OK', fn: narrHide }, ...Site.LINKS.map((l) => ({ label: l.short, href: l.href }))] });
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
      const li = item.href ? el('a', { role, tabIndex: -1, className: 'mi', href: item.href, target: '_blank', rel: 'noopener' })
        : el('div', { role, tabIndex: -1, className: 'mi' });
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
    if (item.fn) item.fn();
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
      const dlg = openDialog();
      if (dlg) return dialogKey(e, dlg);
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
        else if (!sign.busy) narrHide();
        return;
      }
      if (!inBar) return;
      if (document.activeElement === ui.music && (e.key === 'Enter' || e.key === ' ')) return;   // the button's own click
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
      if (e.key === 'Alt' && altAlone && landing() && !openDialog()) {
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

  const openDialog = () => [
    { root: ui.dlg, ctl: [ui.srv, ui.srvOk, ui.srvCancel], input: ui.srv, close: closeServerDialog, save: saveServerDialog },
  ].find((d) => !d.root.hidden);

  function dialogKey(e, d) {
    if (e.key === 'Escape') { e.preventDefault(); d.close(); return; }
    if (e.key === 'Enter' && e.target === d.input) { e.preventDefault(); d.save(); return; }
    if (e.key !== 'Tab') return;
    const ctl = d.ctl;
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
    // The game has its own sound, never both: the fade ends with the iris (close + open).
    InnMusic.leave(reduced() ? 300 : IRIS_CLOSE_MS + IRIS_OPEN_MS);
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
    refreshInv();
    InnScenes.setSpark('disks', adv.ready && !has && !page.started);
    InnScenes.setSpark('phone', adv.ready && has && !page.started);
  }

  function refreshInv() {
    if (!ui) return;
    ui.disk.hidden = !haveDisks();
    ui.card.hidden = !signedIn();
    ui.inv.hidden = ui.disk.hidden && ui.card.hidden;
    const name = who();
    ui.card.title = name ? `Calling card: ${name}` : 'You have a calling card';
    ui.card.setAttribute('aria-label', `Inventory: ${ui.card.title}`);
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
      go();
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

    providers({ list, required }) {
      providers = list;
      signRequired = !!required;
      adv.provLoaded = true;
      if (ui) refreshInv();
      go();
    },

    scenePainted: repaintCards,

    sceneChanged(sc) {
      if (sc && sc.id) InnMusic.setScene(sc.id);
      if (!ui) return;
      if (!sign.busy) narrHide();
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
    ui.music = el('button', { type: 'button', id: 'musicbtn', textContent: '\u266a', title: 'Music (M)' });
    ui.music.setAttribute('aria-label', 'Music');
    ui.music.setAttribute('aria-pressed', String(InnMusic.enabled()));
    ui.music.addEventListener('click', toggleMusic);
    ui.nowp = el('p', { id: 'nowp', className: 'adv', hidden: true, role: 'status' });
    bar.append(el('span', { className: 'spacer' }), ui.music, ui.score);
    ui.pop = el('div', { id: 'menupop', className: 'adv', role: 'menu', hidden: true });
    const item = (title, svg) => {
      const n = el('div', { className: 'item', title, role: 'img', hidden: true });
      n.setAttribute('aria-label', `Inventory: ${title}`);
      n.innerHTML = svg;
      return n;
    };
    ui.disk = item('You have the disks', '<svg viewBox="0 0 12 12" shape-rendering="crispEdges" aria-hidden="true"><path fill="#1c2260" d="M0 0h10l2 2v10H0z"/>'
      + '<path fill="#3c4cc0" d="M1 1h9l1 1v9H1z"/><path fill="#c8ccd8" d="M3 1h6v3H3z"/><path fill="#5c6074" d="M7 1.5h1v2H7z"/>'
      + '<path fill="#f6f0dc" d="M2 6h8v5H2z"/><path fill="#2c2c44" d="M3 7.5h5v1H3z"/></svg>');
    // The calling card: a cream card with a red band, a name line and a dark stripe.
    ui.card = item('You have a calling card', '<svg viewBox="0 0 12 12" shape-rendering="crispEdges" aria-hidden="true"><path fill="#3a2a1a" d="M0 2h12v8H0z"/>'
      + '<path fill="#f6f0dc" d="M1 3h10v6H1z"/><path fill="#c4401c" d="M1 3h10v2H1z"/><path fill="#2c2c44" d="M2 6h5v1H2z"/>'
      + '<path fill="#8f7e62" d="M2 8h3v.5H2z"/><path fill="#2c2c44" d="M8 6h2v2H8z"/></svg>');
    ui.inv = el('div', { id: 'inv', className: 'adv', hidden: true, role: 'group', ariaLabel: 'Inventory' }, ui.disk, ui.card);
    ui.cards = el('div', { className: 'cards', role: 'group', ariaLabel: 'Calling cards', hidden: true });
    ui.hold = el('div', { className: 'hold', ariaHidden: 'true', hidden: true });
    ui.hold.innerHTML = '<svg viewBox="0 0 8 10" shape-rendering="crispEdges"><path d="M3 0h4v2H5v6H3v1H1V8H0V6h1V5h2z"/></svg>'.repeat(3);
    ui.narrText = el('p', { className: 'text', ariaHidden: 'true' });
    ui.narrSr = el('p', { className: 'sr' });
    ui.acts = el('div', { className: 'acts' });
    ui.narr = el('div', { id: 'narr', className: 'adv', hidden: true, role: 'status' }, ui.narrText, ui.narrSr, ui.hold, ui.cards, ui.acts);
    ui.narr.addEventListener('click', (e) => { if (!e.target.closest('button, a')) narrHide(); });
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
    ui.dlg = el('div', { id: 'dlg', className: 'adv dlg', hidden: true },
      el('div', { role: 'dialog', ariaModal: 'true', className: 'box' },
        el('h2', { id: 'dlg-title', textContent: 'Server address' }),
        el('p', { className: 'hint', textContent: 'Where the game server listens, such as ws://host:2316. Leave it empty for the default.' }),
        ui.srv, ui.srvErr, el('div', { className: 'acts' }, ui.srvOk, ui.srvCancel)));
    ui.dlg.firstChild.setAttribute('aria-labelledby', 'dlg-title');
    ui.iris = el('div', { id: 'iris', hidden: true });
    document.body.append(files, bar, ui.pop, ui.inv, ui.narr, ui.nowp, ui.term, ui.dlg, ui.iris);
    ui.narr.addEventListener('keydown', (e) => {
      if (e.key !== 'ArrowRight' && e.key !== 'ArrowLeft') return;
      const list = [...ui.narr.querySelectorAll('button')];
      const i = list.indexOf(document.activeElement);
      if (i < 0) return;
      e.preventDefault();
      list[(i + (e.key === 'ArrowRight' ? 1 : list.length - 1)) % list.length].focus();
    });
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
    InnMusic.hooks.guard = () => !!openDialog();
    InnMusic.on((what, arg) => {
      if (what === 'state') {
        ui.music.setAttribute('aria-pressed', String(arg));
        say(arg ? 'Music on. A band tunes up somewhere out of sight.' : 'Music off. The band packs up, quietly.');
      } else if (what === 'track') {
        clearTimeout(adv.nowTimer);
        ui.nowp.textContent = `Now playing: ${arg.title}`;
        ui.nowp.hidden = false;
        adv.nowTimer = setTimeout(() => { ui.nowp.hidden = true; }, NOW_MS);
      } else if (what === 'gone') {
        clearTimeout(adv.nowTimer);
        ui.nowp.hidden = true;
      }
    });
    adv.anim = store.get(ANIM_KEY, 'local') !== '0';
    if (!adv.anim) { InnScenes.setAnimation(false); document.body.classList.add('still'); }
    window.addEventListener('resize', () => { if (ui.term.dataset.open === '1') placeTerm(); });
    setInterval(() => {
      if (!landing() || page.started || sign.busy || adv.hints >= HINT_MAX || bar.open >= 0 || openDialog()) return;
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
