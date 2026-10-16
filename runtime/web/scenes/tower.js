// Scene "tower": a wizard's crooked tower in an enchanted wood, running a telephone switchboard.
// Original artwork, drawn in code.
'use strict';

InnScenes.register((() => {
  const LEAF = ['#1c4430', '#3c7c42', '#9cc85c'];
  const LEAF2 = ['#143828', '#2c6438', '#78b04c'];
  const BASE_Y = 152;
  const cx = (y) => 214 + 6 * Math.sin((BASE_Y - y) / 26);       // the tower leans, then thinks better of it
  const halfW = (y) => 13 + (y - 60) * 0.1;                      // wider at the foot

  function sky(g, P) {
    g.fillStyle = P.gradRaw(g, 0, 0, 0, 140, P.pick(
      [[0, '#7cc4c8'], [0.4, '#b4e0b8'], [0.8, '#f4eeb0'], [1, '#f8e8a0']],
      [[0, '#120c3c'], [0.35, '#2c2070'], [0.75, '#7a3a90'], [1, '#d86a88']]));
    g.fillRect(0, 0, 320, 160);
    if (P.dusk) {
      P.stars(g, 0, 50, 70, 3);
      P.glow(g, 232, 30, 60, '#c8b8ff', 0.35);
      P.blob(g, 236, 30, 8, 8, ['#fffbe8', '#d8d0ac'], { wob: 0.3, seed: 4, n: 14 });
    } else {
      P.glow(g, 160, 6, 120, '#fffbd0', 0.6);
    }
  }

  function farWoods(g, P) {
    const layers = [
      { y: 96, a: 8, c: ['#8cc0b4', '#a8d0b0'], d: ['#5c5c9c', '#7c6ca8'], seed: 3 },
      { y: 108, a: 7, c: ['#6ca898', '#98c4a0'], d: ['#483c88', '#6a54a0'], seed: 5 },
      { y: 122, a: 7, c: ['#4c8c78', '#78ac84'], d: ['#2c2c68', '#4c3c88'], seed: 7 },
    ];
    for (const L of layers) {
      const cols = P.pick(L.c, L.d);
      const pts = P.ridge(g, { y: L.y, amp: L.a, freq: 0.045, seed: L.seed, bottom: 150, cols });
      // a fringe of pine spikes along the ridge
      for (let i = 2; i < pts.length - 1; i += 2) {
        const [x, y] = pts[i];
        P.shape(g, [[x - 2.2, y + 2], [x, y - 7 - (i % 3) * 2], [x + 2.2, y + 2]], P.mix(cols[0], cols[1], 0.5), { wob: 0.4, seed: i + L.seed });
      }
      g.fillStyle = P.gradRaw(g, 0, L.y, 0, 150, [[0, P.alpha(P.pick('#f4f4d0', '#8a5aa0'), 0)], [1, P.alpha(P.pick('#f4f4d0', '#8a5aa0'), 0.55)]]);
      g.fillRect(0, L.y - 4, 320, 150 - L.y + 4);
    }
  }

  function trunk(g, P, x, w, top, bottom, seed) {
    const pts = [[x - w / 2 - 3, bottom], [x - w / 2, bottom - 14], [x - w / 2 + 1, (top + bottom) / 2], [x - w * 0.4, top], [x + w * 0.4, top],
      [x + w / 2 - 1, (top + bottom) / 2], [x + w / 2, bottom - 14], [x + w / 2 + 4, bottom]];
    P.shape(g, pts, ['#7c5c48', '#4a3228', '#241410'], { wob: 1.3, seed, edge: '#120a08', ea: 0.45 });
    P.within(g, pts, { wob: 1.3, seed }, () => {
      for (let i = 0; i < 7; i++) {
        const lx = x - w / 2 + (i + 0.5) * (w / 7) + ((seed + i) % 3 - 1);
        P.line(g, [[lx, top], [lx + ((i % 2) - 0.5) * 2, (top + bottom) / 2], [lx - 1, bottom]], P.alpha('#120a08', 0.35), 1, { wob: 1.5, seed: seed + i });
      }
      P.dabs(g, x - w / 2, top, w * 0.5, bottom - top, ['#6c9c44', '#4c7c38'], Math.round((bottom - top) * 0.2), 1.7, seed + 9, 0.7);
    });
    // roots
    P.shape(g, [[x - w / 2 - 6, bottom + 3], [x - w / 2 - 1, bottom - 6], [x - w / 2 + 3, bottom - 2]], '#3c2820', { wob: 0.6, seed: seed + 2 });
    P.shape(g, [[x + w / 2 + 8, bottom + 3], [x + w / 2 + 2, bottom - 6], [x + w / 2 - 2, bottom - 2]], '#3c2820', { wob: 0.6, seed: seed + 3 });
  }

  function fern(g, P, x, y, s, seed) {
    g.lineCap = 'round';
    for (let i = -3; i <= 3; i++) {
      const a = -Math.PI / 2 + i * 0.42, len = (9 - Math.abs(i) * 1.2) * s;
      g.strokeStyle = P.col(i % 2 ? '#2c6438' : '#58a048');
      g.lineWidth = 1.4;
      g.beginPath(); g.moveTo(x, y);
      g.quadraticCurveTo(x + Math.cos(a) * len * 0.6, y + Math.sin(a) * len * 0.7 - 2, x + Math.cos(a) * len, y + Math.sin(a) * len + 2 + Math.abs(i) * 0.5);
      g.stroke();
    }
    void seed;
  }

  function mushroom(g, P, x, y, s, seed) {
    P.rect(g, x - 1 * s, y - 5 * s, 2 * s, 5 * s, '#f0e4cc', { wob: 0.2, seed });
    P.shape(g, [[x - 5 * s, y - 4.5 * s], [x - 3 * s, y - 8 * s], [x + 3 * s, y - 8 * s], [x + 5 * s, y - 4.5 * s]], ['#f05a4a', '#a8201c'], { wob: 0.3, seed: seed + 1 });
    g.fillStyle = P.col('#fff6e0');
    for (const [dx, dy] of [[-2, -6.5], [1.4, -7], [3, -5.4]]) g.fillRect(x + dx * s, y + dy * s, 1.4 * s, 1.2 * s);
    if (P.dusk) P.glow(g, x, y - 5 * s, 7 * s, '#ff7ac8', 0.18);
  }

  function tower(g, P) {
    P.shadow(g, 216, BASE_Y + 3, 34, 5, 0.35);
    // body, built as a stack of leaning slices
    const left = [], right = [];
    for (let y = BASE_Y; y >= 62; y -= 6) { left.push([cx(y) - halfW(y), y]); right.push([cx(y) + halfW(y), y]); }
    const body = left.concat(right.reverse());
    P.shape(g, body, ['#d4c8b4', '#a49ab0', '#6c6488'], { wob: 0.9, seed: 21, edge: '#2c2440', ea: 0.5 });
    P.within(g, body, { wob: 0.9, seed: 21 }, () => {
      g.fillStyle = P.alpha(P.pick('#2c2040', '#0c0620'), 0.3);
      g.beginPath(); g.moveTo(cx(62) + 3, 60); g.lineTo(cx(BASE_Y) + 20, 60); g.lineTo(cx(BASE_Y) + 20, 160); g.lineTo(cx(BASE_Y) + 6, 160); g.closePath(); g.fill();
      for (let y = 66, r = 0; y < BASE_Y; y += 5, r++) {
        g.fillStyle = P.alpha(P.pick('#3c2c4c', '#0c0620'), 0.22);
        g.fillRect(180, y, 70, 1);
        for (let x = 184 + (r % 2) * 5; x < 250; x += 10) g.fillRect(x, y, 1, 5);
      }
      P.dabs(g, 180, 62, 70, 90, ['#6c9c44', '#4c7c38'], 26, 2.2, 23, 0.55);
      P.dabs(g, 180, 62, 70, 90, ['#f0e8d8', '#9c90a8'], 30, 1.4, 24, 0.22);
    });
    // balcony
    const by = 108;
    P.shape(g, [[cx(by) - 25, by], [cx(by) + 27, by], [cx(by) + 25, by + 3.5], [cx(by) - 23, by + 3.5]], ['#8c6c4c', '#4a3220'], { wob: 0.4, seed: 25, edge: '#1c1008', ea: 0.5 });
    for (let x = cx(by) - 22; x <= cx(by) + 24; x += 5) P.rect(g, x, by - 6, 1.2, 6, '#5c4028', { wob: 0.1 });
    P.rect(g, cx(by) - 23, by - 6.5, 48, 1.4, '#6c4c30', { wob: 0.2, seed: 26 });
    // windows: a plain arched one, and the round switchboard window above the balcony
    const arch = (x, y, w, h, seed) => {
      P.shape(g, [[x - w / 2, y + h], [x - w / 2, y + 2], [x - w / 4, y], [x + w / 4, y], [x + w / 2, y + 2], [x + w / 2, y + h]], P.dusk ? P.lit('#ffd24a') : P.col('#96bcd4'), { wob: 0.3, seed, edge: '#2c2018', ea: 0.9 });
      g.fillStyle = P.col('#2c2018'); g.fillRect(x - 0.5, y, 1, h); g.fillRect(x - w / 2, y + h * 0.5, w, 1);
      if (P.dusk) P.glow(g, x, y + h / 2, 12, '#ffb030', 0.45);
    };
    arch(cx(130), 124, 8, 11, 31);
    arch(cx(80) - 14, 76, 7, 9, 32);
    arch(cx(80) + 14, 76, 7, 9, 33);
    // the door at the foot
    P.shape(g, [[cx(BASE_Y) - 7, BASE_Y], [cx(BASE_Y) - 7, BASE_Y - 11], [cx(BASE_Y) - 3, BASE_Y - 16], [cx(BASE_Y) + 3, BASE_Y - 16], [cx(BASE_Y) + 7, BASE_Y - 11], [cx(BASE_Y) + 7, BASE_Y]], ['#7c5430', '#3c2416'], { wob: 0.5, seed: 34, edge: '#1c1008', ea: 0.7 });
    P.rect(g, cx(BASE_Y) + 3, BASE_Y - 8, 1.6, 1.6, P.lit('#ffd24a'), { wob: 0.1 });
    if (P.dusk) P.glow(g, cx(BASE_Y), BASE_Y - 6, 18, '#ffb030', 0.35);
    // the crooked hat of a roof, floppy at the tip
    const rb = 63, hw = halfW(rb) + 6;
    P.shape(g, [[cx(rb) - hw - 4, rb + 3], [cx(rb) - hw, rb - 1], [cx(rb) + hw, rb - 1], [cx(rb) + hw + 4, rb + 3], [cx(rb), rb + 5]], ['#6a58b8', '#2c2468'], { wob: 0.6, seed: 41, edge: '#120c34', ea: 0.5 });
    const roof = [[cx(rb) - hw + 2, rb], [cx(rb) - hw + 6, rb - 12], [cx(rb) - 6, rb - 28], [cx(rb) - 2, rb - 42], [cx(rb) + 8, rb - 50],
      [cx(rb) + 20, rb - 50], [cx(rb) + 22, rb - 46], [cx(rb) + 12, rb - 42], [cx(rb) + 9, rb - 28], [cx(rb) + hw - 4, rb - 10], [cx(rb) + hw - 2, rb]];
    P.shape(g, roof, ['#8470d8', '#4838a0', '#241c64'], { wob: 0.8, seed: 42, edge: '#120c34', ea: 0.5 });
    P.within(g, roof, { wob: 0.8, seed: 42 }, () => {
      g.fillStyle = P.alpha('#120c34', 0.28);
      g.beginPath(); g.moveTo(cx(rb) + 3, rb - 50); g.lineTo(cx(rb) + 30, rb); g.lineTo(cx(rb) + 4, rb); g.closePath(); g.fill();
      // stars and a moon on the hat, as per the fashion
      for (const [sx, sy] of [[-6, -10], [4, -20], [-2, -34], [8, -6]]) {
        g.fillStyle = P.lit('#ffe66a');
        g.fillRect(cx(rb) + sx - 1, sy + rb, 3, 1); g.fillRect(cx(rb) + sx, sy + rb - 1, 1, 3);
      }
      P.dabs(g, cx(rb) - 18, rb - 40, 30, 38, ['#a898f0'], 12, 1.6, 43, 0.4);
    });
    P.blob(g, cx(rb) - 13, rb - 8, 3, 3, P.lit('#fff0a8'), { wob: 0.2, seed: 5, n: 8 });
    P.blob(g, cx(rb) - 12, rb - 8.6, 2.4, 2.4, P.pick('#5a48b0', '#33267c'), { wob: 0.2, seed: 6, n: 8 });
    // chimney with a pipe cleaner of smoke
    P.rect(g, cx(rb) + 18, rb - 14, 5, 12, '#8c7c70', { wob: 0.3, seed: 44 });
    // the cable bundle: from the balcony to the ground and away through the trees
    const cable = P.pick('#2c2420', '#0a0618');
    P.wire(g, [cx(by) + 26, by - 2], [330, 52], 12, cable, 2);
    P.wire(g, [cx(by) + 26, by + 1], [330, 58], 11, cable, 1.4);
    P.line(g, [[cx(by) - 20, by + 3], [cx(by) - 26, by + 18], [cx(by) - 18, by + 34], [cx(by) - 28, BASE_Y - 4], [cx(by) - 36, BASE_Y + 3]], cable, 2, { wob: 1.2, seed: 8 });
    P.line(g, [[cx(by) - 36, BASE_Y + 3], [cx(by) - 60, BASE_Y + 8], [cx(by) - 100, BASE_Y + 4]], cable, 2, { wob: 1.6, seed: 9 });
    // a coil of it by the door
    P.blob(g, cx(BASE_Y) - 20, BASE_Y + 2, 6, 2.2, cable, { wob: 0.5, seed: 10, n: 10 });
    P.blob(g, cx(BASE_Y) - 20, BASE_Y + 1, 4, 1.4, P.pick('#5c4a40', '#2c2050'), { wob: 0.3, seed: 11, n: 8 });
  }

  function canopyTop(g, P) {
    const r = P.mulberry(11);
    const cols = [LEAF, LEAF2];
    for (let i = 0; i < 30; i++) {
      const x = r() * 340 - 10;
      if (x > 168 && x < 262 && r() < 0.8) continue;      // keep the sky open round the hat
      const y = -4 + r() * 28 + (x > 140 ? 0 : 10);
      P.canopy(g, x, y, 13 + r() * 14, 9 + r() * 8, cols[i % 2].map((c) => P.col(c)), 500 + i);
    }
    // sunbeams in the day, a moonlit haze at night
    g.save();
    g.globalCompositeOperation = 'lighter';
    for (const [x, w, a] of [[100, 14, 0.12], [150, 20, 0.1], [262, 10, 0.1], [300, 14, 0.08]]) {
      g.fillStyle = P.gradRaw(g, 0, 20, 0, 160, [[0, P.alpha(P.pick('#fff8c0', '#8a7ad8'), a)], [1, P.alpha(P.pick('#fff8c0', '#8a7ad8'), 0)]]);
      g.beginPath(); g.moveTo(x, 20); g.lineTo(x + w, 20); g.lineTo(x + w - 46, 160); g.lineTo(x - 46 - 2, 160); g.closePath(); g.fill();
    }
    g.restore();
  }

  function ground(g, P) {
    P.ridge(g, { y: 150, amp: 2.5, freq: 0.04, seed: 8, bottom: 200, cols: ['#5c9a48', '#38723c', '#1e4a2c'] });
    P.dabs(g, 0, 150, 320, 50, ['#9cc85c', '#2c6438', '#c8d870', '#1c4430'], 110, 3, 15, 0.38);
    // the path to the door
    const path = [[207, 152], [222, 152], [226, 162], [236, 176], [266, 200], [150, 200], [186, 184], [204, 168]];
    P.shape(g, path, ['#e4d0a0', '#a88c64'], { wob: 1.5, seed: 14, edge: '#4c3820', ea: 0.3 });
    P.within(g, path, { wob: 1.5, seed: 14 }, () => {
      P.dabs(g, 150, 152, 120, 48, ['#8c7048', '#f6e8c0', '#6c5438'], 40, 2, 16, 0.4);
    });
    for (let i = 0; i < 4; i++) P.blob(g, 214 + i * 2 - 5, 160 + i * 9, 4 + i, 1.6 + i * 0.3, '#c8b488', { wob: 0.5, seed: 70 + i, n: 8, edge: '#4c3820', ea: 0.25 });
    // trunks: the big ones frame the picture
    trunk(g, P, 22, 26, -4, 176, 61);
    trunk(g, P, 78, 16, 0, 164, 62);
    trunk(g, P, 140, 12, 10, 158, 63);
    trunk(g, P, 300, 22, 0, 176, 64);
    for (const [x, y, s] of [[118, 172, 1.1], [176, 176, 1.0], [150, 184, 1.4], [260, 182, 1.2], [296, 188, 1.6], [48, 186, 1.5], [10, 195, 1.8], [108, 190, 1.3]]) fern(g, P, x, y, s, x);
    for (const [x, y, s] of [[132, 178, 1], [138, 180, 0.7], [284, 170, 1.1], [124, 190, 1.2], [36, 176, 1.2], [60, 192, 0.9]]) mushroom(g, P, x, y, s, x + 3);
  }

  function sign(g, P) {
    const x = 226, y = 153, w = 86, h = 32;
    // two posts, and the board hung between them
    P.rect(g, x + 8, y + 8, 3, 34, '#6c4c30', { wob: 0.3, seed: 81 });
    P.rect(g, x + w - 11, y + 8, 3, 34, '#6c4c30', { wob: 0.3, seed: 82 });
    P.shadow(g, x + w / 2, y + 41, 42, 3, 0.3);
    P.shape(g, [[x, y + 1], [x + w, y], [x + w - 1, y + h], [x + 1, y + h + 1]], ['#e4c68c', '#a8784c'], { wob: 0.8, seed: 83, edge: '#3c2410', ea: 0.8 });
    P.within(g, [[x, y + 1], [x + w, y], [x + w - 1, y + h], [x + 1, y + h + 1]], { wob: 0.8, seed: 83 }, () => {
      for (let i = 1; i < 4; i++) { g.fillStyle = P.alpha('#3c2410', 0.25); g.fillRect(x, y + i * 8, w, 0.8); }
      P.dabs(g, x, y, w, h, ['#c8a06c', '#8c5c38'], 20, 1.5, 84, 0.3);
    });
    const ink = P.dusk ? P.lit('#ffe890') : '#3c1c58';
    const line = (str, yy, col) => P.text(g, str, x + (w - P.textWidth(str, 1, 1)) / 2 + 1, yy, col, 1, 1);
    line('LONG-DISTANCE', y + 4, ink);
    line('SPELLS 3^/MIN', y + 14, P.dusk ? P.lit('#ff9a7a') : '#a82c28');
    line('CHARMS EXTRA', y + 24, P.dusk ? P.lit('#b8e8ff') : '#3c1c58');
  }

  // A lectern with a spellbook on it. The book is a disk box: a floppy is embossed on the cover.
  function lectern(g, P) {
    const x = 86, base = 184;
    P.shadow(g, x, base + 1, 22, 3.4, 0.4);
    P.rect(g, x - 3, 158, 6, base - 158, ['#9c6c3c', '#4a2c18'], { wob: 0.3, seed: 91, edge: '#1c1008', ea: 0.7 });
    P.shape(g, [[x - 10, base + 1], [x - 6, base - 5], [x + 6, base - 5], [x + 10, base + 1]], ['#8c5c30', '#3c2414'], { wob: 0.4, seed: 92, edge: '#1c1008', ea: 0.7 });
    P.shape(g, [[x - 21, 154], [x + 21, 154], [x + 16, 163], [x - 16, 163]], ['#b8844c', '#5c3a1c'], { wob: 0.4, seed: 93, edge: '#1c1008', ea: 0.8 });
    // the book: purple cover, gold border, page edges below, a clasp
    const bx = x - 17, by = 132, bw = 34, bh = 22;
    P.shape(g, [[bx - 1, by + bh], [bx + bw, by + bh], [bx + bw + 2, by + bh + 3], [bx + 1, by + bh + 3]], ['#fff6d8', '#c8b88c'], { wob: 0.2, seed: 94, edge: '#3a2c14', ea: 0.6 });
    P.rect(g, bx, by, bw, bh, ['#9a62e0', '#4a2a9c'], { wob: 0.4, seed: 95, edge: '#1c0c44', ea: 0.9 });
    P.rect(g, bx, by, 4, bh, ['#4a2a9c', '#2c1868'], { wob: 0.2, seed: 96 });
    g.strokeStyle = P.col('#f0c850'); g.lineWidth = 1;
    g.strokeRect(bx + 6.5, by + 2.5, bw - 9, bh - 5);
    P.floppy(g, bx + 13, by + 4.5, 13, '#f0c850', '#fff6d8');
    P.rect(g, bx + bw - 3, by + 8, 5, 5, ['#ffe27a', '#b8801c'], { wob: 0.1, seed: 97, edge: '#3a2410', ea: 0.8 });
    for (const [sx, sy] of [[bx + 7, by + 3], [bx + 24, by + 3], [bx + 7, by + bh - 4], [bx + 24, by + bh - 4]]) { g.fillStyle = P.lit('#ffe66a'); g.fillRect(sx, sy, 1, 1); }
    if (P.dusk) P.glow(g, bx + bw / 2, by + bh / 2, 22, '#c8a0ff', 0.25);
  }

  // A candlestick telephone on the balcony, by the switchboard window.
  function balconyPhone(g, P) {
    P.candlePhone(g, cx(108) + 19, 108.5, 1.9);
  }

  function paint(g, P) {
    sky(g, P);
    farWoods(g, P);
    ground(g, P);
    tower(g, P);
    sign(g, P);
    canopyTop(g, P);
    lectern(g, P);
    balconyPhone(g, P);
  }

  // ---------- animation ----------

  const anims = [
    {
      // the switchboard in the round window: rows of lights, and the operator plugging cords
      x: 200, y: 84, w: 40, h: 26, sample: 0,
      draw(g, t, P) {
        const x0 = cx(96) - 12, y0 = 88;
        P.shape(g, [[x0, y0 + 14], [x0, y0 + 3], [x0 + 3, y0], [x0 + 21, y0], [x0 + 24, y0 + 3], [x0 + 24, y0 + 14]], P.dusk ? P.lit('#ffcf6a') : P.col('#7a98b4'), { wob: 0.3, seed: 6, edge: '#2c2018', ea: 0.95 });
        if (P.dusk) P.glow(g, x0 + 12, y0 + 8, 22, '#ffb030', 0.5);
        // the panel of jacks
        P.rect(g, x0 + 3, y0 + 3, 18, 9, P.pick('#4c3c30', '#241408'), { wob: 0.1 });
        for (let r = 0; r < 3; r++) {
          for (let c = 0; c < 6; c++) {
            const on = Math.sin(t * (1.1 + c * 0.37) + r * 2 + c * 1.7) > 0.25;
            g.fillStyle = on ? P.lit(r === 1 ? '#7affb0' : '#ff7a6a') : P.col('#6a5040');
            g.fillRect(x0 + 4 + c * 3, y0 + 4.5 + r * 2.8, 1.6, 1.4);
          }
        }
        // the operator: a pointed hat sliding along, one arm out
        const ox = x0 + 6 + (Math.sin(t * 0.7) * 0.5 + 0.5) * 12;
        g.fillStyle = P.pick('#3c2c78', '#a8a0f0');
        g.beginPath(); g.moveTo(ox - 2.2, y0 + 12); g.lineTo(ox + 1, y0 + 3); g.lineTo(ox + 2.6, y0 + 12); g.closePath(); g.fill();
        g.fillStyle = P.pick('#f0c8a0', '#d8b090'); g.fillRect(ox - 1.4, y0 + 10, 3, 2);
        g.strokeStyle = P.pick('#2c1c10', '#1c1008'); g.lineWidth = 0.9;
        const cord = Math.floor(t * 1.5) % 5;
        g.beginPath(); g.moveTo(ox + 2, y0 + 10); g.lineTo(x0 + 4 + cord * 3, y0 + 6); g.stroke();
      },
    },
    {
      // the owl on the sign, until it takes off with the end of the cable
      x: 100, y: 20, w: 220, h: 180, sample: 0,
      draw(g, t, P) {
        const cyc = (t - 5) % 26;
        let ox = 292, oy = 150, fly = -1;
        if (t >= 5 && cyc < 11) {
          fly = cyc / 11;
          ox = 292 + fly * 44 - fly * fly * 8;
          oy = 150 - Math.sin(Math.min(1, fly * 1.4) * Math.PI * 0.5) * 100 + (fly > 0.5 ? (fly - 0.5) * 20 : 0);
        } else if (t >= 5 && cyc < 24) { return; }
        // the cable he is hauling, paying out from the balcony
        if (fly >= 0) {
          g.strokeStyle = P.col('#2c2420'); g.lineWidth = 1.6; g.lineCap = 'round';
          g.beginPath(); g.moveTo(cx(108) + 26, 106);
          g.quadraticCurveTo((cx(108) + 26 + ox) / 2, Math.max(106, oy) + 30 + (1 - fly) * 8, ox - 2, oy + 7);
          g.stroke();
        }
        const flap = fly >= 0 ? Math.sin(t * 22) : 0;
        owl(g, P, ox, oy, flap, fly >= 0);
      },
    },
    {
      // the frog on hold: foot tapping, music notes drifting up
      x: 118, y: 140, w: 46, h: 50, sample: 0,
      draw(g, t, P) {
        const x = 150, y = 184;
        const tap = Math.abs(Math.sin(t * 6)) * 2;
        P.shadow(g, x, y + 1, 12, 2.4, 0.3);
        // a log to sit on
        P.shape(g, [[x - 14, y + 1], [x - 15, y - 5], [x - 8, y - 8], [x + 10, y - 8], [x + 16, y - 5], [x + 15, y + 1]], ['#8c6c48', '#4c3424'], { wob: 0.5, seed: 91, edge: '#1c1008', ea: 0.5 });
        P.blob(g, x + 15, y - 3.4, 1.8, 4.2, '#c8a470', { wob: 0.2, seed: 92, n: 8 });
        // the frog
        P.line(g, [[x - 6, y - 9], [x - 9, y - 5 + tap * 0.2]], '#4c9c3c', 2.6);
        P.blob(g, x, y - 13, 7, 6, ['#9cdc64', '#3c8c3c'], { wob: 0.3, seed: 93, n: 12, edge: '#14340c', ea: 0.5 });
        P.blob(g, x + 5, y - 20, 5, 3.6, ['#a8e470', '#4c9c3c'], { wob: 0.3, seed: 94, n: 10, edge: '#14340c', ea: 0.5 });
        P.blob(g, x + 3, y - 22.4, 1.7, 1.7, '#ffffff', { wob: 0.1, seed: 95, n: 8 });
        P.blob(g, x + 7.6, y - 22.4, 1.7, 1.7, '#ffffff', { wob: 0.1, seed: 96, n: 8 });
        g.fillStyle = '#14100c'; g.fillRect(x + 3.4, y - 22, 1, 1.2); g.fillRect(x + 8, y - 22, 1, 1.2);
        P.line(g, [[x + 3, y - 18.5], [x + 9, y - 18]], '#14340c', 1);
        // a crown, tilted, in case it is relevant
        P.shape(g, [[x + 2, y - 25], [x + 2.4, y - 28.4], [x + 4, y - 26.6], [x + 5.4, y - 29], [x + 7, y - 26.6], [x + 8.6, y - 28.6], [x + 9, y - 25]], P.lit('#ffd23c'), { wob: 0.2, seed: 97 });
        // the telephone receiver at his ear
        P.line(g, [[x + 11, y - 22], [x + 12, y - 16]], '#2c2c3c', 2.4);
        P.line(g, [[x + 10, y - 22.6], [x + 13.4, y - 22.6]], '#2c2c3c', 2);
        P.line(g, [[x + 12, y - 15], [x + 14, y - 8], [x + 11, y - 2]], '#2c2c3c', 0.9, { wob: 0.4, seed: 7 });
        // front foot taps
        P.blob(g, x + 6, y - 7 - tap * 0.5, 3.4, 1.4, '#58a844', { wob: 0.2, seed: 98, n: 8 });
        // hold music
        for (let i = 0; i < 3; i++) {
          const k = ((t * 0.5 + i / 3) % 1);
          const nx = x + 16 + Math.sin(k * 6 + i) * 4, ny = y - 24 - k * 18;
          g.globalAlpha = Math.sin(k * Math.PI);
          g.fillStyle = P.pick('#f6e8ff', '#ffe0ff');
          g.fillRect(nx, ny, 2, 1.6); g.fillRect(nx + 1.4, ny - 4, 0.8, 4); g.fillRect(nx + 1.4, ny - 4, 2.4, 0.8);
          g.globalAlpha = 1;
        }
      },
    },
    {
      // fireflies by night, drifting pollen by day
      x: 120, y: 90, w: 200, h: 100, sample: 0,
      draw(g, t, P) {
        const r = P.mulberry(17);
        for (let i = 0; i < 22; i++) {
          const bx = 120 + r() * 200, by = 96 + r() * 90, sp = 0.3 + r() * 0.6, ph = r() * 6.28;
          const x = bx + Math.sin(t * sp + ph) * 7, y = by + Math.cos(t * sp * 1.3 + ph) * 5;
          const pulse = 0.5 + 0.5 * Math.sin(t * (1.5 + r()) + ph);
          if (P.dusk) {
            P.glow(g, x, y, 6, '#d8ff70', 0.7 * pulse);
            g.fillStyle = P.lit('#f4ffb0'); g.globalAlpha = 0.5 + pulse * 0.5;
            g.fillRect(x - 0.5, y - 0.5, 1.6, 1.6); g.globalAlpha = 1;
          } else {
            g.fillStyle = P.alpha('#fffbe0', 0.5 * pulse + 0.15);
            g.fillRect(x, y, 1.4, 1.4);
          }
        }
      },
    },
  ];

  function owl(g, P, x, y, flap, flying) {
    const brown = ['#d8b484', '#7c5a38'];
    if (flying) {
      const w = 7 + flap * 3;
      P.shape(g, [[x - 2, y - 4], [x - w - 6, y - 10 - flap * 5], [x - w - 3, y - 3], [x - 2, y + 1]], ['#b8946a', '#5c4028'], { wob: 0.3, seed: 1 });
      P.shape(g, [[x + 2, y - 4], [x + w + 6, y - 10 - flap * 5], [x + w + 3, y - 3], [x + 2, y + 1]], ['#b8946a', '#5c4028'], { wob: 0.3, seed: 2 });
    }
    P.blob(g, x, y - 1, 5.2, 6.6, brown, { wob: 0.3, seed: 3, n: 12, edge: '#2c1c10', ea: 0.5 });
    P.blob(g, x, y + 0.5, 3.2, 4, '#f0dcb4', { wob: 0.3, seed: 4, n: 10 });
    P.shape(g, [[x - 5, y - 6.5], [x - 3.6, y - 10.4], [x - 1.4, y - 7]], '#7c5a38', { wob: 0.1, seed: 5 });
    P.shape(g, [[x + 5, y - 6.5], [x + 3.6, y - 10.4], [x + 1.4, y - 7]], '#7c5a38', { wob: 0.1, seed: 6 });
    P.blob(g, x - 2.2, y - 4, 2, 2, '#fff6d0', { wob: 0.1, seed: 7, n: 8 });
    P.blob(g, x + 2.2, y - 4, 2, 2, '#fff6d0', { wob: 0.1, seed: 8, n: 8 });
    g.fillStyle = '#1c1008'; g.fillRect(x - 2.6, y - 4.4, 1.2, 1.4); g.fillRect(x + 1.6, y - 4.4, 1.2, 1.4);
    g.fillStyle = P.lit('#e8a030'); g.fillRect(x - 0.6, y - 2.6, 1.4, 1.6);
  }

  const hotspots = [
    { id: 'tower', x: 180, y: 12, w: 70, h: 142, say: [
      'A wizard\'s tower. It leans in the manner of someone listening at a door.',
      'The tower is crooked because the builder charged by the straight line.',
      'It is rather tall for a place with one resident and no stairs worth mentioning.'] },
    { id: 'switchboard', x: 200, y: 84, w: 36, h: 26, say: [
      '"Please hold," says a voice inside. "I will transfer you to a newt."',
      'Rows of little lights blink. Each one is somebody being put through to somebody else\'s problem.'] },
    { id: 'owl', x: 278, y: 134, w: 26, h: 22, say: [
      'An owl. It has been hauling cable for six hours and has asked for a raise in mice.',
      'The owl gives you a look. It is the look of a bird with a very long cable.'] },
    { id: 'sign', x: 224, y: 152, w: 90, h: 36, say: [
      'Long-distance spells, three cents a minute. Charms are extra and the extra is not small.',
      'A smaller notice adds: "No hexes on credit."'] },
    { id: 'frog', x: 134, y: 154, w: 36, h: 36, say: [
      'A frog on hold. He claims he is a prince and would like to speak to a manager.',
      'The hold music is a lute. It has been the same eight bars since spring.',
      'He gives you a hopeful look. You have not got the right kind of lips.'] },
    { id: 'mushrooms', x: 118, y: 166, w: 30, h: 28, say: [
      'Do not eat the mushrooms. They are on a call.'] },
    { id: 'cable', x: 36, y: 150, w: 100, h: 20, say: [
      'A very thick cable. It goes on into the forest with an air of purpose.'] },
    { id: 'tree', x: 282, y: 10, w: 40, h: 140, say: [
      'The tree is listening. Trees are notorious for it.',
      'It has a knot like a face. It would rather you did not mention it.'] },
  ];

  hotspots.push(
    { id: 'spellbook', role: 'disks', x: 66, y: 128, w: 40, h: 58, say: ['A spellbook on a lectern. It is mostly disk, with a little magic in the margins.'] },
    { id: 'candlephone', role: 'phone', x: 234, y: 84, w: 18, h: 26, say: ['A candlestick telephone. Wizards use them to avoid shouting spells across the forest.'] });

  const objects = {
    disks: {
      open: 'The spellbook creaks open. The pages are floppies, and each one has been carefully blessed.',
      got: 'The switchboard operator in the tower window is looking your way. Try the telephone on the balcony.',
      again: 'The book sighs. It was just getting comfortable.',
      hint: 'The big purple book on the lectern is twinkling. Books do that when they want to be read.',
    },
    phone: {
      refuse: 'You pick up the receiver. A voice from the tower says: "Disks first, please. Spells are not free, and neither is the book."',
      answer: 'Switchboard. Which realm?',
      hint: 'The telephone on the balcony is sparkling. Either it is magic or it needs dusting.',
      busy: 'The owl on the switchboard holds up a wing. The line is occupied.',
    },
  };

  return { id: 'tower', title: 'The tower', focus: { x: 220, y: 100 }, paint, anims, hotspots, objects,
    termAt: [104, 70],
    spark: { disks: [62, 126, 48, 62], phone: [230, 80, 26, 32] } };
})());
