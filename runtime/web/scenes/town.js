// Scene "town": the main square of a small town on a sunny morning (or a lamp-lit evening): a clock
// tower, a post office, a shop with a confident sign, a fountain and a red telephone booth.
// Original artwork, drawn in code. Registers itself with InnScenes (scene.js).
'use strict';

InnScenes.register((() => {
  const GY = 152;                       // where the buildings meet the paving
  const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);

  function sky(g, P) {
    g.fillStyle = P.gradRaw(g, 0, 0, 0, 120, P.pick(
      [[0, '#58a4e8'], [0.45, '#9ccff0'], [0.8, '#f4eccc'], [1, '#fcd8a8']],
      [[0, '#141040'], [0.4, '#3c2c78'], [0.78, '#b8527c'], [1, '#f49a5c']]));
    g.fillRect(0, 0, 320, 120);
    if (P.dusk) {
      P.grain(g, 0, 0, 320, 60, '#ffffff', 80, 14, 0.9);
      P.glow(g, 60, 34, 36, '#c8c0ff', 0.28);
      P.blob(g, 60, 34, 8, 8, ['#fffbe8', '#d4cca8'], { wob: 0.4, seed: 2, n: 16 });
    } else {
      P.glow(g, 250, 40, 80, '#fff6c8', 0.5);
      P.glow(g, 250, 40, 28, '#ffffff', 0.45);
    }
  }

  function hills(g, P) {
    P.ridge(g, { y: 112, amp: 8, freq: 0.03, seed: 5, bottom: 150, cols: ['#a8b8e0', '#c8d0e0'] });
    P.ridge(g, { y: 122, amp: 5, freq: 0.05, seed: 11, bottom: 150, cols: ['#8caa8c', '#a8c098'] });
    // a far rooftop skyline
    const r = P.mulberry(7);
    for (let x = -6; x < 330; x += 11 + r() * 9) {
      const w = 9 + r() * 12, h = 10 + r() * 16;
      const wall = r() < 0.5 ? ['#e0c8b0', '#b8a090'] : ['#d8d0c0', '#a8a0a0'];
      P.shape(g, [[x, GY], [x, GY - h], [x + w, GY - h], [x + w, GY]], wall, { wob: 0.5, seed: Math.floor(x) + 3 });
      P.shape(g, [[x - 1, GY - h], [x + w / 2, GY - h - 5 - r() * 3], [x + w + 1, GY - h]], r() < 0.5 ? ['#b8503a', '#80281c'] : ['#4c6c9c', '#2c446c'], { wob: 0.5, seed: Math.floor(x) + 4 });
    }
  }

  function windowAt(g, P, x, y, w, h) {
    if (P.dusk) {
      g.fillStyle = P.lit('#ffd24a');
      g.fillRect(x, y, w, h);
      P.glow(g, x + w / 2, y + h / 2, w + 5, '#ffb830', 0.45);
    } else {
      g.fillStyle = P.col('#9ec8dc');
      g.fillRect(x, y, w, h);
      g.fillStyle = P.alpha('#ffffff', 0.4);
      g.fillRect(x, y, w, 1);
    }
    g.fillStyle = P.col('#3a2418');
    g.fillRect(x + Math.floor(w / 2), y, 1, h);
    g.fillRect(x, y + Math.floor(h / 2), w, 1);
  }

  function brick(g, P, pts, x, y, w, h, base, seed) {
    P.shape(g, pts, base, { wob: 0.5, seed, edge: '#3a1c14', ea: 0.45 });
    P.within(g, pts, { wob: 0.5, seed }, () => {
      g.fillStyle = P.alpha(P.col('#2c1010'), 0.22);
      for (let yy = y + 3, r = 0; yy < y + h; yy += 4, r++) {
        g.fillRect(x, yy, w, 1);
        for (let xx = x + (r % 2) * 4; xx < x + w; xx += 8) g.fillRect(xx, yy - 3, 1, 3);
      }
      g.fillStyle = P.alpha(P.col('#2c1010'), 0.3);
      g.fillRect(x + w - 8, y, 8, h);
      P.dabs(g, x, y, w, h, ['#e8a080', '#702818'], 20, 1.6, seed + 1, 0.22);
    });
  }

  // ---- the clock tower in the middle ----
  function clockTower(g, P) {
    const x0 = 140, x1 = 172, top = 62;
    P.shadow(g, 160, GY + 2, 30, 4, 0.3);
    const body = [[x0, GY], [x0 + 1, top], [x1 - 1, top], [x1, GY]];
    P.shape(g, body, ['#f2e2bc', '#c8b088', '#9c8868'], { wob: 0.6, seed: 31, edge: '#4a3420', ea: 0.45 });
    P.within(g, body, { wob: 0.6, seed: 31 }, () => {
      g.fillStyle = P.alpha(P.col('#3a2418'), 0.28);
      g.fillRect(x1 - 9, top, 9, GY - top);
      g.fillStyle = P.alpha(P.col('#3a2418'), 0.14);
      for (let y = top + 4; y < GY; y += 5) g.fillRect(x0, y, 32, 1);
      P.dabs(g, x0, top, 32, GY - top, ['#fff4d8', '#7c6448'], 18, 1.4, 32, 0.22);
    });
    // belfry with arches under the roof
    P.rect(g, x0 - 2, top - 14, 36, 14, ['#e8d4a8', '#a88c64'], { wob: 0.4, seed: 33, edge: '#4a3420', ea: 0.5 });
    for (const ax of [x0 + 5, x0 + 15, x0 + 25]) {
      P.shape(g, [[ax, top - 1], [ax, top - 9], [ax + 2, top - 11], [ax + 4, top - 9], [ax + 4, top - 1]], P.pick('#3a2c3c', '#1c1230'), { wob: 0.2, seed: ax });
    }
    P.shape(g, [[x0 - 5, top - 13], [156, top - 38], [x1 + 5, top - 13]], ['#c4503c', '#7c2418'], { wob: 0.6, seed: 34, edge: '#2c1008', ea: 0.45 });
    P.within(g, [[x0 - 5, top - 13], [156, top - 38], [x1 + 5, top - 13]], { wob: 0.6, seed: 34 }, () => {
      g.fillStyle = P.alpha('#1c0c08', 0.3);
      g.beginPath(); g.moveTo(156, top - 38); g.lineTo(x1 + 5, top - 13); g.lineTo(157, top - 13); g.closePath(); g.fill();
    });
    P.line(g, [[156, top - 38], [156, top - 47]], '#2c1c18', 1);
    P.shape(g, [[151, top - 46], [161, top - 46], [159, top - 43], [153, top - 43]], P.pick('#e8b830', '#c8a850'), { wob: 0.2, seed: 1 });
    // the clock face is drawn live (anims); this is the dial and the door
    P.blob(g, 156, 80, 11.5, 11.5, ['#8c6c3c', '#4a3018'], { wob: 0.3, seed: 35, n: 14, edge: '#1c1008', ea: 0.8 });
    P.blob(g, 156, 80, 9.6, 9.6, P.lit(P.pick('#fff6d8', '#ffe8a0')), { wob: 0.2, seed: 36, n: 14, edge: '#3a2418', ea: 0.7 });
    if (P.dusk) P.glow(g, 156, 80, 22, '#ffe8a0', 0.4);
    g.fillStyle = P.col('#2c1c18');
    for (let i = 0; i < 12; i++) { const a = (i / 12) * Math.PI * 2; g.fillRect(Math.round(156 + Math.sin(a) * 8) - 0.5, Math.round(80 - Math.cos(a) * 8) - 0.5, 1, 1); }
    P.shape(g, [[150, GY], [150, GY - 12], [153, GY - 16], [159, GY - 16], [162, GY - 12], [162, GY]], ['#7c5430', '#3c2416'], { wob: 0.4, seed: 37, edge: '#1c1008', ea: 0.7 });
    P.rect(g, 159, GY - 8, 1.6, 1.6, P.lit('#ffd24a'), { wob: 0.1 });
    windowAt(g, P, 144, 108, 4, 7);
    windowAt(g, P, 164, 108, 4, 7);
    windowAt(g, P, 144, 128, 4, 7);
    windowAt(g, P, 164, 128, 4, 7);
  }

  // ---- the post office on the left ----
  function postOffice(g, P) {
    const x = 6, w = 80, top = 100;
    P.shadow(g, x + w / 2, GY + 2, 44, 4, 0.3);
    const wall = [[x, GY], [x, top], [x + w, top], [x + w, GY]];
    brick(g, P, wall, x, top, w, GY - top, ['#d86a48', '#a03c28', '#7c2a1c'], 41);
    P.shape(g, [[x - 3, top + 1], [x - 3, top - 4], [x + w + 3, top - 4], [x + w + 3, top + 1]], ['#e8d8b0', '#a8946c'], { wob: 0.4, seed: 42, edge: '#3a2c1c', ea: 0.6 });
    // sign across the front
    P.rect(g, x + 4, top + 3, 70, 11, ['#2c4c7c', '#18284c'], { wob: 0.3, seed: 43, edge: '#0c1430', ea: 0.8 });
    P.text(g, 'POST OFFICE', x + 6.5, top + 5, '#ffe27a', 1, 1);
    // the shop window with the disks in it
    const wx = 14, wy = 120, ww = 44, wh = 28;
    P.rect(g, wx - 2, wy - 2, ww + 4, wh + 4, ['#e8d8b0', '#a8946c'], { wob: 0.3, seed: 44, edge: '#2c2010', ea: 0.7 });
    g.fillStyle = P.pick('#fff0c0', '#ffd870'); g.fillRect(wx, wy, ww, wh);
    if (P.dusk) P.glow(g, wx + ww / 2, wy + wh / 2, 34, '#ffb830', 0.5);
    g.fillStyle = P.alpha(P.col('#5a3a20'), 0.35); g.fillRect(wx, wy + 22, ww, 6);
    // the box of disks, with the disks standing up in it
    const cols = ['#d8382c', '#2c5cc8', '#f0b020', '#2c9c58', '#8c3cb8', '#e8741c'];
    cols.forEach((c, i) => {
      g.save();
      g.translate(wx + 8 + i * 5.6, wy + 14);
      g.rotate((i - 2.5) * 0.07);
      P.floppy(g, -4.5, -11, 9, c);
      g.restore();
    });
    P.rect(g, wx + 4, wy + 13, 36, 13, ['#e8b468', '#9c6428'], { wob: 0.3, seed: 46, edge: '#3a2410', ea: 0.85 });
    P.rect(g, wx + 6, wy + 15, 32, 9, '#f6f0dc', { wob: 0.1, seed: 47 });
    P.text(g, 'DISKS', wx + 7.5 + 0.5, wy + 16, '#8c1410', 1, 1);
    g.fillStyle = P.col('#2c2c5c'); g.fillRect(wx + 7.5 + 30, wy + 16, 1, 7);
    // striped awning
    for (let i = 0; i < 11; i++) {
      const ax = wx - 3 + i * 4.6;
      P.shape(g, [[ax, wy - 7], [ax + 4.6, wy - 7], [ax + 5.6, wy - 1], [ax + 0.8, wy - 1]], i % 2 ? ['#ffffff', '#d0c8bc'] : ['#e04c38', '#982418'], { wob: 0.15, seed: 50 + i });
    }
    // the door and a window above
    P.shape(g, [[64, GY], [64, 128], [67, 125], [75, 125], [78, 128], [78, GY]], ['#3c5c8c', '#1c2c54'], { wob: 0.4, seed: 47, edge: '#0c1430', ea: 0.8 });
    P.rect(g, 66, 130, 10, 8, P.pick('#a8d0e0', '#ffd24a'), { wob: 0.2, seed: 48, edge: '#0c1430', ea: 0.7 });
    P.rect(g, 75, GY - 10, 1.8, 1.8, P.lit('#ffd24a'), { wob: 0.1 });
    // the pillar box by the door, red and fairly proud of it
    P.shadow(g, 94, GY + 2, 9, 2, 0.35);
    P.shape(g, [[89, GY + 1], [89, 138], [91, 135], [97, 135], [99, 138], [99, GY + 1]], ['#ec4c38', '#9c1c10'], { wob: 0.3, seed: 49, edge: '#3c0808', ea: 0.8 });
    P.shape(g, [[88, 137], [100, 137], [98, 134], [90, 134]], ['#c82c1c', '#7c1008'], { wob: 0.2, seed: 5 });
    g.fillStyle = P.col('#1c0808'); g.fillRect(91, 141, 6, 1);
    P.rect(g, 91, 146, 6, 4, '#ffe27a', { wob: 0.1, seed: 6 });
  }

  // ---- the shop on the right ----
  function shop(g, P) {
    const x = 238, w = 78, top = 92;
    P.shadow(g, x + w / 2, GY + 2, 44, 4, 0.3);
    const wall = [[x, GY], [x, top], [x + w, top], [x + w, GY]];
    P.shape(g, wall, ['#5cc0c8', '#2c8494', '#1c5c72'], { wob: 0.5, seed: 61, edge: '#0c2c38', ea: 0.55 });
    P.within(g, wall, { wob: 0.5, seed: 61 }, () => {
      g.fillStyle = P.alpha(P.col('#0c2c38'), 0.25); g.fillRect(x + w - 8, top, 8, GY - top);
      P.dabs(g, x, top, w, GY - top, ['#e8ffff', '#1c5c72'], 16, 1.6, 62, 0.2);
    });
    P.shape(g, [[x - 3, top + 1], [x - 3, top - 5], [x + w + 3, top - 5], [x + w + 3, top + 1]], ['#f4e8c8', '#a8946c'], { wob: 0.4, seed: 63, edge: '#3a2c1c', ea: 0.6 });
    // the sign
    P.rect(g, x + 2, top + 3, 74, 11, ['#fff4d0', '#e8d098'], { wob: 0.3, seed: 64, edge: '#2c1a0c', ea: 0.8 });
    P.text(g, 'BITS & BYTES', x + 3.5, top + 5, '#2c1c5c', 1, 1);
    // the window, with a monitor showing a face made of ones and zeros
    const wx = x + 6, wy = top + 20, ww = 46, wh = 30;
    P.rect(g, wx - 2, wy - 2, ww + 4, wh + 4, ['#f4e8c8', '#a8946c'], { wob: 0.3, seed: 65, edge: '#2c2010', ea: 0.7 });
    g.fillStyle = P.pick('#dff4f8', '#ffd870'); g.fillRect(wx, wy, ww, wh);
    if (P.dusk) P.glow(g, wx + ww / 2, wy + wh / 2, 34, '#ffb830', 0.45);
    P.rect(g, wx + 5, wy + 4, 20, 15, ['#d8d4c8', '#8c8878'], { wob: 0.3, seed: 66, edge: '#2c2c28', ea: 0.8 });
    g.fillStyle = P.lit('#103c24'); g.fillRect(wx + 7, wy + 6, 16, 10);
    g.fillStyle = P.lit('#5cff8c');
    for (const [dx, dy] of [[8, 7], [10, 7], [13, 7], [16, 7], [20, 7], [9, 9], [12, 9], [15, 9], [18, 9], [21, 9], [8, 11], [11, 11], [14, 11], [19, 11], [10, 13], [13, 13], [17, 13], [21, 13]]) g.fillRect(wx + dx, wy + dy, 1, 2);
    P.rect(g, wx + 11, wy + 19, 8, 2, '#9c9888', { wob: 0.1, seed: 67 });
    // boxes of bits on the shelf
    for (const [dx, c, dw] of [[28, '#d8382c', 7], [36, '#f0b020', 6], [43, '#2c5cc8', 5]]) {
      P.rect(g, wx + dx, wy + 18, dw, 9, [c, P.mix(c, '#000000', 0.35)], { wob: 0.2, seed: dx, edge: '#1c1008', ea: 0.6 });
    }
    g.fillStyle = P.col('#3a2418'); g.fillRect(wx, wy + 27, ww, 3);
    // the banner under the window
    P.rect(g, x + 4, GY - 27, 52, 17, ['#e8484c', '#9c1c28'], { wob: 0.3, seed: 68, edge: '#3c0810', ea: 0.8 });
    P.text(g, 'NOW WITH', x + 6.5, GY - 25.5, '#fff4d0', 1, 0);
    P.text(g, 'MORE BITS', x + 6.5, GY - 17, '#ffe27a', 1, 0);
    // door
    P.shape(g, [[x + 62, GY], [x + 62, top + 24], [x + 65, top + 21], [x + 73, top + 21], [x + 76, top + 24], [x + 76, GY]], ['#e8c04a', '#a87818'], { wob: 0.4, seed: 69, edge: '#3a2410', ea: 0.8 });
    P.rect(g, x + 65, top + 26, 8, 12, P.pick('#c8e8f0', '#ffd24a'), { wob: 0.2, seed: 70, edge: '#3a2410', ea: 0.7 });
    P.rect(g, x + 64, GY - 9, 1.8, 1.8, '#3a2410', { wob: 0.1 });
    // flower box
    P.rect(g, x + 4, GY - 3, 52, 3, ['#8c5c30', '#4a2c18'], { wob: 0.2, seed: 71 });
    P.dabs(g, x + 4, GY - 8, 52, 7, ['#f06a9c', '#ffe27a', '#ffffff', '#4c8c34'], 22, 1.5, 72, 0.9);
  }

  // ---- the red telephone booth and a street lamp ----
  function booth(g, P) {
    const x = 99, w = 19, top = 124;
    P.shadow(g, x + w / 2 + 1, GY + 8, 13, 2.6, 0.38);
    const body = [[x, GY + 7], [x, top + 3], [x + 2, top], [x + w - 2, top], [x + w, top + 3], [x + w, GY + 7]];
    P.shape(g, body, ['#f2503c', '#c42c1c', '#8c1410'], { wob: 0.3, seed: 81, edge: '#2c0604', ea: 0.9 });
    // dark interior through the panes
    const glass = P.dusk ? P.lit('#ffcf70') : P.col('#3c4c64');
    const panes = [];
    for (let c = 0; c < 2; c++) for (let r = 0; r < 3; r++) panes.push([x + 2.5 + c * 7.4, top + 9 + r * 7.2, 6.2, 5.6]);
    for (const [px, py, pw, ph] of panes) { g.fillStyle = glass; g.fillRect(px, py, pw, ph); g.fillStyle = P.alpha('#ffffff', 0.28); g.fillRect(px, py, pw, 1); }
    if (P.dusk) P.glow(g, x + w / 2, top + 20, 26, '#ffb830', 0.5);
    // the little crowned header with its lettering
    P.rect(g, x + 1.5, top + 1.5, w - 3, 7, ['#fff4d0', '#d8c898'], { wob: 0.1, seed: 82 });
    P.text(g, 'TEL', x + 1.9, top + 1.8, '#8c1410', 1, 0);
    P.shape(g, [[x - 1, top + 1], [x + w / 2, top - 4], [x + w + 1, top + 1]], ['#e04030', '#8c1410'], { wob: 0.2, seed: 83, edge: '#2c0604', ea: 0.8 });
    // a phone you can just make out in there
    P.rect(g, x + 8, top + 12, 3, 4, '#14141c', { wob: 0.1, seed: 84 });
    g.fillStyle = P.col('#ffe27a'); g.fillRect(x + 9, top + 17, 1, 3);
    // street lamp
    const lx = 130;
    P.shadow(g, lx, GY + 7, 7, 1.6, 0.3);
    P.rect(g, lx - 1, 112, 2, GY + 6 - 112, ['#5c6074', '#2c2c3c'], { wob: 0.2, seed: 85 });
    P.shape(g, [[lx - 4, 112], [lx - 3, 106], [lx + 3, 106], [lx + 4, 112]], P.dusk ? P.lit('#ffe08a') : P.col('#e8f0e0'), { wob: 0.2, seed: 86, edge: '#1c1c28', ea: 0.9 });
    P.shape(g, [[lx - 5, 106], [lx, 102], [lx + 5, 106]], ['#3c3c4c', '#14141c'], { wob: 0.2, seed: 87 });
    if (P.dusk) P.glow(g, lx, 109, 26, '#ffd060', 0.55);
  }

  function fountainBase(g, P) {
    const cx = 207, cy = 163;
    P.shadow(g, cx + 3, cy + 6, 26, 4.2, 0.34);
    // basin: back rim, water, front lip
    P.blob(g, cx, cy, 24, 7, ['#e8e0d0', '#a89c8c'], { wob: 0.4, seed: 91, n: 18, edge: '#3a3024', ea: 0.6 });
    P.blob(g, cx, cy - 0.5, 21.5, 5.6, ['#78c4e8', '#2c7ab0'], { wob: 0.3, seed: 92, n: 18 });
    P.dabs(g, cx - 18, cy - 4, 36, 7, ['#d8f4ff', '#58a8d8'], 14, 1.4, 93, 0.5);
    P.shape(g, [[cx - 24, cy], [cx - 24, cy + 6], [cx - 16, cy + 9.5], [cx + 16, cy + 9.5], [cx + 24, cy + 6], [cx + 24, cy]], ['#d4ccb8', '#8c8070'], { wob: 0.4, seed: 94, edge: '#3a3024', ea: 0.6 });
    // pedestal and upper bowl
    P.shape(g, [[cx - 3, cy], [cx - 2, cy - 22], [cx + 2, cy - 22], [cx + 3, cy]], ['#e0d8c4', '#8c8070'], { wob: 0.3, seed: 95, edge: '#3a3024', ea: 0.6 });
    P.blob(g, cx, cy - 21, 10, 3.4, ['#ece4d4', '#a89c8c'], { wob: 0.3, seed: 96, n: 14, edge: '#3a3024', ea: 0.6 });
    P.blob(g, cx, cy - 21.6, 8.4, 2.4, ['#78c4e8', '#2c7ab0'], { wob: 0.2, seed: 97, n: 12 });
    P.shape(g, [[cx - 1.4, cy - 22], [cx - 1, cy - 28], [cx + 1, cy - 28], [cx + 1.4, cy - 22]], ['#e0d8c4', '#8c8070'], { wob: 0.2, seed: 98 });
  }

  function ground(g, P) {
    g.fillStyle = P.grad(g, 0, GY, 0, 200, [[0, '#d8c8a0'], [0.5, '#c4b088'], [1, '#a89468']]);
    g.fillRect(0, GY, 320, 48);
    // cobbles in rows that grow toward us
    const r = P.mulberry(12);
    for (let y = GY + 2, row = 0; y < 200; row++) {
      const h = 3 + (y - GY) * 0.07, wd = 5 + (y - GY) * 0.16;
      for (let x = -((row * 7) % wd); x < 322; x += wd) {
        const j = r();
        g.fillStyle = P.alpha(P.col(j < 0.5 ? '#8c7858' : j < 0.8 ? '#f0e4c0' : '#6c5a40'), 0.28);
        g.fillRect(x + 1, y, wd - 2, h - 1);
      }
      y += h;
    }
    // a pale path from the foreground up to the fountain, and the kerb
    P.shape(g, [[150, 200], [250, 200], [226, 170], [188, 170]], ['#ecdcb4', '#cdb98c'], { wob: 1.4, seed: 101, edge: '#6c4a2c', ea: 0.2 });
    P.dabs(g, 0, GY, 320, 48, ['#6c5a40', '#fff4d0'], 60, 1.6, 102, 0.25);
    g.fillStyle = P.alpha(P.col('#3a2c1c'), 0.35); g.fillRect(0, GY, 320, 1);
  }

  function foreground(g, P) {
    // planters at the front corners
    P.shape(g, [[4, 198], [6, 184], [30, 184], [32, 198]], ['#c8844c', '#7c4624'], { wob: 0.4, seed: 111, edge: '#2c1608', ea: 0.7 });
    P.canopy(g, 18, 180, 13, 9, ['#2c5c2c', '#4c9438', '#98d058'].map((c) => P.col(c)), 112);
    P.dabs(g, 8, 172, 20, 10, ['#f06a9c', '#ffe27a', '#ffffff'], 12, 1.4, 113, 0.9);
    P.shape(g, [[290, 198], [292, 186], [314, 186], [316, 198]], ['#c8844c', '#7c4624'], { wob: 0.4, seed: 114, edge: '#2c1608', ea: 0.7 });
    P.canopy(g, 303, 182, 12, 8, ['#2c5c2c', '#4c9438', '#98d058'].map((c) => P.col(c)), 115);
    P.dabs(g, 294, 174, 20, 9, ['#e8741c', '#ffe27a', '#ffffff'], 12, 1.4, 116, 0.9);
  }

  function paint(g, P) {
    sky(g, P);
    hills(g, P);
    clockTower(g, P);
    postOffice(g, P);
    shop(g, P);
    ground(g, P);
    booth(g, P);
    fountainBase(g, P);
    foreground(g, P);
  }

  // ---------- animated bits ----------

  // Morse for "HI": the fountain says it over and over, a dot being a short hop of water.
  const MORSE = [1, 1, 1, 1, 0, 1, 1];            // 1 = dot, 0 = pause between letters
  const PULSE = 0.38;

  const anims = [
    {
      x: 0, y: 0, w: 320, h: 70, sample: 20,
      draw(g, t, P) {
        for (const [x0, y, s, seed] of [[0, 24, 1.5, 3], [100, 46, 1.0, 5], [190, 16, 1.8, 8], [250, 40, 1.1, 12]]) {
          const x = ((x0 + t * (1.2 + seed * 0.14) + 60) % 400) - 60;
          P.cloud(g, x, y, s, seed);
        }
      },
    },
    {
      // the clock: hands that wander, not quite agreeing with each other
      x: 144, y: 68, w: 24, h: 24, sample: 0,
      draw(g, t, P) {
        const m = t * 0.35 + 1.2, h = t * 0.03 + 4.2;
        g.strokeStyle = P.col('#2c1c18'); g.lineCap = 'round';
        g.lineWidth = 1.4;
        g.beginPath(); g.moveTo(156, 80); g.lineTo(156 + Math.sin(h) * 5, 80 - Math.cos(h) * 5); g.stroke();
        g.lineWidth = 1;
        g.beginPath(); g.moveTo(156, 80); g.lineTo(156 + Math.sin(m) * 8, 80 - Math.cos(m) * 8); g.stroke();
      },
    },
    {
      // the fountain spells "HI" in water, a spout at a time
      x: 186, y: 112, w: 42, h: 52, sample: 0,
      draw(g, t, P) {
        const cx = 207;
        const slot = Math.floor(t / PULSE) % (MORSE.length + 4);
        const k = (t / PULSE) % 1;
        const on = slot < MORSE.length && MORSE[slot] === 1;
        const col = P.pick('#e8faff', '#a8d8ff');
        g.fillStyle = col;
        // the steady trickle from the top bowl
        for (let i = 0; i < 4; i++) g.fillRect(cx - 7 + i * 4 + (Math.floor(t * 3) % 2), 142 + (i % 2), 1, 2);
        if (!on) return;
        const lift = Math.sin(Math.min(1, k * 1.3) * Math.PI);
        for (let i = 0; i < 7; i++) {
          const u = i / 6, h = 4 + lift * 15 * (1 - u * 0.2);
          const dx = (u - 0.5) * 7 * (0.6 + lift);
          g.fillRect(Math.round(cx + dx), Math.round(135 - h + u * 6 + Math.abs(dx) * 0.8), 1, 2);
        }
        g.fillRect(cx - 1, Math.round(133 - lift * 18), 2, 3);
        g.fillStyle = P.alpha('#ffffff', 0.8);
        g.fillRect(cx, Math.round(133 - lift * 18), 1, 1);
      },
    },
    {
      // a very patient snail, first in the queue for the booth; it advances one step now and then
      x: 116, y: 164, w: 30, h: 18, sample: 0,
      draw(g, t, P) {
        const sway = Math.sin(t * 1.3);
        const x = 124 + Math.floor((t % 40) / 40 * 3), y = 176;
        P.shape(g, [[x - 6, y], [x - 5, y - 2.4], [x + 4, y - 2.4], [x + 8, y - 0.5], [x + 8.5, y + 0.8], [x - 6, y + 0.8]], ['#c8d8a0', '#7c9460'], { wob: 0.2, seed: 5, edge: '#2c3c1c', ea: 0.6 });
        P.blob(g, x - 0.5, y - 5, 4.4, 4.4, ['#f0a050', '#a85018'], { wob: 0.2, seed: 6, n: 12, edge: '#3c1c08', ea: 0.7 });
        g.strokeStyle = P.col('#5c2808'); g.lineWidth = 0.8;
        g.beginPath(); g.arc(x - 0.5, y - 5, 2.4, 0.5, 4.6); g.stroke();
        P.line(g, [[x + 7, y - 1.2], [x + 8 + sway * 0.8, y - 5]], '#7c9460', 0.9);
        P.line(g, [[x + 5.2, y - 1.6], [x + 5.6 - sway * 0.8, y - 5]], '#7c9460', 0.9);
        g.fillStyle = '#14141c';
        g.fillRect(Math.round(x + 7.6 + sway * 0.8), y - 6, 1.4, 1.4);
        g.fillRect(Math.round(x + 5.2 - sway * 0.8), y - 6, 1.4, 1.4);
        // a tiny scarf, because the queue is long
        g.fillStyle = P.col('#e04c38'); g.fillRect(x + 4, y - 2.6, 2, 3);
      },
    },
    {
      // a pigeon on the post round: it hurries across the square with a letter in its beak, in at one
      // edge and out at the other (the rectangle is the whole width, so it is never cut off on the way)
      gag: { start: 4, period: 24, dur: 16 }, x: 0, y: 170, w: 320, h: 28, sample: 0,
      draw(g, t, P, u) {
        const x = 330 - u * 344, y = 188 + Math.abs(Math.sin(u * 108)) * -0.8;
        const f = Math.sin(u * 162) * 1.8;
        P.shape(g, [[x - 4, y - 1], [x - 2, y - 5.5], [x + 3, y - 5.5], [x + 5, y - 2], [x + 2, y + 0.6], [x - 3, y + 0.6]], ['#b8bcd0', '#6c708c'], { wob: 0.2, seed: 7, edge: '#2c2c40', ea: 0.6 });
        P.shape(g, [[x - 4, y - 3], [x - 8, y - 4.4], [x - 7, y - 2]], '#8c90ac', { wob: 0.1, seed: 8 });
        P.blob(g, x + 4.4, y - 7.4, 2.2, 2.2, ['#d8dcec', '#8c90ac'], { wob: 0.1, seed: 9, n: 8 });
        g.fillStyle = '#e8741c'; g.fillRect(x + 6.4, y - 7.2, 2, 1);
        g.fillStyle = '#14141c'; g.fillRect(x + 4.6, y - 8, 1, 1);
        g.fillStyle = P.col('#fff6e0'); g.fillRect(x + 6.8, y - 6.2, 4, 3);
        g.fillStyle = P.col('#d8382c'); g.fillRect(x + 8, y - 5.2, 1, 1);
        g.fillStyle = '#e8741c';
        g.fillRect(x - 1 + f, y + 0.6, 1, 2.4); g.fillRect(x + 1.4 - f, y + 0.6, 1, 2.4);
      },
    },
  ];

  const hotspots = [
    { id: 'clock', x: 138, y: 24, w: 38, h: 112, say: [
      'The clock reads a quarter past something. It has been that for years, and refuses to be rushed.',
      'The clock strikes thirteen, just to see who is paying attention.',
      'The hands are held up by a committee. The committee has not met since spring.'] },
    { id: 'postoffice', x: 6, y: 98, w: 80, h: 54, say: [
      'The post office is open. A sign says "Back in five minutes". It does not say which five.',
      'Inside, a clerk is stamping everything, including the stamps.'] },
    { id: 'shopsign', x: 238, y: 90, w: 78, h: 62, say: [
      'The sign promises NOW WITH MORE BITS. Nobody asked what it was before.',
      'The monitor in the window shows a smiley made of ones and zeros. It is mostly ones.',
      'The shop is open until the last bit is sold. The last bit is not for sale.'] },
    { id: 'fountain', x: 182, y: 120, w: 52, h: 52, say: [
      'The fountain is spouting in Morse code. You think it says "HI". It says it very wetly.',
      'It pauses for a long dash, then makes a splash. You decide that was a full stop.'] },
    { id: 'snail', x: 118, y: 166, w: 28, h: 16, say: [
      'A snail, first in the queue for the booth. It has been there since the booth was painted.',
      'The snail has brought a flask and a small folding stool. It is a professional.',
      'You ask whether it is next. It nods, eventually.'] },
    { id: 'pigeon', x: 160, y: 176, w: 160, h: 24, say: [
      'A pigeon delivers a letter by hand. By beak, strictly. The post office has been told.',
      'The pigeon holds out the letter. It is addressed to "The Person Who Looks Like They Are Reading This".'] },
    { id: 'lamp', x: 124, y: 98, w: 14, h: 58, say: [
      'A street lamp. It comes on at dusk, or whenever it is feeling dramatic.'] },
    { id: 'pillar', x: 86, y: 132, w: 18, h: 24, say: [
      'A pillar box. It has been fed so many letters that it is a little round.'] },
    { id: 'clouds', x: 0, y: 0, w: 320, h: 70, say: [
      'A cloud drifts over the square, taking its time. Rain is not scheduled until Thursday.'] },
  ];

  // The two objects that run the game: where the disks live and the telephone (adv.js).
  hotspots.push(
    { id: 'parcels', role: 'disks', x: 12, y: 116, w: 48, h: 36, say: ['A box of disks in the post office window, marked "hand delivery only".'] },
    { id: 'booth', role: 'phone', x: 96, y: 118, w: 24, h: 44, say: ['A red telephone booth, slightly bigger on the inside, according to the snail.'] });

  const objects = {
    disks: {
      open: 'The clerk slides the box out of the window and stamps it, ten times to be safe.',
      got: 'There is a red telephone booth by the lamp. A snail is waiting politely outside it.',
      again: 'The clerk sniffs. Returns are by appointment only, but he stamps it anyway.',
      hint: 'Something in the post office window is twinkling beside the parcels. It is probably the box of disks.',
    },
    phone: {
      refuse: 'You step into the booth and lift the receiver. A voice says: "Disks first, please." The snail behind you tuts, slowly.',
      answer: 'Exchange. Whom shall I ring?',
      hint: 'The telephone booth is twinkling. The snail has noticed, but it is not in a hurry.',
      busy: 'The booth door is shut. The snail looks at you and then, deliberately, at the clock.',
    },
  };

  return { id: 'town', title: 'The town square', focus: { x: 160, y: 110 }, paint, anims, hotspots, objects,
    termAt: [4, 8],
    spark: { disks: [10, 112, 54, 42], phone: [94, 116, 28, 48] } };
})());
