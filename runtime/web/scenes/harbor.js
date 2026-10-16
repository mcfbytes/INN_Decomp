// Scene "harbor": a seaside village, a lighthouse that blinks like a modem, a tangled sea serpent.
// Original artwork, drawn in code.
'use strict';

InnScenes.register((() => {
  const HORIZON = 94;
  const LAMP = { x: 292, y: 47 };
  const GULL = { start: 6, period: 24, dur: 9 };

  function gullPhase(t) {
    const s = t - GULL.start;
    if (s < 0) return -1;
    const m = s % GULL.period;
    return m < GULL.dur ? m / GULL.dur : -1;
  }

  function sky(g, P) {
    g.fillStyle = P.gradRaw(g, 0, 0, 0, HORIZON + 4, P.pick(
      [[0, '#3c84d4'], [0.5, '#8ccaec'], [0.9, '#f8ecc4'], [1, '#fdddb0']],
      [[0, '#120c3c'], [0.4, '#3c2478'], [0.8, '#d0507c'], [1, '#ffb064']]));
    g.fillRect(0, 0, 320, HORIZON + 6);
    if (P.dusk) {
      P.stars(g, 0, 56, 80, 9);
      P.glow(g, 60, HORIZON, 110, '#ffb060', 0.5);
      P.blob(g, 44, 70, 7, 7, ['#fff4d0', '#ffd890'], { wob: 0.3, seed: 3, n: 14 });
      P.glow(g, 44, 70, 22, '#ffd890', 0.6);
    } else {
      P.glow(g, 70, 30, 90, '#fff8d0', 0.55);
      P.glow(g, 70, 30, 26, '#ffffff', 0.65);
    }
  }

  function sea(g, P) {
    g.fillStyle = P.gradRaw(g, 0, HORIZON, 0, 164, P.pick(
      [[0, '#a8dcec'], [0.25, '#5cb4d8'], [1, '#1c6498']],
      [[0, '#f0905c'], [0.18, '#a8487c'], [0.55, '#3c2a78'], [1, '#160e48']]));
    g.fillRect(0, HORIZON, 320, 78);
    // wash streaks, flatter toward the horizon
    const r = P.mulberry(7);
    for (let i = 0; i < 150; i++) {
      const y = HORIZON + 2 + Math.pow(r(), 1.5) * 68, x = r() * 330 - 10, len = 4 + (y - HORIZON) * 0.4 * r() * 1.6;
      g.fillStyle = P.alpha(r() < 0.5 ? P.pick('#e8fcff', '#ffa080') : P.pick('#0c4c7c', '#0c0830'), P.dusk ? 0.22 : 0.2);
      g.fillRect(x, y, len, 1);
    }
    if (P.dusk) {
      // the sunset's path across the water
      g.save();
      g.globalCompositeOperation = 'lighter';
      g.fillStyle = P.gradRaw(g, 0, HORIZON, 0, 150, [[0, P.alpha('#ff9a50', 0.55)], [1, P.alpha('#ff9a50', 0)]]);
      g.beginPath(); g.moveTo(40, HORIZON); g.lineTo(80, HORIZON); g.lineTo(130, 162); g.lineTo(0, 162); g.closePath(); g.fill();
      g.restore();
    }
  }

  function house(g, P, x, y, w, h, wall, roof, seed, tilt = 0) {
    const body = [[x, y - h], [x + w, y - h + tilt], [x + w, y], [x, y]];
    P.shape(g, body, wall, { wob: 0.7, seed, edge: '#3c2418', ea: 0.3 });
    P.within(g, body, { wob: 0.7, seed }, () => {
      g.fillStyle = P.alpha(P.pick('#2c1c28', '#0c0820'), 0.2);
      g.fillRect(x + w * 0.64, y - h - 2, w * 0.4, h + 3);
      P.dabs(g, x, y - h, w, h, ['#ffffff', '#6c4c4c'], 12, 1.5, seed, 0.2);
    });
    const ov = 2.6, apex = [x + w * 0.46, y - h - h * 0.62 + tilt * 0.4];
    const rf = [[x - ov, y - h + 1], apex, [x + w + ov, y - h + tilt + 1]];
    P.shape(g, rf, roof, { wob: 0.8, seed: seed + 1, edge: '#1c0c10', ea: 0.4 });
    P.within(g, rf, { wob: 0.8, seed: seed + 1 }, () => {
      g.fillStyle = P.alpha('#10080c', 0.28);
      g.beginPath(); g.moveTo(apex[0], apex[1]); g.lineTo(x + w + ov, y - h + tilt + 1); g.lineTo(apex[0] + 1, y - h + 1); g.closePath(); g.fill();
    });
    P.rect(g, x + w * 0.66, apex[1] - 3, 3, 7, '#7c4c3c', { wob: 0.3, seed });
    for (const wx of w > 14 ? [x + 3, x + w - 7] : [x + w / 2 - 2]) {
      if (P.dusk) { g.fillStyle = P.lit('#ffd24a'); g.fillRect(wx, y - h + 3, 4, 4); P.glow(g, wx + 2, y - h + 5, 8, '#ffb030', 0.45); }
      else { g.fillStyle = P.col('#9cc4dc'); g.fillRect(wx, y - h + 3, 4, 4); }
      g.fillStyle = P.col('#3c2418'); g.fillRect(wx + 1.5, y - h + 3, 1, 4);
    }
    P.rect(g, x + w * 0.4, y - 6.4, 4, 6.4, '#3c2418', { wob: 0.2, seed });
  }

  function village(g, P) {
    // the hill the houses climb
    const hill = [[204, 162], [212, 136], [228, 116], [248, 102], [270, 96], [300, 92], [320, 94], [320, 162]];
    P.shape(g, hill, ['#c8c470', '#7c9c44', '#456c30'], { wob: 1.6, seed: 3, v: true });
    P.within(g, hill, { wob: 1.6, seed: 3 }, () => {
      P.dabs(g, 204, 92, 116, 70, ['#e8d478', '#3c6428', '#a8bc54', '#d8a048'], 90, 3, 5, 0.38);
    });
    const rock = [[258, 162], [264, 134], [276, 108], [288, 99], [304, 96], [320, 98], [320, 164]];
    P.shape(g, rock, ['#b0a4b4', '#6c6078', '#3c3450'], { wob: 1.4, seed: 4, edge: '#1c1830', ea: 0.4 });
    P.within(g, rock, { wob: 1.4, seed: 4 }, () => {
      P.dabs(g, 258, 96, 62, 66, ['#d8ccd4', '#4c4260', '#847890'], 50, 3, 6, 0.35);
      P.dabs(g, 258, 96, 62, 66, ['#6c9c48'], 12, 2, 7, 0.55);
    });
    // houses, the far ones first
    house(g, P, 228, 112, 12, 9, ['#f6e8b8', '#d0b87c'], ['#b8503c', '#7c2820'], 17);
    house(g, P, 244, 108, 13, 10, ['#9cd4c8', '#5c9c98'], ['#c8583c', '#8c2c24'], 15, -1);
    house(g, P, 216, 124, 14, 10, ['#f2a8a0', '#c8706c'], ['#7c4c6c', '#4c2c48'], 13);
    house(g, P, 236, 124, 15, 11, ['#fcd0a0', '#d49464'], ['#5c4ca0', '#342868'], 19, 1);
    house(g, P, 256, 118, 12, 9, ['#b0c8f0', '#7894c4'], ['#a8483c', '#6c2420'], 23, -1);
    house(g, P, 208, 138, 16, 11, ['#f6d89c', '#d0a468'], ['#4c88a8', '#2c5878'], 11, 1);
    house(g, P, 230, 140, 17, 12, ['#f6ecc4', '#c8b884'], ['#cc7c3c', '#8c4020'], 21);
    house(g, P, 252, 136, 15, 11, ['#f4b8a8', '#c47c70'], ['#3c6c8c', '#244858'], 25);
    P.canopy(g, 226, 134, 5, 5, ['#2c5c34', '#58963c', '#9cc858'].map((c) => P.col(c)), 31);
    P.canopy(g, 270, 128, 5, 5, ['#2c5c34', '#58963c', '#9cc858'].map((c) => P.col(c)), 33);
    // a jetty running out from the village, on stilts
    P.shape(g, [[130, 150], [208, 148], [208, 153], [130, 155]], ['#c8a070', '#785030'], { wob: 0.5, seed: 35, edge: '#2c1408', ea: 0.6 });
    for (let x = 140; x < 208; x += 14) P.rect(g, x, 153, 2, 8, '#4c3420', { wob: 0.2, seed: x });
    // laundry between two houses
    P.wire(g, [214, 128], [232, 126], 2.5, P.pick('#4c3c30', '#14102c'), 0.8);
    for (const [x, c] of [[218, '#f8f0e0'], [222, '#e8484c'], [227, '#f8f0e0']]) P.rect(g, x, 127.5, 2.6, 3.4, c, { wob: 0.2, seed: x });
  }

  function lighthouse(g, P) {
    const cx = 292, top = 56, base = 138;
    const hw = (y) => 6.5 + (y - top) * 0.075;
    P.shadow(g, cx + 2, base + 1, 16, 3, 0.3);
    const pts = [[cx - hw(base), base], [cx - hw(top), top], [cx + hw(top), top], [cx + hw(base), base]];
    P.shape(g, pts, ['#fffaf0', '#d8ccc4', '#8c80a0'], { wob: 0.5, seed: 41, edge: '#2c2440', ea: 0.5 });
    P.within(g, pts, { wob: 0.5, seed: 41 }, () => {
      for (let i = 0; i < 4; i++) {
        const y0 = top + 8 + i * 19;
        g.fillStyle = P.col('#d8382c'); g.fillRect(cx - 20, y0, 40, 9);
        g.fillStyle = P.alpha('#ffffff', 0.25); g.fillRect(cx - 20, y0, 40, 1.4);
      }
      g.fillStyle = P.alpha(P.pick('#2c2040', '#0c0620'), 0.34);
      g.fillRect(cx + 2, top, 14, base - top);
      P.dabs(g, cx - 10, top, 20, base - top, ['#8c80a0'], 14, 1.4, 5, 0.2);
    });
    // a door and a porthole
    P.shape(g, [[cx - 2.5, base], [cx - 2.5, base - 7], [cx, base - 9], [cx + 2.5, base - 7], [cx + 2.5, base]], '#3c2418', { wob: 0.3, seed: 42 });
    P.blob(g, cx, top + 24, 1.8, 1.8, P.dusk ? P.lit('#ffd24a') : P.col('#9cc4dc'), { wob: 0.1, seed: 5, n: 8, edge: '#2c2018', ea: 0.9 });
    // gallery
    P.shape(g, [[cx - 12, top + 0.5], [cx + 12, top + 0.5], [cx + 11, top + 3.5], [cx - 11, top + 3.5]], ['#5c5470', '#2c2440'], { wob: 0.3, seed: 43 });
    for (let x = cx - 10; x <= cx + 10; x += 4) P.rect(g, x, top - 4, 0.9, 4.4, '#2c2440', { wob: 0.1 });
    P.rect(g, cx - 11, top - 4.4, 22, 1, '#2c2440', { wob: 0.2, seed: 44 });
    // lamp room, glass and dome
    P.shape(g, [[cx - 7, top - 4], [cx - 7, top - 14], [cx + 7, top - 14], [cx + 7, top - 4]], P.dusk ? P.lit('#2c2848') : P.col('#c4e0ec'), { wob: 0.3, seed: 45, edge: '#2c2440', ea: 0.9 });
    P.shape(g, [[cx - 9, top - 14], [cx - 5, top - 20], [cx, top - 23], [cx + 5, top - 20], [cx + 9, top - 14]], ['#e8584c', '#8c1c1c'], { wob: 0.4, seed: 46, edge: '#2c0c0c', ea: 0.5 });
    P.line(g, [[cx, top - 23], [cx, top - 29]], '#2c2440', 1);
    P.blob(g, cx, top - 29.6, 1.4, 1.4, '#ffd23c', { wob: 0.1, seed: 4, n: 8 });
  }

  function dock(g, P) {
    // the stone quay wall, then planks in front of it
    P.shape(g, [[-4, 160], [324, 160], [324, 174], [-4, 174]], ['#b8aca4', '#7c7088', '#4c4466'], { wob: 0.8, seed: 51, v: true, edge: '#241c38', ea: 0.45 });
    P.within(g, [[-4, 160], [324, 160], [324, 174], [-4, 174]], { wob: 0.8, seed: 51 }, () => {
      for (let y = 162, r = 0; y < 174; y += 6, r++) {
        g.fillStyle = P.alpha(P.pick('#2c2040', '#0c0620'), 0.3); g.fillRect(0, y, 320, 1);
        for (let x = (r % 2) * 9; x < 320; x += 18) g.fillRect(x, y, 1, 6);
      }
      P.dabs(g, 0, 160, 320, 14, ['#d4c8c0', '#4c4260', '#5c8c4c'], 60, 2.4, 52, 0.28);
    });
    P.shape(g, [[-4, 172], [324, 172], [324, 202], [-4, 202]], ['#c89c64', '#8c6440', '#5c3c28'], { wob: 0.6, seed: 53, v: true, edge: '#241408', ea: 0.5 });
    P.within(g, [[-4, 172], [324, 172], [324, 202], [-4, 202]], { wob: 0.6, seed: 53 }, () => {
      for (let y = 172, i = 0; y < 202; y += 8, i++) {
        g.fillStyle = P.alpha('#241408', 0.5); g.fillRect(0, y, 320, 1);
        g.fillStyle = P.alpha('#fff0c8', 0.18); g.fillRect(0, y + 1, 320, 1);
        for (let x = (i * 53) % 70; x < 330; x += 70) g.fillRect(x, y, 1, 8);
      }
      P.dabs(g, 0, 172, 320, 30, ['#e8c890', '#6c4830', '#a88050'], 70, 2.2, 54, 0.3);
    });
    // bollards, barrels, a coil of rope, a crate and a pot
    const post = (x, y) => {
      P.shape(g, [[x - 3.5, y], [x - 3, y - 8], [x - 4.5, y - 9], [x + 4.5, y - 9], [x + 3, y - 8], [x + 3.5, y]], ['#6c6478', '#2c2640'], { wob: 0.4, seed: x, edge: '#10101c', ea: 0.5 });
    };
    post(60, 184); post(178, 186); post(300, 190);
    const barrel = (x, y, s, seed) => {
      P.shape(g, [[x - 6 * s, y], [x - 7 * s, y - 7 * s], [x - 6 * s, y - 14 * s], [x + 6 * s, y - 14 * s], [x + 7 * s, y - 7 * s], [x + 6 * s, y]], ['#c8884c', '#7c4c28'], { wob: 0.4, seed, edge: '#2c1408', ea: 0.6 });
      for (const k of [3, 7, 11]) { g.fillStyle = P.col('#3c2c28'); g.fillRect(x - 6.4 * s, y - k * s, 12.8 * s, 1.2); }
      P.blob(g, x, y - 14 * s, 6 * s, 1.8 * s, '#5c3c20', { wob: 0.2, seed: seed + 1, n: 10 });
    };
    barrel(120, 196, 1.2, 61); barrel(138, 198, 1.1, 62);
    P.text(g, 'SIGNAL', 108, 188, '#f6e8c8', 1, 0);
    // coil of rope
    P.blob(g, 36, 194, 12, 4, ['#e8d098', '#8c6c40'], { wob: 0.4, seed: 63, n: 12, edge: '#3c2410', ea: 0.5 });
    P.blob(g, 36, 193, 6, 2, '#6c4c28', { wob: 0.2, seed: 64, n: 8 });
    // lobster pot
    P.shape(g, [[196, 198], [194, 189], [202, 185], [212, 185], [218, 189], [216, 198]], ['#c8a070', '#785030'], { wob: 0.5, seed: 65, edge: '#2c1408', ea: 0.6 });
    for (const x of [200, 205, 210]) P.line(g, [[x, 186], [x - 1, 198]], P.alpha('#2c1408', 0.7), 0.8);
  }

  // A crate on the quay, stencilled for the disks it holds. Two floppies poke out under the lid.
  function diskCrate(g, P) {
    const x = 54, w = 44, top = 158, bot = 184;
    P.shadow(g, x + w / 2 + 2, bot + 1, 28, 3.6, 0.38);
    g.save(); g.translate(x + 12, top + 1); g.rotate(-0.22); P.floppy(g, -6, -14, 12, '#d8382c'); g.restore();
    g.save(); g.translate(x + 22, top + 1); g.rotate(0.14); P.floppy(g, -6, -15, 12, '#2c5cc8'); g.restore();
    const front = [[x, top], [x + w, top], [x + w + 1, bot], [x - 1, bot]];
    P.shape(g, front, ['#d8a860', '#8c5c2c'], { wob: 0.5, seed: 71, v: true, edge: '#2c1a0c', ea: 0.8 });
    P.within(g, front, { wob: 0.5, seed: 71 }, () => {
      g.fillStyle = P.alpha(P.col('#2c1a0c'), 0.55);
      g.fillRect(x, top + 8, w, 1); g.fillRect(x, top + 17, w, 1);
      g.fillRect(x + 3, top, 2, 26); g.fillRect(x + w - 5, top, 2, 26);
      P.dabs(g, x, top, w, 26, ['#f0d49c', '#4a2c14'], 14, 1.3, 72, 0.3);
    });
    P.shape(g, [[x - 2, top - 3], [x + w + 2, top - 3], [x + w + 2, top + 1], [x - 2, top + 1]], ['#e8bc78', '#a06c34'], { wob: 0.3, seed: 73, edge: '#2c1a0c', ea: 0.8 });
    P.text(g, 'DISKS', x + (w - 29) / 2 + 0.5, top + 10, '#f6f0dc', 1, 1);
    for (const ax of [x + 12, x + w - 12]) {
      g.fillStyle = P.col('#f6f0dc');
      g.beginPath(); g.moveTo(ax, top + 18); g.lineTo(ax + 4, top + 23); g.lineTo(ax - 4, top + 23); g.closePath(); g.fill();
      g.fillRect(ax - 1, top + 23, 2, 3);
    }
  }

  // The lighthouse keeper's telephone, screwed to the tower beside the door.
  function lighthousePhone(g, P) {
    P.wallPhone(g, 294, 106, 1.3);
  }

  function paint(g, P) {
    sky(g, P);
    sea(g, P);
    // far shore, a low blue ridge
    P.ridge(g, { y: HORIZON - 2, amp: 2, freq: 0.05, seed: 6, x0: 0, x1: 120, bottom: HORIZON + 1, cols: ['#a8b8d8', '#b8c4dc'] });
    village(g, P);
    lighthouse(g, P);
    dock(g, P);
    diskCrate(g, P);
    lighthousePhone(g, P);
    if (P.dusk) P.glow(g, 210, 168, 90, '#ffa060', 0.14);
  }

  // ---------- animation ----------

  const anims = [
    {
      x: 0, y: 6, w: 320, h: 56, sample: 14,
      draw(g, t, P) {
        for (const [x0, y, s, seed] of [[10, 22, 1.7, 3], [110, 34, 1.2, 5], [200, 14, 1.9, 8], [60, 52, 1.0, 11]]) {
          const x = ((x0 + t * (1.2 + seed * 0.12) + 60) % 400) - 60;
          P.cloud(g, x, y, s, seed);
        }
      },
    },
    {
      // sea glints
      x: 0, y: HORIZON, w: 250, h: 66, sample: 1,
      draw(g, t, P) {
        const r = P.mulberry(31);
        for (let i = 0; i < 60; i++) {
          const x = r() * 250, y = HORIZON + 1 + Math.pow(r(), 1.4) * 62, ph = r() * 9, sp = 0.6 + r();
          const k = Math.sin(t * sp + ph);
          if (k < 0.55) continue;
          g.fillStyle = P.alpha(P.pick('#ffffff', '#ffc890'), (k - 0.5) * 1.4);
          g.fillRect(x, y, 2 + (y - HORIZON) * 0.06, 1);
        }
      },
    },
    {
      // the lighthouse: TX green, then RX amber, then both, with a beam that is really a blinking LED
      x: 90, y: 6, w: 220, h: 80, sample: 1,
      draw(g, t, P) {
        const frames = [1, 1, 0, 1, 0, 0, 2, 0, 3, 3, 0, 1, 0];       // 1 = TX green, 2 = RX amber, 3 = both
        const f = frames[Math.floor(t * 3) % frames.length];
        const tx = f === 1 || f === 3, rx = f === 2 || f === 3;
        const a = P.dusk ? 0.5 : 0.22;
        const wedge = (hex, dy, alpha) => {
          g.save();
          g.globalCompositeOperation = 'lighter';
          g.fillStyle = P.gradRaw(g, LAMP.x, 0, 100, 0, [[0, P.alpha(hex, alpha)], [1, P.alpha(hex, 0)]]);
          g.beginPath(); g.moveTo(LAMP.x - 2, LAMP.y - 1); g.lineTo(90, LAMP.y - 34 + dy); g.lineTo(90, LAMP.y + 26 + dy); g.lineTo(LAMP.x - 2, LAMP.y + 3); g.closePath(); g.fill();
          g.restore();
        };
        if (tx) wedge('#5cff8c', -6, a);
        if (rx) wedge('#ffb840', 8, a);
        g.fillStyle = tx ? P.lit('#5cff8c') : P.col('#2c5c3c');
        g.fillRect(LAMP.x - 5, LAMP.y - 1, 4, 4);
        g.fillStyle = rx ? P.lit('#ffb840') : P.col('#6c4c20');
        g.fillRect(LAMP.x + 1, LAMP.y - 1, 4, 4);
        if (tx) P.glow(g, LAMP.x - 3, LAMP.y + 1, 18, '#5cff8c', 0.7);
        if (rx) P.glow(g, LAMP.x + 3, LAMP.y + 1, 18, '#ffb840', 0.7);
      },
    },
    {
      // the sea serpent, wound up in the undersea cable
      x: 126, y: 104, w: 100, h: 54, sample: 5,
      draw(g, t, P) {
        const wl = 140;
        const tug = (t % 20) > 16 ? Math.sin(((t % 20) - 16) / 4 * Math.PI) : 0;
        const green = ['#9ce47c', '#3c9c58', '#1c5c48'];
        const humps = [[142, 7], [160, 9], [178, 8]];
        // body, humps arching out of the water
        humps.forEach(([hx, hh], i) => {
          const bob = Math.sin(t * 1.3 + i * 1.5) * 1.4;
          P.shape(g, [[hx - 9, wl + 2], [hx - 8, wl - hh * 0.5 + bob], [hx - 3, wl - hh + bob], [hx + 3, wl - hh + bob], [hx + 8, wl - hh * 0.5 + bob], [hx + 9, wl + 2]], green, { wob: 0.4, seed: 70 + i, v: true, edge: '#0c2c28', ea: 0.5 });
          g.fillStyle = P.alpha('#ffd868', 0.55);
          for (let k = -1; k <= 1; k++) g.fillRect(hx + k * 4 - 0.5, wl - hh + 1 + bob + Math.abs(k), 1.4, 3);
        });
        // neck rising on the right and the confused head
        const hx = 196 - tug * 6, hy = 118 - tug * 7 + Math.sin(t * 1.6) * 1.2;
        const neck = [[hx + 4, hy + 8], [193, 132], [193, wl + 2], [207, wl + 2], [207, 132], [hx + 10, hy + 6]];
        P.shape(g, neck, green, { wob: 0.5, seed: 78, v: true, edge: '#0c2c28', ea: 0.5 });
        P.shape(g, [[hx - 8, hy + 2], [hx - 2, hy - 4], [hx + 8, hy - 3], [hx + 12, hy + 4], [hx + 6, hy + 9], [hx - 4, hy + 8]], green, { wob: 0.4, seed: 79, edge: '#0c2c28', ea: 0.55 });
        P.blob(g, hx - 5, hy + 3, 3.4, 2.2, '#f0b8a0', { wob: 0.2, seed: 80, n: 8 });
        // the cable: black bands wound round the neck, a loop at the head, and a line down into the water
        g.strokeStyle = P.col('#1c1820'); g.lineWidth = 1.8; g.lineCap = 'round';
        for (let k = 0; k < 4; k++) {
          const yy = hy + 10 + k * 5;
          g.beginPath(); g.moveTo(193, yy); g.lineTo(207, yy + 3); g.stroke();
        }
        g.beginPath(); g.ellipse(hx + 4, hy + 1, 7, 4.2, 0.4, 0, Math.PI * 2); g.stroke();
        g.beginPath(); g.moveTo(207, wl - 2); g.quadraticCurveTo(216, wl + 3, 224, wl + 6); g.stroke();
        // eyes: one open, one swirling
        g.fillStyle = '#ffffff'; g.fillRect(hx - 1, hy - 1, 3.6, 3.6); g.fillRect(hx + 5, hy - 1, 3.6, 3.6);
        g.fillStyle = '#10181c'; g.fillRect(hx + 0.4 + Math.sin(t * 2.4) * 0.8, hy, 1.6, 1.8);
        g.strokeStyle = '#10181c'; g.lineWidth = 0.9;
        g.beginPath(); g.arc(hx + 6.8, hy + 0.8, 1.2, 0, 5); g.stroke();
        // foam and ripples where the body meets the water
        g.strokeStyle = P.alpha(P.pick('#ffffff', '#ffb890'), 0.7); g.lineWidth = 1;
        for (const [rx, rw] of [[142, 12], [160, 12], [178, 12], [200, 12]]) {
          const w = rw + Math.sin(t * 2 + rx) * 1.6;
          g.beginPath(); g.ellipse(rx, wl + 2.6, w, 2.2, 0, 0, Math.PI * 2); g.stroke();
        }
        // taut cable with a puff when it yanks him back
        if (tug > 0.5) {
          g.fillStyle = P.lit('#fff6a0');
          for (const [dx, dy] of [[-3, -8], [6, -10], [-9, -3]]) { g.fillRect(hx + dx, hy + dy, 2, 0.8); g.fillRect(hx + dx + 0.6, hy + dy - 0.8, 0.8, 2.4); }
        }
      },
    },
    {
      // a rowing boat, rocking at its mooring
      x: 148, y: 100, w: 36, h: 24, sample: 0,
      draw(g, t, P) {
        const x = 166, y = 114 + Math.sin(t * 1.1) * 0.8, rot = Math.sin(t * 1.4) * 0.05;
        g.save(); g.translate(x, y); g.rotate(rot);
        P.shape(g, [[-12, -3], [12, -3], [8, 4], [-8, 4]], ['#ffe8c0', '#b0683c', '#6c3c24'], { wob: 0.3, seed: 85, v: true, edge: '#2c1408', ea: 0.6 });
        g.fillStyle = P.col('#e8484c'); g.fillRect(-11, -2, 22, 1.4);
        P.line(g, [[0, -3], [0, -14]], '#4c3420', 1);
        P.shape(g, [[1, -14], [1, -5], [9, -5]], ['#fffaf0', '#c8c0c4'], { wob: 0.3, seed: 86, edge: '#3c3448', ea: 0.5 });
        g.restore();
      },
    },
    {
      // the fisherman, his bucket, and the floppy disk the gull wants
      x: 196, y: 140, w: 80, h: 48, sample: 0,
      draw(g, t, P) {
        const gp = gullPhase(t);
        const x = 246, y = 180;
        const fist = gp > 0.2 && gp < 0.6 ? Math.sin(t * 14) * 2 : 0;
        P.shadow(g, x, y + 1, 14, 2.4, 0.3);
        // rod and line
        const tip = [210 + Math.sin(t * 1.2) * 0.6, 150];
        P.line(g, [[x - 8, y - 12], [tip[0], tip[1]]], '#5c3c20', 1);
        g.strokeStyle = P.alpha(P.pick('#ffffff', '#ffe8c8'), 0.8); g.lineWidth = 0.7;
        g.beginPath(); g.moveTo(tip[0], tip[1]); g.lineTo(tip[0] - 1, 156); g.stroke();
        // body: a sitting fisherman in a stripy jersey
        P.shape(g, [[x - 6, y], [x - 7, y - 11], [x + 5, y - 12], [x + 7, y]], ['#5c7cc0', '#2c3c78'], { wob: 0.5, seed: 87, edge: '#10183c', ea: 0.5 });
        g.fillStyle = P.col('#f4f0e0');
        for (const k of [3, 6, 9]) g.fillRect(x - 6, y - k, 12, 1.4);
        P.blob(g, x, y - 16, 3.8, 4, '#f0c8a0', { wob: 0.2, seed: 88, n: 10 });
        P.shape(g, [[x - 5, y - 17], [x - 3, y - 22], [x + 3, y - 22], [x + 5, y - 17]], ['#f6d03c', '#c89818'], { wob: 0.3, seed: 89 });
        P.shape(g, [[x - 7, y - 17], [x + 7, y - 17], [x + 6, y - 15.6], [x - 6, y - 15.6]], '#c89818', { wob: 0.2, seed: 90 });
        P.rect(g, x + 2.4, y - 14, 5, 1.2, '#4c3420', { wob: 0.1 });
        // arm: holds the rod, or shakes a fist at the sky
        P.line(g, [[x - 4, y - 10], [x - 8, y - 12]], '#5c7cc0', 2.4);
        if (fist) { P.line(g, [[x + 4, y - 10], [x + 8, y - 17 + fist]], '#5c7cc0', 2.4); P.blob(g, x + 8.6, y - 18 + fist, 1.6, 1.6, '#f0c8a0', { wob: 0.1, seed: 5, n: 8 }); }
        else P.line(g, [[x + 4, y - 10], [x + 8, y - 6]], '#5c7cc0', 2.4);
        // bucket with a fish tail, and a floppy disk on top (until the gull takes it)
        P.shape(g, [[x + 14, y], [x + 13, y - 8], [x + 25, y - 8], [x + 24, y]], ['#a8b0c0', '#585c78'], { wob: 0.3, seed: 91, edge: '#1c1c30', ea: 0.6 });
        P.shape(g, [[x + 17, y - 8], [x + 21, y - 13], [x + 23, y - 8]], '#e8884c', { wob: 0.2, seed: 92 });
        if (gp < 0 || gp < 0.15) floppy(g, P, x + 14.5, y - 13.5, 0);
      },
    },
    {
      // the gull, who has seen the floppy disk
      gag: GULL, x: 0, y: 14, w: 320, h: 160, sample: 0,
      draw(g, t, P, u) {
        if (u === undefined) u = 0.5;
        const swoop = 0.16;
        let x, y;
        if (u < swoop) { const k = u / swoop; x = 330 - k * 52; y = 40 + k * k * 110; }
        else { const k = (u - swoop) / (1 - swoop); x = 278 - k * 310; y = 150 - Math.sin(Math.min(1, k * 1.5) * Math.PI / 2) * 100 + Math.sin(k * 12) * 2; }
        const flap = Math.sin(t * 16);
        const carrying = u >= swoop;
        // wings
        g.fillStyle = P.pick('#ffffff', '#e0d8f0');
        const wy = flap * 5;
        g.beginPath(); g.moveTo(x, y); g.quadraticCurveTo(x - 7, y - 6 - wy, x - 14, y - 2 - wy); g.quadraticCurveTo(x - 7, y - 1, x, y + 1); g.fill();
        g.beginPath(); g.moveTo(x, y); g.quadraticCurveTo(x + 6, y - 6 - wy, x + 12, y - 3 - wy); g.quadraticCurveTo(x + 6, y - 1, x, y + 1); g.fill();
        P.blob(g, x, y + 1.4, 4.6, 2.4, ['#ffffff', '#b8b8d0'], { wob: 0.2, seed: 1, n: 10 });
        P.blob(g, x - 4.6, y - 0.4, 2, 1.8, '#ffffff', { wob: 0.1, seed: 2, n: 8 });
        P.shape(g, [[x - 6, y - 0.6], [x - 9.6, y + 0.6], [x - 6, y + 1.4]], P.lit('#ffb030'), { wob: 0.1, seed: 3 });
        g.fillStyle = '#10101c'; g.fillRect(x - 5.2, y - 1.2, 1, 1);
        P.shape(g, [[x + 4, y + 1], [x + 8, y + 0.4], [x + 8, y + 2.6], [x + 4, y + 2.6]], '#b8b8d0', { wob: 0.1, seed: 4 });
        if (carrying) floppy(g, P, x - 12, y - 1, Math.sin(t * 5) * 0.3);
      },
    },
  ];

  function floppy(g, P, x, y, rot) {
    g.save();
    g.translate(x + 3.5, y + 3.5); g.rotate(rot);
    g.fillStyle = P.pick('#1c2c5c', '#3c4c8c'); g.fillRect(-3.5, -3.5, 7, 7);
    g.fillStyle = P.pick('#c8ccd8', '#c8ccd8'); g.fillRect(-2, -3.5, 4, 2.6);
    g.fillStyle = '#10101c'; g.fillRect(0.6, -3, 1, 1.8);
    g.fillStyle = P.pick('#f4f0e0', '#f4f0e0'); g.fillRect(-2.6, 0.6, 5.2, 2.8);
    g.fillStyle = P.pick('#d8302c', '#d8302c'); g.fillRect(-2, 1.4, 3, 0.8);
    g.restore();
  }

  const hotspots = [
    { id: 'lighthouse', x: 272, y: 22, w: 44, h: 116, say: [
      'The lighthouse blinks green, then amber, then green. Ships have learned to read it as "busy".',
      'Its beam is a very large status light. It is permanently on "Connected" and permanently smug about it.',
      'The lamp room has two lights, one for each direction. They are never in agreement for long.'] },
    { id: 'serpent', x: 134, y: 108, w: 80, h: 38, say: [
      'A sea serpent, wound up in the undersea cable. It is not trying to escape. It is trying to find the end.',
      'The serpent looks at you, then at the cable, then at you again. Neither of you has a plan.',
      'It has the particular expression of someone who just pulled the wrong plug.'] },
    { id: 'fisherman', x: 232, y: 152, w: 40, h: 40, say: [
      'The fisherman is fishing for compliments. He has been at it since breakfast and has a nibble.',
      'He tips his hat to you without turning around. This takes skill.'] },
    { id: 'bucket', x: 258, y: 168, w: 14, h: 14, say: [
      'A bucket with a fish tail and a floppy disk. Both are going to be missed by someone.'] },
    { id: 'village', x: 204, y: 100, w: 60, h: 52, say: [
      'A fishing village. Everybody knows everybody else\'s business, and the cable explains why.',
      'The pink house has a laundry line with exactly one sock on it. Nobody comments.'] },
    { id: 'boat', x: 150, y: 100, w: 34, h: 18, say: [
      'A rowing boat named "Dial". It is going nowhere in particular, patiently.'] },
    { id: 'barrels', x: 104, y: 178, w: 44, h: 24, say: [
      'A barrel marked "SIGNAL". You decide not to ask what is in it. It sloshes.'] },
    { id: 'rope', x: 22, y: 186, w: 28, h: 14, say: [
      'A coil of rope. It has been waiting here so long it has become a hat for the dock.'] },
    { id: 'sea', x: 0, y: 96, w: 132, h: 62, say: [
      'The sea goes on a long way. Somewhere out there is the other end of the cable.'] },
    { id: 'sky', x: 0, y: 0, w: 260, h: 90, say: [
      'A gull wheels overhead, watching the bucket with professional interest.',
      'Clouds drift by. None of them has a forwarding address either.'] },
  ];

  hotspots.push(
    { id: 'crate', role: 'disks', x: 50, y: 142, w: 54, h: 46, say: ['A crate marked DISKS. The arrows say which way is up, and they are firm about it.'] },
    { id: 'keeperphone', role: 'phone', x: 290, y: 104, w: 22, h: 28, say: ['The lighthouse has a telephone. It rings only when somebody out at sea feels lonely.'] });

  const objects = {
    disks: {
      open: 'You prise up the lid of the crate. Floppies, packed in straw, and not a single herring.',
      got: 'The keeper will take your call. Try the telephone on the lighthouse.',
      again: 'The crate rattles. THIS SIDE UP, it reminds you.',
      hint: 'Something is glinting on the quay beside the crate marked DISKS. It might be the crate.',
    },
    phone: {
      refuse: 'You pick up the receiver. A foghorn voice says: "Disks first, please."',
      answer: 'Lighthouse. Mind the rocks.',
      hint: 'The telephone on the lighthouse is twinkling. The lamp pretends not to be jealous.',
      busy: 'The lamp swings round and blinks twice. Busy.',
    },
  };

  return { id: 'harbor', title: 'The harbour', focus: { x: 245, y: 110 }, paint, anims, hotspots, objects,
    termAt: [196, 40],
    spark: { disks: [48, 138, 58, 50], phone: [286, 102, 30, 32] } };
})());
