// Scene "diner": a chrome roadside diner at the edge of nowhere. Original artwork, drawn in code.
'use strict';

InnScenes.register((() => {
  const SIGN = { x: 252, y: 52, w: 56, h: 36 };

  function sky(g, P) {
    g.fillStyle = P.gradRaw(g, 0, 0, 0, 132, P.pick(
      [[0, '#3a84d6'], [0.45, '#8cc8ec'], [0.82, '#fde6b6'], [1, '#ffd49a']],
      [[0, '#190f4a'], [0.4, '#4c2878'], [0.78, '#d8507c'], [1, '#ffa45c']]));
    g.fillRect(0, 0, 320, 150);
    if (P.dusk) {
      P.stars(g, 0, 80, 110, 6);
      P.glow(g, 70, 128, 90, '#ffb060', 0.55);
    } else {
      P.glow(g, 84, 40, 80, '#fff8d0', 0.6);
      P.glow(g, 84, 40, 24, '#ffffff', 0.7);
    }
  }

  function mesa(g, P, x0, x1, top, base, cols, seed, bands) {
    const w = x1 - x0, t = Math.min(10, w * 0.16);
    const pts = [[x0, base], [x0 + t * 0.5, top + (base - top) * 0.55], [x0 + t * 0.9, top + (base - top) * 0.5], [x0 + t * 1.4, top + 5],
      [x0 + t * 2.2, top], [x1 - t * 2.2, top], [x1 - t * 1.4, top + 4], [x1 - t * 0.8, top + (base - top) * 0.42],
      [x1 - t * 0.5, top + (base - top) * 0.6], [x1, base]];
    P.shape(g, pts, cols, { wob: 1.3, seed, v: true });
    P.within(g, pts, { wob: 1.3, seed }, () => {
      for (let i = 0; i < bands; i++) {
        const y = top + 6 + i * ((base - top - 6) / bands) + (i % 2);
        g.fillStyle = P.alpha(P.pick('#7a2c30', '#1c0c30'), 0.16 + (i % 3) * 0.05);
        g.fillRect(x0, y, w, 1 + (i % 2));
      }
      g.fillStyle = P.alpha(P.pick('#3c1430', '#0c0620'), 0.3);
      g.fillRect(x0 + w * 0.7, top, w * 0.4, base - top);
    });
  }

  function desert(g, P) {
    // far, hazy mesas, then closer red ones
    mesa(g, P, 40, 110, 96, 142, ['#d8aaa8', '#c496a0'], 3, 3);
    mesa(g, P, 190, 300, 88, 142, ['#d49a98', '#b88290'], 5, 4);
    mesa(g, P, 270, 340, 100, 142, ['#d8a49a', '#c08c92'], 8, 3);
    g.fillStyle = P.gradRaw(g, 0, 100, 0, 134, [[0, P.alpha(P.pick('#fde6b6', '#d8507c'), 0)], [1, P.alpha(P.pick('#fde6b6', '#d8507c'), 0.55)]]);
    g.fillRect(0, 100, 320, 34);
    mesa(g, P, -20, 78, 108, 138, ['#d8704c', '#a44c3c'], 12, 5);
    mesa(g, P, 112, 168, 114, 140, ['#c8664a', '#94483c'], 14, 4);
    // the sand
    P.ridge(g, { y: 136, amp: 2.5, freq: 0.04, seed: 5, bottom: 200, cols: ['#f2d28c', '#e4b46a', '#c89050'] });
    P.dabs(g, 0, 138, 320, 62, ['#fbe4a8', '#c8964e', '#a87c44', '#e8b870'], 110, 3.2, 17, 0.35);
    // scrub brush
    const r = P.mulberry(9);
    for (let i = 0; i < 26; i++) {
      const x = r() * 320, y = 140 + r() * 22;
      P.canopy(g, x, y, 3 + r() * 3, 2 + r() * 2, ['#4c5c34', '#8a9a50', '#b8c070'].map((c) => P.col(c)), 200 + i);
    }
    // distant telephone line
    for (let x = 6; x < 330; x += 34) {
      const y = 134 + Math.sin(x * 0.05) * 1;
      P.rect(g, x, y - 12, 1.4, 12, '#3a2c28', { wob: 0.1 });
      P.rect(g, x - 2, y - 11, 5.4, 1, '#3a2c28', { wob: 0.1 });
    }
    for (let x = 6; x < 300; x += 34) P.wire(g, [x, 123.5], [x + 34, 123.5], 2, P.pick('#3a2c28', '#140c28'), 1);
  }

  function road(g, P) {
    // gravel lot in front of the diner
    P.shape(g, [[130, 166], [290, 166], [320, 182], [-4, 182], [-4, 176]], ['#e6c88c', '#c4a066'], { wob: 1.4, seed: 8, v: true });
    P.dabs(g, 0, 166, 320, 16, ['#fbe4b0', '#a88050', '#8c6c44'], 60, 1.8, 11, 0.35);
    // highway
    P.shape(g, [[-4, 182], [324, 180], [324, 202], [-4, 202]], ['#6c6878', '#3e3c4c'], { wob: 0.8, seed: 21, v: true });
    P.dabs(g, 0, 182, 320, 18, ['#7c788a', '#2c2a38', '#58546a'], 60, 2.4, 22, 0.4);
    g.fillStyle = P.col('#f6f0d8');
    g.fillRect(0, 183, 320, 1);
    for (let x = -6; x < 330; x += 28) P.shape(g, [[x, 191], [x + 15, 190.6], [x + 15, 192.2], [x, 192.6]], P.pick('#f6c83c', '#d8aa38'), { wob: 0.4, seed: x });
  }

  function diner(g, P) {
    P.shadow(g, 214, 166, 58, 4, 0.3);
    // sign pole, with a back brace
    P.shape(g, [[279, 88], [281.5, 88], [282, 168], [278.5, 168]], ['#a4acc0', '#4c5470'], { wob: 0.3, seed: 31 });
    // main body: a long streamlined car
    const body = [[164, 168], [164, 142], [168, 134], [176, 129], [252, 129], [260, 134], [264, 142], [264, 168]];
    P.shape(g, body, ['#fcfcff', '#c8d0e0', '#8c98b4'], { wob: 0.7, seed: 32, v: true, edge: '#3c4260', ea: 0.45 });
    P.within(g, body, { wob: 0.7, seed: 32 }, () => {
      // red stripe, sky reflected in the chrome
      g.fillStyle = P.col('#d8302c'); g.fillRect(160, 150, 110, 5);
      g.fillStyle = P.col('#f06048'); g.fillRect(160, 150, 110, 1);
      g.fillStyle = P.col('#24607c'); g.fillRect(160, 158, 110, 1.5);
      g.fillStyle = P.alpha(P.pick('#7ab8e8', '#6a50a8'), 0.28); g.fillRect(160, 156, 110, 12);
    });
    // window band with patrons
    const win = P.dusk ? '#ffd870' : '#8cc8dc';
    P.shape(g, [[170, 135], [168, 144], [258, 144], [256, 135]], [win, P.dusk ? '#f09848' : '#4c88a8'], { wob: 0.4, seed: 33, v: true, edge: '#2c3250', ea: 0.7 });
    if (P.dusk) P.glow(g, 214, 140, 62, '#ffb040', 0.4);
    for (const [hx, c] of [[184, '#6a3c2c'], [200, '#2c2c4c'], [236, '#a8483c'], [248, '#3a5a3c']]) {
      P.blob(g, hx, 140.5, 2.6, 2.8, P.pick('#4c5c70', c), { wob: 0.2, seed: hx, n: 8 });
      P.rect(g, hx - 3, 142.5, 6, 1.6, P.pick('#4c5c70', c), { wob: 0.1 });
    }
    g.fillStyle = P.col('#e8e8f4');
    for (const x of [176, 190, 208, 228, 242]) g.fillRect(x, 135, 1.4, 9);
    // roof
    P.shape(g, [[160, 131], [168, 127], [260, 127], [268, 131], [266, 134], [162, 134]], ['#f04c3c', '#a8201c'], { wob: 0.5, seed: 34, v: true, edge: '#4c0c0c', ea: 0.5 });
    for (let x = 172; x < 262; x += 10) P.rect(g, x, 128, 6, 1.4, P.pick('#fff4c8', '#ffe8a0'), { wob: 0.1 });
    // door and step
    P.shape(g, [[212, 168], [212, 148], [224, 148], [224, 168]], ['#2c3858', '#141c34'], { wob: 0.3, seed: 35, edge: '#e8e8f4', ea: 0.9 });
    P.shape(g, [[213.5, 167], [213.5, 150], [222.5, 150], [222.5, 167]], P.dusk ? '#ffd070' : '#a8dcec', { wob: 0.2, seed: 36 });
    P.rect(g, 209, 167, 18, 2, '#9ca4b8', { wob: 0.2 });
    // chimney vent and a little stainless awning
    P.rect(g, 234, 120, 8, 8, ['#c8d0e0', '#7c88a4'], { wob: 0.3, seed: 37 });
    P.rect(g, 232, 118.5, 12, 2.4, '#9ca4b8', { wob: 0.2 });
    P.text(g, 'EATS', 175, 160, '#fff6e8', 1, 1);
    P.text(g, 'PIE', 238, 160, '#fff6e8', 1, 1);
    // the sign cabinet
    const s = SIGN;
    P.shape(g, [[s.x, s.y], [s.x + s.w, s.y], [s.x + s.w + 2, s.y + s.h], [s.x - 2, s.y + s.h]], ['#e4e8f4', '#8c94b0'], { wob: 0.5, seed: 38, edge: '#2c3250', ea: 0.8 });
    P.rect(g, s.x + 3, s.y + 3, s.w - 6, s.h - 6, P.pick('#3a2c58', '#120a2a'), { wob: 0.3, seed: 39 });
    // unlit tubes; the lit ones are drawn by the animation
    P.text(g, 'OPEN', s.x + 8, s.y + 6, P.pick('#9a6c8c', '#4a3058'), 2, 0);
    P.text(g, '24 BAUD', s.x + 8, s.y + 24, P.pick('#6c9ca8', '#304860'), 1, 1);
    // chasing bulbs round the edge
    for (let x = s.x + 2; x < s.x + s.w; x += 5) { g.fillStyle = P.lit(P.pick('#ffe890', '#ffcc60')); g.fillRect(x, s.y + 0.5, 1.6, 1.6); g.fillRect(x, s.y + s.h - 1.5, 1.6, 1.6); }
  }

  function saguaro(g, P, x, y, h) {
    const green = ['#9cd070', '#4c8c48', '#2c5c3c'];
    const trunk = [[x - 4, y], [x - 4.5, y - h + 6], [x - 2.5, y - h + 1], [x, y - h], [x + 2.5, y - h + 1], [x + 4.5, y - h + 6], [x + 4, y]];
    const arm = (sx, sy, dx, up) => [[x + sx, y - sy], [x + sx + dx, y - sy], [x + sx + dx, y - sy - up + 2], [x + sx + dx + 1.5, y - sy - up], [x + sx + dx + 4, y - sy - up + 1], [x + sx + dx + 4, y - sy + 4], [x + sx, y - sy + 4]];
    P.shape(g, arm(-4, h * 0.42, -9, 22), green, { wob: 0.5, seed: 61 });
    P.shape(g, arm(4, h * 0.55, 5, 16), green, { wob: 0.5, seed: 62 });
    P.shape(g, trunk, green, { wob: 0.5, seed: 63, edge: '#1c3c24', ea: 0.45 });
    P.within(g, trunk, { wob: 0.5, seed: 63 }, () => {
      g.fillStyle = P.alpha('#1c3c24', 0.35);
      for (const dx of [-1.6, 0.6, 2.8]) g.fillRect(x + dx, y - h, 0.8, h);
    });
    P.grain(g, x - 14, y - h, 28, h, '#f6f2d0', 60, 64, 0.8);
  }

  function paint(g, P) {
    sky(g, P);
    desert(g, P);
    road(g, P);
    diner(g, P);
    // saguaro by the diner, then a big rock for the jackalope
    P.shadow(g, 147, 169, 11, 2.5, 0.3);
    saguaro(g, P, 146, 168, 50);
    // headset: band over the top, ear cups on each side
    const hx = 146, hy = 118;
    g.strokeStyle = P.col('#1c1c28'); g.lineWidth = 1.6; g.lineCap = 'round';
    g.beginPath(); g.arc(hx, hy + 2, 5.6, Math.PI * 1.08, Math.PI * 1.92); g.stroke();
    P.blob(g, hx - 5.4, hy + 3.4, 1.9, 2.6, '#2c2c3c', { wob: 0.2, seed: 5, n: 8 });
    P.blob(g, hx + 5.4, hy + 3.4, 1.9, 2.6, '#2c2c3c', { wob: 0.2, seed: 6, n: 8 });
    g.beginPath(); g.moveTo(hx + 5.2, hy + 5.4); g.quadraticCurveTo(hx + 4, hy + 11, hx + 0.5, hy + 9.5); g.stroke();
    P.blob(g, hx + 0.5, hy + 9.4, 1.3, 1.3, '#1c1c28', { wob: 0.1, seed: 7, n: 8 });
    // a pink flower on top, as is customary
    P.blob(g, hx - 1, hy - 1.5, 1.6, 1.6, '#f06a9c', { wob: 0.2, seed: 9, n: 8 });
    // rock
    const rock = [[278, 180], [282, 172], [290, 169], [306, 169], [314, 174], [318, 181]];
    P.shape(g, rock, ['#c8a47c', '#7c5c40'], { wob: 1.0, seed: 71, edge: '#4a3020', ea: 0.4 });
    P.dabs(g, 280, 166, 36, 14, ['#6c4c30', '#f0d4a0'], 14, 1.6, 72, 0.4);
    // a few stones, and one of those fallen cattle-skull things
    P.dabs(g, 0, 168, 320, 12, ['#a88050', '#f4e0b0'], 26, 1.2, 73, 0.7);
    if (P.dusk) { P.glow(g, 290, 168, 46, '#ff3a9a', 0.3); P.glow(g, 262, 174, 40, '#40e8ff', 0.16); }
    for (const [x, y, r, sd] of [[128, 176, 4, 1], [172, 174, 3, 2], [244, 175, 5, 3], [268, 172, 3, 4], [100, 172, 4, 5]]) {
      P.blob(g, x, y, r, r * 0.6, ['#e0c498', '#8c6c4c'], { wob: 0.6, seed: 90 + sd, edge: '#4a3020', ea: 0.4 });
    }
    for (const [x, y] of [[120, 170], [262, 177], [30, 175]]) P.canopy(g, x, y, 7, 4, ['#5c6a38', '#98a85a', '#c8d080'].map((c) => P.col(c)), 300 + x);
  }

  // ---------- animation ----------

  const anims = [
    {
      // the sign's tubes buzz and flicker
      x: SIGN.x, y: SIGN.y, w: SIGN.w + 2, h: SIGN.h, sample: 0,
      draw(g, t, P) {
        const s = SIGN;
        const flick = Math.sin(t * 13) > 0.92 || (t % 7.3) > 7.0;
        const pulse = 0.78 + 0.22 * Math.sin(t * 2.4);
        if (P.dusk) {
          P.glow(g, s.x + 28, s.y + 11, 36, '#ff3a9a', 0.55 * pulse);
          P.glow(g, s.x + 28, s.y + 26, 34, '#40e8ff', 0.4 * pulse);
        }
        // OPEN: the N is the one with a loose connection
        const w1 = 12;
        g.save();
        g.globalAlpha = P.dusk ? 1 : 0.62;
        for (let i = 0; i < 4; i++) {
          if (i === 2 && flick) continue;
          P.text(g, 'OPEN'[i], s.x + 8 + i * w1, s.y + 6, P.lit('#ff58a8'), 2, 0);
        }
        P.text(g, '24 BAUD', s.x + 8, s.y + 24, P.lit('#58f0ff'), 1, 1);
        g.restore();
        // the chaser bulbs
        for (let x = s.x + 2, i = 0; x < s.x + s.w; x += 5, i++) {
          const on = (Math.floor(t * 4) + i) % 3 === 0;
          g.fillStyle = on ? P.lit('#fffbd0') : P.col('#8c7040');
          g.fillRect(x, s.y + 0.5, 1.6, 1.6);
          g.fillRect(x, s.y + s.h - 1.5, 1.6, 1.6);
        }
      },
    },
    {
      // the saucer, its beam, the cow and the telephone cord
      x: 150, y: 6, w: 110, h: 162, sample: 14,
      draw(g, t, P) {
        const sx = 205, bob = Math.sin(t * 1.4) * 2, sy = 34 + bob;
        const cyc = (t - 4) % 24;
        let lift = 0, beam = 0;
        if (t >= 4 && cyc < 14) {
          const m = cyc;
          beam = m < 1.5 ? m / 1.5 : m > 12.5 ? (14 - m) / 1.5 : 1;
          lift = m < 2 ? 0 : m < 5 ? (m - 2) / 3 : m < 9 ? 1 : m < 12 ? 1 - (m - 9) / 3 : 0;
          lift = lift * lift * (3 - 2 * lift);
        }
        const cowY = 164 - lift * 70, cowX = sx + 4 + Math.sin(t * 2.2) * 2 * lift;
        // beam
        if (beam > 0) {
          g.save();
          g.globalAlpha = 0.34 * beam;
          g.fillStyle = P.gradRaw(g, 0, sy, 0, 168, [[0, '#f8ffb0'], [1, '#a8ffd8']]);
          g.beginPath(); g.moveTo(sx - 9, sy + 5); g.lineTo(sx + 9, sy + 5); g.lineTo(sx + 28, 170); g.lineTo(sx - 20, 170); g.closePath(); g.fill();
          g.restore();
        }
        // coiled phone cord from the saucer's belly down to the cow
        if (beam > 0) {
          g.save();
          g.strokeStyle = P.col('#22222c'); g.lineWidth = 1.2; g.globalAlpha = Math.min(1, beam * 1.5);
          g.beginPath();
          const x0 = sx - 2, y0 = sy + 6, x1 = cowX - 4, y1 = cowY - 12;
          const n = 34;
          for (let i = 0; i <= n; i++) {
            const k = i / n, cx = x0 + (x1 - x0) * k, cy = y0 + (y1 - y0) * k;
            const px = cx + Math.cos(k * 120) * 2.8, py = cy + Math.sin(k * 120) * 1.2;
            if (i) g.lineTo(px, py); else g.moveTo(px, py);
          }
          g.stroke();
          g.restore();
        }
        cow(g, P, cowX, cowY, lift, t);
        // the saucer
        P.glow(g, sx, sy + 3, 26, '#b8ffe0', P.dusk ? 0.35 : 0.18);
        P.blob(g, sx, sy - 3, 8, 6.5, ['#e0fcff', '#6cbcd8'], { wob: 0.3, seed: 3, n: 12, edge: '#244c68', ea: 0.5 });
        P.blob(g, sx, sy + 3, 20, 5.4, ['#eef0fa', '#8890b0', '#4a5278'], { wob: 0.4, seed: 4, n: 16, v: true, edge: '#1c2040', ea: 0.6 });
        for (let i = 0; i < 7; i++) {
          const on = (Math.floor(t * 6) + i) % 3 === 0;
          g.fillStyle = on ? P.lit('#fff070') : P.col('#c07840');
          g.fillRect(sx - 15 + i * 5, sy + 3.5 + (Math.abs(i - 3) === 3 ? -1.5 : Math.abs(i - 3) === 2 ? -0.5 : 0.5), 2, 2);
        }
        // a small, polite face in the dome
        g.fillStyle = P.lit('#2c4c28');
        g.fillRect(sx - 2.5, sy - 4, 1.4, 2); g.fillRect(sx + 1.4, sy - 4, 1.4, 2);
      },
    },
    {
      // the jackalope: ears twitch, and now and then it hops
      x: 284, y: 144, w: 34, h: 34, sample: 0,
      draw(g, t, P) {
        const hop = (t % 11) > 9.4 ? Math.abs(Math.sin(((t % 11) - 9.4) * 5.2)) * 7 : 0;
        const x = 298, y = 168 - hop;
        const ear = Math.sin(t * 7) * ((t % 5) > 4.2 ? 1.6 : 0.2);
        const brown = ['#e8c898', '#a07848'];
        // hind leg and body
        P.blob(g, x, y - 5, 7.5, 5.4, brown, { wob: 0.3, seed: 81, n: 12, edge: '#4a3020', ea: 0.4 });
        P.blob(g, x - 6, y - 1.4, 3.6, 2, '#b08850', { wob: 0.2, seed: 82, n: 8 });
        P.blob(g, x - 8, y - 6, 2.4, 2.4, '#fffaf0', { wob: 0.2, seed: 83, n: 8 });
        // head
        P.blob(g, x + 6, y - 10, 4, 3.4, brown, { wob: 0.2, seed: 84, n: 10, edge: '#4a3020', ea: 0.4 });
        // ears, long and ear-like
        P.line(g, [[x + 4, y - 12], [x + 2 + ear, y - 19], [x + 1 + ear, y - 22]], '#c89c68', 2);
        P.line(g, [[x + 6, y - 12], [x + 6 + ear, y - 19], [x + 7 + ear, y - 22]], '#c89c68', 2);
        // antlers
        const ant = P.col('#6c4426');
        g.strokeStyle = ant; g.lineWidth = 1; g.lineCap = 'round';
        g.beginPath();
        g.moveTo(x + 4.6, y - 13); g.lineTo(x + 1, y - 17); g.moveTo(x + 2.6, y - 15) ; g.lineTo(x - 1, y - 14);
        g.moveTo(x + 2.4, y - 15.6); g.lineTo(x + 0.4, y - 20);
        g.moveTo(x + 6, y - 13); g.lineTo(x + 10, y - 17); g.moveTo(x + 8.2, y - 15); g.lineTo(x + 11.6, y - 14);
        g.moveTo(x + 8.6, y - 15.8); g.lineTo(x + 10.6, y - 20);
        g.stroke();
        g.fillStyle = '#1c1008'; g.fillRect(x + 7.6, y - 11, 1.4, 1.4);
        g.fillStyle = '#e07888'; g.fillRect(x + 9.6, y - 9.4, 1.4, 1);
      },
    },
    {
      // the headset's busy light
      x: 143, y: 124, w: 8, h: 8, sample: 0,
      draw(g, t, P) {
        g.fillStyle = (Math.floor(t * 2) % 2) ? P.lit('#ff4a3a') : P.col('#5a1c18');
        g.fillRect(146, 127.5, 1.6, 1.6);
        if ((Math.floor(t * 2) % 2) && P.dusk) P.glow(g, 146.5, 128, 5, '#ff4a3a', 0.8);
      },
    },
  ];

  function cow(g, P, x, y, lift, t) {
    const legs = (dx, swing) => {
      const sw = lift * swing * Math.sin(t * 6 + dx);
      P.line(g, [[x + dx, y - 6], [x + dx + sw, y + 1]], '#f4f0e8', 1.8);
      g.fillStyle = P.col('#2c2018'); g.fillRect(x + dx + sw - 1, y + 0.2, 2.4, 1.4);
    };
    legs(-6, 1.5); legs(-3, -1.2); legs(5, 1.4); legs(8, -1.2);
    P.blob(g, x, y - 9, 11, 6.2, ['#ffffff', '#cfd2dc'], { wob: 0.4, seed: 91, n: 14, edge: '#3c3c4c', ea: 0.4 });
    P.within(g, P.ellipsePts(x, y - 9, 11, 6.2, 14), { wob: 0.4, seed: 91 }, () => {
      P.blob(g, x - 3, y - 11, 3.6, 2.6, '#2c2c34', { wob: 0.6, seed: 92, n: 8 });
      P.blob(g, x + 5, y - 7, 3, 2.4, '#2c2c34', { wob: 0.6, seed: 93, n: 8 });
    });
    P.blob(g, x - 12, y -  13, 4.2, 3.4, ['#ffffff', '#d0d4e0'], { wob: 0.3, seed: 94, n: 10, edge: '#3c3c4c', ea: 0.4 });
    P.blob(g, x - 15, y - 11.5, 2.4, 1.6, '#f0a0a8', { wob: 0.2, seed: 95, n: 8 });
    g.fillStyle = '#1c1c24'; g.fillRect(x - 13, y - 14.6, 1.2, 1.2);
    P.line(g, [[x - 11, y - 16.5], [x - 10, y - 18.5]], '#d8c898', 1.2);
    P.line(g, [[x + 11, y - 12], [x + 13, y - 7]], '#2c2c34', 1);
    P.blob(g, x + 1, y - 3.4, 3, 1.5, '#f0a0a8', { wob: 0.2, seed: 96, n: 8 });
  }

  const hotspots = [
    { id: 'sign', x: 250, y: 50, w: 60, h: 40, say: [
      'The sign reads OPEN 24 BAUD. Nobody here is in a hurry, which is lucky.',
      'The N in OPEN has a loose connection. It also has opinions.'] },
    { id: 'diner', x: 160, y: 120, w: 108, h: 50, say: [
      'A diner. The pie is rumoured to be fresh, but the rumour is itself rather old.',
      'Through the window a man is staring at a cup of coffee. The cup is winning.',
      'The menu has one page. It is a map of the nearest other diner.'] },
    { id: 'saucer', x: 185, y: 24, w: 44, h: 24, say: [
      'A flying saucer. It hovers politely, with its signal light on.',
      'The saucer is on hold with the cow. The cow does not mind. The cow was going to stay on hold anyway.'] },
    { id: 'cow', x: 192, y: 146, w: 30, h: 26, say: [
      'A cow, attached to the saucer by a telephone cord. It is a very civilised abduction.',
      'The cow regards you calmly. It has had a call waiting for years.'] },
    { id: 'cactus', x: 130, y: 114, w: 34, h: 56, say: [
      'This cactus is wearing a headset. Press 1 for needles, press 2 for more needles.',
      'Your call is important to the cactus. It will be with you shortly, in about forty years.'] },
    { id: 'jackalope', x: 284, y: 144, w: 32, h: 30, say: [
      'A jackalope. Nobody has ever been able to prove it, which is why it is standing right there.',
      'It looks at you, then at your antlers. You do not have any. It seems disappointed.'] },
    { id: 'road', x: 0, y: 178, w: 320, h: 22, say: [
      'The highway goes on forever. The diner is very clear that you should stop here instead.',
      'A tumbleweed was here. It has since left a forwarding address.'] },
  ];

  return { id: 'diner', title: 'The diner', focus: { x: 210, y: 118 }, paint, anims, hotspots };
})());
