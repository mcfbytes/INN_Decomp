// Scene "valley": a small autumn town under a grumbling volcano, strung together by a telephone line.
// Original artwork, drawn in code. Registers itself with InnScenes (scene.js).
'use strict';

InnScenes.register((() => {
  const AUTUMN = {
    orange: ['#8a3a10', '#d8641c', '#f6a84a'], red: ['#6c1a14', '#b02c1c', '#ec6840'],
    gold: ['#7a5a10', '#d0a028', '#f6e078'], pine: ['#10301e', '#1e4a2c', '#3c7040'],
  };

  function sky(g, P) {
    g.fillStyle = P.gradRaw(g, 0, 0, 0, 125, P.pick(
      [[0, '#4a92dc'], [0.4, '#8cc4ee'], [0.78, '#f4e6bc'], [1, '#fbd49a']],
      [[0, '#15123e'], [0.4, '#3c2870'], [0.75, '#c8587a'], [1, '#f6a05a']]));
    g.fillRect(0, 0, 320, 125);
    if (P.dusk) {
      P.grain(g, 0, 0, 320, 70, '#ffffff', 90, 4, 0.9);
      P.grain(g, 0, 0, 320, 50, '#ffffff', 40, 8, 1);
      // moon
      P.glow(g, 92, 36, 40, '#c8c0ff', 0.3);
      P.blob(g, 92, 36, 9, 9, ['#fffbe8', '#d4cca8'], { wob: 0.4, seed: 2, n: 16 });
      P.dabs(g, 86, 30, 10, 10, ['#b8b090'], 6, 1.4, 5, 0.5);
    } else {
      P.glow(g, 96, 56, 90, '#fff6c8', 0.55);
      P.glow(g, 96, 56, 34, '#ffffff', 0.5);
    }
  }

  function volcano(g, P) {
    // far range
    P.ridge(g, { y: 104, amp: 9, freq: 0.03, seed: 4, bottom: 130, cols: ['#a2b0da', '#c4c8dc'] });
    P.ridge(g, { y: 108, amp: 6, freq: 0.05, seed: 9, bottom: 130, x0: -4, x1: 160, cols: ['#8c9ccc', '#bcc4d8'] });
    const body = [[168, 124], [196, 94], [214, 72], [226, 56], [236, 52], [247, 57], [258, 74], [276, 98], [312, 124]];
    P.shape(g, body, ['#b08a94', '#7c5a6c'], { wob: 1.4, seed: 3 });
    P.within(g, body, { wob: 1.4, seed: 3 }, () => {
      // shaded right flank, lit left flank
      g.fillStyle = P.alpha('#2c1a3c', P.dusk ? 0.5 : 0.5);
      g.beginPath(); g.moveTo(236, 51); g.lineTo(260, 76); g.lineTo(316, 126); g.lineTo(244, 126); g.lineTo(240, 90); g.closePath(); g.fill();
      P.dabs(g, 170, 56, 150, 70, ['#d8b8a0', '#a08090', '#8a6a7c'], 70, 4, 21, 0.28);
      // gullies
      for (const [x0, y0, x1, y1] of [[226, 60, 212, 112], [233, 56, 232, 118], [243, 60, 256, 116], [220, 70, 196, 118], [252, 68, 280, 118]])
        P.line(g, [[x0, y0], [(x0 + x1) / 2 + 3, (y0 + y1) / 2], [x1, y1]], P.alpha('#3a2444', 0.4), 2, { wob: 2, seed: x0 });
      // haze at the foot
      g.fillStyle = P.gradRaw(g, 0, 90, 0, 126, [[0, P.alpha(P.pick('#f6e4c0', '#c8587a'), 0)], [1, P.alpha(P.pick('#f6e4c0', '#c8587a'), 0.75)]]);
      g.fillRect(150, 86, 180, 44);
    });
    // crater rim and lava glow
    P.shape(g, [[226, 56], [236, 52], [247, 57], [240, 60], [231, 60]], P.pick('#4a3040', '#2a1c34'), { wob: 0.8, seed: 5 });
    P.blob(g, 237, 56, 6, 1.8, P.lit('#ff8a2c'), { wob: 0.5, seed: 2 });
    P.glow(g, 237, 56, 20, '#ff7a24', P.dusk ? 0.8 : 0.45);
    P.line(g, [[236, 58], [234, 68], [237, 78], [233, 90]], P.lit('#ff7a24'), 1.6, { wob: 1.5, seed: 6 });
    // plume
    for (const [x, y, r] of [[238, 46, 6], [234, 38, 7.5], [240, 29, 8], [247, 21, 9], [257, 15, 9], [268, 10, 8]]) {
      P.blob(g, x, y, r, r * 0.8, [P.pick('#fffaf0', '#8a7aa8'), P.pick('#cdbed0', '#4c3c78')], { wob: 1.5, seed: x });
    }
  }

  function hills(g, P) {
    P.ridge(g, { y: 118, amp: 7, freq: 0.022, seed: 12, bottom: 140, cols: ['#b8b068', '#8a9a48'] });
    P.ridge(g, { y: 128, amp: 5, freq: 0.03, seed: 21, bottom: 160, cols: ['#a6a838', '#6c8c2c'] });
    // far trees, little painted blobs along the ridge
    const cols = [AUTUMN.orange, AUTUMN.gold, AUTUMN.red, AUTUMN.pine];
    for (let i = 0; i < 18; i++) {
      const x = 8 + i * 18 + (i % 3) * 3, y = 122 + Math.sin(i * 1.7) * 3;
      P.canopy(g, x, y, 5 + (i % 3), 4 + (i % 2), cols[i % 4].map((c) => P.col(c)), 40 + i);
    }
  }

  function windowAt(g, P, x, y, w, h, seed) {
    if (P.dusk) {
      g.fillStyle = P.lit('#ffd24a');
      g.fillRect(x, y, w, h);
      P.glow(g, x + w / 2, y + h / 2, 9, '#ffb830', 0.5);
    } else {
      g.fillStyle = P.col('#9ec8dc');
      g.fillRect(x, y, w, h);
      g.fillStyle = P.alpha('#ffffff', 0.4);
      g.fillRect(x, y, w, 1);
    }
    g.fillStyle = P.col('#4a2c20');
    g.fillRect(x + Math.floor(w / 2), y, 1, h);
    g.fillRect(x, y + Math.floor(h / 2), w, 1);
    void seed;
  }

  function cottage(g, P, x, y, w, h, wall, roof, seed) {
    // y is the ground line
    P.shape(g, [[x, y - h], [x + w, y - h], [x + w, y], [x, y]], wall, { wob: 0.6, seed, edge: '#5a3a28', ea: 0.3 });
    P.within(g, [[x, y - h], [x + w, y - h], [x + w, y], [x, y]], { wob: 0.6, seed }, () => {
      g.fillStyle = P.alpha(P.col('#3a2418'), 0.25);
      g.fillRect(x + w * 0.62, y - h, w * 0.4, h);
      P.dabs(g, x, y - h, w, h, ['#ffffff', '#8c6c4c'], 14, 1.6, seed, 0.2);
    });
    const ov = 3;
    P.shape(g, [[x - ov, y - h + 1], [x + w / 2, y - h - h * 0.6], [x + w + ov, y - h + 1]], roof, { wob: 0.7, seed: seed + 1, edge: '#2c1410', ea: 0.35 });
    P.within(g, [[x - ov, y - h + 1], [x + w / 2, y - h - h * 0.6], [x + w + ov, y - h + 1]], { wob: 0.7, seed: seed + 1 }, () => {
      g.fillStyle = P.alpha('#1c0c08', 0.28);
      g.beginPath(); g.moveTo(x + w / 2, y - h - h * 0.6); g.lineTo(x + w + ov, y - h + 1); g.lineTo(x + w / 2 + 1, y - h + 1); g.closePath(); g.fill();
      P.grain(g, x - 3, y - h * 1.6, w + 6, h * 0.7, '#ffffff', 12, seed, 0.18);
    });
    P.rect(g, x + w * 0.66, y - h - h * 0.72, 3, h * 0.45, '#8c4838', { wob: 0.4, seed });
    windowAt(g, P, x + 3, y - h + 3, 4, 4, seed);
    if (w > 16) windowAt(g, P, x + w - 8, y - h + 3, 4, 4, seed);
    P.rect(g, x + w / 2 - 2, y - 7, 4, 7, '#3a2418', { wob: 0.3, seed });
    if (P.dusk) P.glow(g, x + w / 2, y - 2, 8, '#ffb830', 0.18);
  }

  function pine(g, P, x, y, h, seed) {
    const [dk, md, lt] = AUTUMN.pine.map((c) => P.col(c));
    P.rect(g, x - 1, y - 3, 2, 4, '#3a2418', { wob: 0.2, seed });
    for (let i = 0; i < 4; i++) {
      const ty = y - 3 - i * h * 0.22, w = (h * 0.28) * (1 - i * 0.18);
      P.shape(g, [[x - w, ty], [x, ty - h * 0.3], [x + w, ty]], [lt, md, dk], { wob: 0.8, seed: seed + i });
    }
  }

  function town(g, P) {
    // church
    const wall = ['#f6e8c0', '#cdb68a'];
    P.shape(g, [[138, 134], [138, 112], [168, 112], [168, 134]], wall, { wob: 0.6, seed: 31, edge: '#5a3a28', ea: 0.3 });
    P.shape(g, [[136, 113], [153, 100], [170, 113]], '#9c3a2c', { wob: 0.7, seed: 32, edge: '#2c1410', ea: 0.35 });
    P.shape(g, [[165, 134], [165, 90], [183, 90], [183, 134]], wall, { wob: 0.6, seed: 33, edge: '#5a3a28', ea: 0.3 });
    g.fillStyle = P.alpha(P.col('#3a2418'), 0.22);
    g.fillRect(176, 90, 7, 44);
    P.shape(g, [[162, 91], [174, 58], [186, 91]], ['#c4503c', '#7c2a20'], { wob: 0.7, seed: 34, edge: '#2c1410', ea: 0.4 });
    P.line(g, [[174, 58], [174, 50]], '#2c1c18', 1);
    P.shape(g, [[172, 51], [177, 51], [175, 53]], P.pick('#e8b830', '#c8a850'), { wob: 0.2, seed: 1 });
    // clock and belfry
    P.blob(g, 174, 100, 4.6, 4.6, P.lit(P.pick('#fff6d8', '#ffe8a0')), { wob: 0.3, seed: 2, edge: '#3a2418', ea: 0.7, n: 12 });
    if (P.dusk) P.glow(g, 174, 100, 12, '#ffe8a0', 0.4);
    g.strokeStyle = P.col('#2c1c18'); g.lineWidth = 1;
    g.beginPath(); g.moveTo(174, 100); g.lineTo(174, 97); g.moveTo(174, 100); g.lineTo(176.5, 101); g.stroke();
    P.shape(g, [[171, 120], [171, 111], [172.5, 109], [174, 111], [174, 120]], '#2c1c18', { wob: 0.2, seed: 3 });
    P.rect(g, 148, 124, 5, 10, '#3a2418', { wob: 0.3, seed: 4 });
    windowAt(g, P, 142, 118, 4, 5, 7);
    windowAt(g, P, 158, 118, 4, 5, 8);
    // cottages
    cottage(g, P, 112, 142, 18, 12, ['#f2dcae', '#cdb084'], ['#b8503a', '#80281c'], 41);
    cottage(g, P, 192, 140, 20, 13, ['#e6d4aa', '#bfa57c'], ['#3c5c8c', '#26386c'], 51);
    cottage(g, P, 216, 146, 14, 10, ['#f0d0a4', '#c8a47a'], ['#9c3a2c', '#64261c'], 61);
    cottage(g, P, 86, 146, 16, 11, ['#e8d8b0', '#c4ac84'], ['#8c4a38', '#5c2c20'], 71);
    cottage(g, P, 252, 138, 15, 10, ['#f4e2b6', '#d0b88c'], ['#b8503a', '#80281c'], 81);
    // trees among the houses
    P.rect(g, 133, 138, 2, 7, '#3a2418', { wob: 0.2 });
    P.canopy(g, 134, 133, 8, 9, AUTUMN.orange.map((c) => P.col(c)), 91);
    P.rect(g, 187, 140, 2, 8, '#3a2418', { wob: 0.2 });
    P.canopy(g, 188, 134, 9, 9, AUTUMN.gold.map((c) => P.col(c)), 95);
    P.rect(g, 236, 142, 2, 8, '#3a2418', { wob: 0.2 });
    P.canopy(g, 237, 136, 9, 10, AUTUMN.red.map((c) => P.col(c)), 99);
    pine(g, P, 104, 150, 22, 111);
    pine(g, P, 72, 148, 18, 115);
    pine(g, P, 270, 142, 20, 119);
    pine(g, P, 282, 146, 16, 123);
  }

  function ground(g, P) {
    P.ridge(g, { y: 154, amp: 3, freq: 0.04, seed: 31, bottom: 200, cols: ['#92a634', '#5c7c28', '#3e6020'] });
    P.dabs(g, 0, 156, 320, 44, ['#c8a428', '#e0742c', '#3a5c1c', '#7c9a30'], 90, 3, 33, 0.35);
    // the road, a pale ribbon down from the village
    const road = [[168, 150], [176, 150], [188, 162], [214, 182], [246, 200], [110, 200], [136, 184], [158, 164]];
    P.shape(g, road, ['#ecd49c', '#bf9e68'], { wob: 1.6, seed: 34, edge: '#6c4a2c', ea: 0.25 });
    P.within(g, road, { wob: 1.6, seed: 34 }, () => {
      P.dabs(g, 100, 150, 160, 50, ['#a8845a', '#f6e4b4', '#8c6a44'], 70, 2.2, 36, 0.35);
      P.line(g, [[172, 151], [170, 170], [180, 188], [200, 200]], P.alpha('#6c4a2c', 0.3), 1.5, { wob: 2, seed: 37 });
    });
    // tufts and fallen leaves
    const r = P.mulberry(55);
    for (let i = 0; i < 90; i++) {
      const x = r() * 320, y = 158 + r() * 42;
      if (x > 110 && x < 250 && y > 170 && Math.abs(x - (180 + (y - 160) * 1.1)) < 36) continue;
      g.strokeStyle = P.col(r() < 0.5 ? '#3e6020' : '#a4b440');
      g.lineWidth = 1;
      g.beginPath(); g.moveTo(x, y); g.lineTo(x - 1, y - 3); g.moveTo(x, y); g.lineTo(x + 1.5, y - 2.5); g.stroke();
    }
    P.dabs(g, 0, 160, 320, 40, ['#e0742c', '#b02c1c', '#f0b030'], 34, 1.2, 77, 0.9);
    // foreground bushes and a big tree on the left, by the card
    P.canopy(g, 14, 178, 22, 16, AUTUMN.gold.map((c) => P.col(c)), 140);
    P.canopy(g, 300, 190, 24, 12, AUTUMN.orange.map((c) => P.col(c)), 150);
  }

  function pole(g, P, x, top, bottom, w, arm) {
    P.shape(g, [[x - w / 2, top], [x + w / 2, top], [x + w / 2 + 0.3, bottom], [x - w / 2 - 0.3, bottom]], ['#7a5a3c', '#3c281c'], { wob: 0.4, seed: x, v: false });
    P.shape(g, [[x - arm, top + w], [x + arm, top + w], [x + arm, top + w + w * 0.7], [x - arm, top + w + w * 0.7]], ['#6c4c30', '#3c281c'], { wob: 0.3, seed: x + 1 });
    g.fillStyle = P.col('#9cd0c8');
    for (const dx of [-arm + 1, arm - 1]) g.fillRect(x + dx - 0.5, top + w - 1.5, 1.5, 2);
  }

  function lines(g, P) {
    const poles = [
      [12, 108, 150, 2], [60, 112, 150, 2.4], [112, 114, 158, 2.8], [196, 112, 162, 3.2], [262, 54, 200, 5.2],
    ];
    // left poles first (far), big one last
    const tips = poles.map(([x, top, , w]) => ({ x, y: top + w * 1.4, a: 2 + w * 1.2 }));
    for (let i = 0; i < poles.length - 1; i++) {
      for (const sgn of [-0.6, 0.6]) {
        const A = tips[i], B = tips[i + 1];
        P.wire(g, [A.x + A.a * sgn * 0.8, A.y], [B.x + B.a * sgn * 0.8, B.y], 3 + (B.x - A.x) * 0.045, P.pick('#2c2018', '#14102c'), 1);
      }
    }
    // the line runs off to the right from the big pole
    P.wire(g, [tips[4].x + tips[4].a * 0.5, tips[4].y], [330, tips[4].y + 6], 4, P.pick('#2c2018', '#14102c'), 1);
    P.wire(g, [tips[4].x - tips[4].a * 0.5, tips[4].y], [tips[3].x + tips[3].a * 0.5, tips[3].y], 5, P.pick('#2c2018', '#14102c'), 1);
    for (const [x, top, bottom, w] of poles) pole(g, P, x, top, bottom, w, 2 + w * 1.2);
    // leaning ladder at the foot of the big pole
    P.line(g, [[273, 198], [265, 146]], '#a87c48', 1.4);
    P.line(g, [[277, 198], [269, 146]], '#a87c48', 1.4);
    for (let i = 0; i < 6; i++) P.line(g, [[273.5 - i * 1.4 + 2, 194 - i * 8.6], [277.5 - i * 1.4 + 1, 194 - i * 8.6 - 0.6]], '#6c4a28', 1);
  }

  // The box that holds the disks: a wooden crate by the road, floppies standing up in it like toast.
  function diskCrate(g, P) {
    const x = 82, w = 42, top = 157, bot = 177;
    P.shadow(g, x + w / 2 + 2, bot + 1, 22, 3.4, 0.35);
    const cols = ['#d8382c', '#2c5cc8', '#f0b020', '#2c9c58', '#8c3cb8', '#e8741c', '#d8382c'];
    cols.forEach((c, i) => {
      g.save();
      g.translate(x + 5 + i * 5.3, top + 3);
      g.rotate((i - 3) * 0.08);
      P.floppy(g, -4.5, -14, 9, c);
      g.restore();
    });
    const front = [[x, top], [x + w, top], [x + w + 1, bot], [x - 1, bot]];
    P.shape(g, front, ['#c8924c', '#7c5224'], { wob: 0.5, seed: 61, v: true, edge: '#2c1a0c', ea: 0.7 });
    P.within(g, front, { wob: 0.5, seed: 61 }, () => {
      g.fillStyle = P.alpha(P.col('#2c1a0c'), 0.5);
      g.fillRect(x, top + 5, w, 1); g.fillRect(x, top + 11, w, 1);
      g.fillRect(x + 4, top, 1, 16); g.fillRect(x + w - 5, top, 1, 16);
      P.dabs(g, x, top, w, 16, ['#f0d49c', '#4a2c14'], 10, 1.2, 62, 0.3);
    });
    // a paper tag, lettered by hand
    P.rect(g, x + 4, top + 5, w - 8, 11, '#f6f0dc', { wob: 0.2, seed: 63, edge: '#2c1a0c', ea: 0.5 });
    P.text(g, 'DISKS', x + (w - 29) / 2 + 0.5, top + 8, '#2c2c5c', 1, 1);
    P.dabs(g, x - 6, bot - 3, w + 12, 6, ['#3e6020', '#7c9a30'], 8, 1.6, 64, 0.8);
  }

  // The telephone on the big pole: a wall set with a handset on a hook.
  function poleTelephone(g, P) {
    P.wallPhone(g, 244, 103, 1.3);
  }

  function paint(g, P) {
    sky(g, P);
    volcano(g, P);
    hills(g, P);
    town(g, P);
    ground(g, P);
    lines(g, P);
    diskCrate(g, P);
    poleTelephone(g, P);
  }

  // ---------- animated bits ----------

  function bubbleAt(g, P, x, y, r, a) {
    g.save();
    g.globalAlpha = a;
    // an @: outer ring, inner ring and the tail that joins them
    g.strokeStyle = P.pick('#fffaf0', '#c8b8e8');
    g.fillStyle = 'rgba(0,0,0,0)';
    g.lineWidth = Math.max(1.4, r * 0.28);
    g.lineCap = 'round';
    g.beginPath(); g.arc(x, y, r, 0.4, Math.PI * 2 - 0.1); g.stroke();
    g.beginPath(); g.arc(x, y, r * 0.42, 0, Math.PI * 2); g.stroke();
    g.beginPath(); g.moveTo(x + r * 0.42, y - r * 0.1); g.lineTo(x + r * 0.42, y + r * 0.5); g.quadraticCurveTo(x + r * 0.5, y + r * 0.62, x + r * 0.95, y + r * 0.35); g.stroke();
    g.restore();
  }

  const anims = [
    {
      x: 0, y: 0, w: 320, h: 64, sample: 20,
      draw(g, t, P) {
        const items = [[0, 26, 1.5, 3], [90, 44, 1.1, 5], [170, 16, 1.8, 8], [240, 40, 1.0, 12]];
        for (const [x0, y, s, seed] of items) {
          const sp = 1.2 + seed * 0.14;
          const x = ((x0 + t * sp + 60) % 400) - 60;
          P.cloud(g, x, y, s, seed);
        }
      },
    },
    {
      // smoke rings shaped like @ signs, drifting up from the crater
      gag: { start: 6, period: 21, dur: 7 }, x: 214, y: 0, w: 106, h: 52, sample: 0,
      draw(g, t, P, u) {
        for (let i = 0; i < 3; i++) {
          const k = clamp01((u * 1.5 - i * 0.28));
          if (k <= 0 || k >= 1) continue;
          bubbleAt(g, P, 240 + k * 52 + i * 3, 46 - k * 40, 3 + k * 6.5, Math.sin(k * Math.PI) * 0.95);
        }
      },
    },
    {
      // the repairman stuck at the top of the pole, kicking his legs now and then
      x: 246, y: 56, w: 34, h: 56, sample: 0,
      draw(g, t, P) {
        const px = 262, y0 = 74;
        const kick = Math.sin(t * 5) * (Math.floor(t / 4) % 2 ? 1 : 0.25);
        // legs and boots dangling
        const leg = (dx, ph) => {
          const sw = kick * ph * 3;
          P.line(g, [[px + dx, y0 + 14], [px + dx + sw, y0 + 25]], '#2c4c8c', 3);
          P.shape(g, [[px + dx + sw - 2, y0 + 24], [px + dx + sw + 3, y0 + 24], [px + dx + sw + 3, y0 + 27], [px + dx + sw - 2, y0 + 27]], '#3a2418', { wob: 0.3, seed: 2 });
        };
        leg(-3, 1); leg(4, -1);
        // body hugging the pole
        P.shape(g, [[px - 6, y0 + 3], [px + 7, y0 + 3], [px + 7, y0 + 15], [px - 6, y0 + 15]], ['#f08c28', '#c05c18'], { wob: 0.6, seed: 4 });
        P.line(g, [[px - 7, y0 + 6], [px - 2, y0 + 2]], '#f6c8a0', 2);
        P.line(g, [[px + 8, y0 + 6], [px + 3, y0 + 2]], '#f6c8a0', 2);
        // head with a yellow hat, looking sideways at you
        P.blob(g, px + 6, y0 - 1, 3.2, 3.4, '#f6c8a0', { wob: 0.3, seed: 5, n: 10 });
        P.shape(g, [[px + 2, y0 - 2], [px + 4, y0 - 6], [px + 9, y0 - 6], [px + 11, y0 - 2]], ['#ffe040', '#d8a818'], { wob: 0.3, seed: 6 });
        g.fillStyle = '#2c1c18';
        g.fillRect(px + 7, y0 - 1, 1, 1);
        g.fillRect(px + 8, y0 + 1, 2, 1);
      },
    },
    {
      // a mailbox that has grown legs; every so often it strolls to the road and back
      x: 266, y: 164, w: 54, h: 32, sample: 0,
      draw(g, t, P) {
        const c = (t % 21) / 21;
        let off = 0, step = 0;
        if (c > 0.55 && c < 0.8) { const k = (c - 0.55) / 0.25; off = -Math.sin(k * Math.PI) * 22; step = Math.sin(k * 50); }
        const x = 294 + off, y = 190;
        const sw = step * 2.5;
        P.line(g, [[x - 3, y - 7], [x - 3 + sw, y - 1]], '#2c2c3c', 1.5);
        P.line(g, [[x + 3, y - 7], [x + 3 - sw, y - 1]], '#2c2c3c', 1.5);
        P.shape(g, [[x - 6 + sw, y - 1.5], [x - 1 + sw, y - 1.5], [x - 1 + sw, y + 1], [x - 7 + sw, y + 1]], '#3a2418', { wob: 0.2, seed: 1 });
        P.shape(g, [[x + 1 - sw, y - 1.5], [x + 6 - sw, y - 1.5], [x + 7 - sw, y + 1], [x + 1 - sw, y + 1]], '#3a2418', { wob: 0.2, seed: 2 });
        P.shape(g, [[x - 8, y - 9], [x - 8, y - 18], [x - 3, y - 22], [x + 3, y - 22], [x + 8, y - 18], [x + 8, y - 9]], ['#a8b0c8', '#5a6490'], { wob: 0.4, seed: 3, edge: '#1c2040', ea: 0.5 });
        P.shape(g, [[x + 8, y - 17], [x + 13, y - 17], [x + 13, y - 22], [x + 9, y - 22]], '#d8382c', { wob: 0.3, seed: 4 });
        g.fillStyle = '#1c2040';
        g.fillRect(x - 4, y - 16, 5, 1);
        g.fillStyle = '#fff';
        g.fillRect(x - 5, y - 19, 2, 2); g.fillRect(x - 1, y - 19, 2, 2);
        g.fillStyle = '#1c2040';
        g.fillRect(x - 4, y - 19, 1, 1); g.fillRect(x, y - 19, 1, 1);
      },
    },
  ];

  const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);

  const hotspots = [
    { id: 'repairman', role: 'phone', x: 250, y: 66, w: 26, h: 42, say: [
      'A repairman is stuck at the top of the pole. He says he is "just inspecting the line."',
      'The repairman has been up there since Tuesday. Nobody has told him which Tuesday.',
      'He waves. The pole wobbles. You both pretend that did not happen.'] },
    { id: 'pole', x: 258, y: 108, w: 10, h: 90, say: [
      'A telephone pole. It has heard everything, and for a small fee will deny it.',
      'The pole is humming. Possibly a hymn, possibly a bill.'] },
    { id: 'mailbox', x: 281, y: 166, w: 28, h: 28, say: [
      'The mailbox has sprouted legs. It looks like it is late for something.',
      'You offer it a letter. It takes a step back and checks its flag.',
      'It is a very nervous mailbox. Think of the post it has seen.'] },
    { id: 'volcano', x: 205, y: 6, w: 100, h: 112, say: [
      'The volcano is puffing little @ signs. It has a lot of mail to get through.',
      'It rumbles politely, like someone clearing their throat at a dinner party.',
      'No, you may not toast anything over it. There is a sign.'] },
    { id: 'church', x: 160, y: 54, w: 28, h: 82, say: [
      'The church clock reads a quarter past something. The bell reads the clock.',
      'The weathervane points at you. It always does. It is very rude.'] },
    { id: 'cottages', x: 84, y: 118, w: 70, h: 36, say: [
      'A cottage with its curtains drawn and its chimney crossed.',
      'Somebody inside is humming along to a dial tone.'] },
    { id: 'trees', x: 226, y: 122, w: 28, h: 30, say: [
      'An autumn tree. It is showing off, and the other trees are letting it.',
      'A leaf falls on your head. This is the full extent of the excitement here.'] },
    { id: 'clouds', x: 0, y: 0, w: 200, h: 80, say: [
      'A cloud drifts past. It does not have the right of way, but it has the confidence.'] },
  ];

  // The two objects that run the game: where the disks live and the telephone (adv.js).
  hotspots.push(
    { id: 'crate', role: 'disks', x: 80, y: 142, w: 48, h: 38, say: ['A crate of disks. It is labelled, which is more than most crates manage.'] },
    { id: 'telephone', role: 'phone', x: 240, y: 102, w: 24, h: 30, say: ['A telephone, bolted to a pole. Somebody up there is expected to answer it.'] });

  const objects = {
    disks: {
      open: 'You lift the lid of the crate. Rows of floppies look up at you, hopeful.',
      got: 'The repairman up the pole is waving at his telephone. Perhaps he will take the call.',
      again: 'The crate does not mind, but the floppies are starting to talk.',
      hint: 'Something is glinting in the grass by the road. It looks like a crate with ideas.',
    },
    phone: {
      refuse: 'You pick up the receiver. The repairman leans down and says: "Disks first, please."',
      answer: 'Number please?',
      hint: 'The telephone on the pole is twinkling. The repairman is pretending not to notice.',
      busy: 'The repairman holds up one finger. He is already on the line.',
    },
  };

  return { id: 'valley', title: 'The valley', focus: { x: 215, y: 118 }, paint, anims, hotspots, objects,
    termAt: [166, 138],
    spark: { disks: [80, 140, 48, 40], phone: [238, 100, 30, 32] } };
})());
