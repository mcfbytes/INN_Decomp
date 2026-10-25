// Background music for the landing: a tiny sequencer and a two-operator FM synth on the Web Audio API,
// in the spirit of an early-90s OPL2 sound card. No samples, no MIDI files, no third-party code; the
// songs are original text notation in music/<scene>.js. Loaded before adv.js. Notation and playback
// rules: docs/web.md, section Music.
//
//   InnMusic.engine(ctx, out)  builds the synth and mixer on any AudioContext (live or offline)
//   engine.player(song, opts)  a looping, look-ahead scheduled song; player.schedule(untilTime)
//   InnMusic.enabled/toggle/setScene/leave/...  the page policy (first gesture, crossfade, hide, leave)
'use strict';

const InnMusic = (() => {
  const KEY = 'inn.music';
  const MAXV = 8;             // voices sounding at once (steals the one nearest its end)
  const AHEAD = 0.45;         // seconds of audio scheduled in advance
  const TICK_MS = 40;         // how often the look-ahead runs (the audio clock does the timing)
  const FADE_S = 1.0;         // crossfade between scenes

  // ---------- the instruments (patches) ----------
  // r: modulator/carrier ratio; ix: peak modulation index (radians); ixa: start fraction of the index
  // swell; ixd/ixs: index decay time and sustain fraction; a/d/s/rl: carrier attack, decay, sustain
  // level (0 = a plucked note that rings out by itself), release; vib/vd: vibrato cents and delay;
  // det: a second carrier detuned by that many cents; w: carrier wave (0 sine, 1 rectified sine); g: level.
  const PATCH = {
    harpsichord: { r: 1, ix: 2.4, ixd: .18, ixs: .45, a: .002, d: 1.1, s: 0, rl: .05, g: .60 },
    flute: { r: 1, ix: .55, ixd: .1, ixs: 1, a: .07, d: .1, s: .85, rl: .14, vib: 14, vd: .2, g: .75 },
    strings: { r: 1, ix: 1.2, ixd: .3, ixs: .9, a: .22, d: .2, s: .8, rl: .35, vib: 9, vd: .15, det: 9, g: .40 },
    brass: { r: 1, ix: 3.2, ixa: .25, ixd: .2, ixs: .65, a: .05, d: .2, s: .8, rl: .1, vib: 7, vd: .3, g: .50 },
    bass: { r: 1, ix: 2.0, ixd: .18, ixs: .35, a: .006, d: .4, s: .55, rl: .07, g: 1.0 },
    bell: { r: 3.5, ix: 2.4, ixd: 1.6, ixs: .02, a: .002, d: 2.2, s: 0, rl: .3, g: .50 },
    organ: { r: 2, ix: .9, ixd: .1, ixs: 1, a: .025, d: .02, s: 1, rl: .07, det: 5, vib: 5, vd: .4, w: 1, g: .42 },
    pluck: { r: 3, ix: 2.6, ixd: .07, ixs: .3, a: .002, d: .45, s: 0, rl: .05, g: .65 },
  };

  // ---------- notation ----------
  // ABC-like: C D E F G A B is the octave from middle C, c d e f g a b the next one, c' above, C, below.
  // ^ sharp, _ flat, = natural before a note (the song's `sig`, such as '_B_E', flats those letters).
  // A number after a note or rest (z) is its length in steps (a step is a sixteenth note); l<n> sets the
  // default length, v<n> the velocity (1 to 15). [CEG]4 is a chord, `!` after a length makes a note
  // short, `~` ties it to the next note when that has the same pitch, `|` ends a bar (every bar must
  // add up to song.bar steps; parse() reports the ones that do not).
  const LETTER = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 };

  function pitchAt(s, i, sig) {
    let acc = null;
    if (s[i] === '^' || s[i] === '_' || s[i] === '=') { acc = s[i] === '^' ? 1 : s[i] === '_' ? -1 : 0; i++; }
    const ch = s[i++];
    const base = LETTER[(ch || '').toUpperCase()];
    if (base === undefined) throw new Error(`bad note near "${s.slice(Math.max(0, i - 3), i + 3)}"`);
    let oct = ch === ch.toLowerCase() ? 5 : 4;
    while (s[i] === ',' || s[i] === "'") oct += s[i++] === "'" ? 1 : -1;
    if (acc === null) acc = sig[ch.toUpperCase()] || 0;
    return { m: 12 * (oct + 1) + base + acc, i };
  }

  function sigMap(sig) {
    const m = {};
    for (let i = 0; i + 1 < (sig || '').length; i += 2) m[sig[i + 1]] = sig[i] === '^' ? 1 : -1;
    return m;
  }

  // Returns { ev: [{t, d, m, v, st}], errors: [...] } with t and d in steps.
  function parseMml(src, song) {
    const sig = sigMap(song.sig);
    const ev = [], errors = [];
    let t = 0, L = 2, vel = 10, tie = null;
    src.split('|').forEach((bar, bi) => {
      const t0 = t;
      let i = 0;
      const num = () => { let j = i; while (/\d/.test(bar[j] || '')) j++; const n = j > i ? +bar.slice(i, j) : null; i = j; return n; };
      try {
        while (i < bar.length) {
          const c = bar[i];
          if (/\s/.test(c)) { i++; continue; }
          if (c === 'l') { i++; L = num() || L; continue; }
          if (c === 'v') { i++; vel = num() || vel; continue; }
          if (c === 'z') { i++; t += num() || L; tie = null; continue; }
          let ms;
          if (c === '[') {
            const close = bar.indexOf(']', i);
            ms = [];
            let j = i + 1;
            while (j < close) { const p = pitchAt(bar, j, sig); ms.push(p.m); j = p.i; }
            i = close + 1;
          } else {
            const p = pitchAt(bar, i, sig);
            ms = [p.m]; i = p.i;
          }
          const d = num() || L;
          let st = false, tied = false;
          while (bar[i] === '!' || bar[i] === '~') { if (bar[i] === '!') st = true; else tied = true; i++; }
          if (tie && ms.length === 1 && tie.m === ms[0] && tie.t + tie.d === t) {
            tie.d += d;
          } else {
            tie = null;
            for (const m of ms) ev.push({ t, d, m, v: vel / 12, st });
            if (ms.length === 1) tie = ev[ev.length - 1];
          }
          if (!tied) tie = null;
          t += d;
        }
      } catch (e) { errors.push(`bar ${bi + 1}: ${e.message}`); }
      if (t - t0 !== song.bar) errors.push(`bar ${bi + 1}: ${t - t0} steps, expected ${song.bar}`);
    });
    return { ev, errors };
  }

  // ---------- generated accompaniment (from the song's chord list) ----------
  const QUAL = { '': [0, 4, 7], m: [0, 3, 7], 7: [0, 4, 7, 10], m7: [0, 3, 7, 10], maj7: [0, 4, 7, 11], 6: [0, 4, 7, 9],
    dim: [0, 3, 6], sus: [0, 5, 7], 9: [0, 4, 7, 10, 14], m6: [0, 3, 7, 9] };
  const PC = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 };

  function chordOf(name) {
    const m = /^([A-G])([#b]?)(.*)$/.exec(name);
    if (!m || !QUAL[m[3]]) throw new Error(`bad chord "${name}"`);
    return { r: (PC[m[1]] + (m[2] === '#' ? 1 : m[2] === 'b' ? -1 : 0) + 12) % 12, q: QUAL[m[3]] };
  }

  // song.chords: one entry per bar, "F" or "F.C7" (the bar split evenly among the chords named).
  function chordTrack(song) {
    const out = [];
    song.chords.forEach((entry, bi) => {
      const names = entry.split('.');
      const len = song.bar / names.length;
      names.forEach((n, k) => out.push({ c: chordOf(n), from: bi * song.bar + k * len, to: bi * song.bar + (k + 1) * len }));
    });
    return out;
  }

  // Pattern characters (one per step; `.` holds, `-` rests):
  //   1 root, 3 third, 5 fifth, 6 sixth, 7 seventh, 8 octave, f fifth below, n approach to the next root (bass);
  //   c the whole chord, x y z w its 1st..4th tone (comp range).
  function genPart(part, song, chords) {
    const ev = [];
    const pats = [].concat(part.p);
    const lo = part.lo || (part.k === 'comp' ? 55 : 35);
    const near = (pc, floor) => floor + ((pc - floor) % 12 + 12) % 12;
    for (let bi = 0; bi < song.bars; bi++) {
      let pat = pats[bi % pats.length];
      pat = pat + '-'.repeat(Math.max(0, song.bar - pat.length));
      for (let k = 0; k < song.bar; k++) {
        const ch = pat[k];
        if (ch === '-' || ch === '.') continue;
        let len = 1;
        while (pat[k + len] === '.') len++;
        const at = bi * song.bar + k;
        const idx = chords.findIndex((c) => at >= c.from && at < c.to);
        const cur = chords[idx].c, next = chords[(idx + 1) % chords.length].c;
        const root = near(cur.r, lo);
        const voice = cur.q.map((x) => root + x);
        const add = (m, v) => ev.push({ t: at, d: len, m, v: v || 1, st: false, gate: part.gate || .8 });
        const tone = { 1: 0, 3: 1, 5: 2, 7: 3, x: 0, y: 1, z: 2, w: 3 }[ch];
        if (ch === 'c') voice.forEach((m) => add(m, .85));
        else if (ch === '6') add(root + 9);
        else if (ch === '8') add(root + 12);
        else if (ch === 'f') add(root - 5);
        else if (ch === 'n') {
          const nr = near(next.r, lo);
          add(nr === root ? root + 7 : nr - 1 < lo - 1 ? nr + 11 : nr - 1, .9);
        } else if (tone !== undefined) add(voice[Math.min(tone, voice.length - 1)]);
        else throw new Error(`bad pattern character "${ch}"`);
      }
    }
    return ev;
  }

  // Drum patterns, one string per bar (cycled). k kick, s snare, h hat, o open hat, r rim; capitals are
  // accents; (kh) is two at once; `.` is silence.
  function drumPart(part, song) {
    const ev = [];
    const pats = [].concat(part.p);
    for (let bi = 0; bi < song.bars; bi++) {
      const pat = pats[bi % pats.length].replace(/\s/g, '');
      let k = 0;
      for (let i = 0; i < pat.length && k < song.bar; i++, k++) {
        const group = pat[i] === '(' ? pat.slice(i + 1, pat.indexOf(')', i)) : pat[i];
        if (pat[i] === '(') i = pat.indexOf(')', i);
        for (const c of group) if (c !== '.') ev.push({ t: bi * song.bar + k, d: 0, drum: c.toLowerCase(), v: c === c.toUpperCase() ? 1 : .65 });
      }
    }
    return ev;
  }

  // Everything one song contributes, sorted: the notes of each part at their step. Also reports mistakes.
  function compile(song) {
    const errors = [];
    const chords = song.chords ? chordTrack(song) : null;
    const total = song.bars * song.bar;
    if (chords && chords[chords.length - 1].to !== total) errors.push(`chords cover ${chords[chords.length - 1].to} steps, song is ${total}`);
    const all = [];
    song.parts.forEach((part, pi) => {
      let ev;
      if (part.i === 'drums') ev = drumPart(part, song);
      else if (part.p) ev = genPart(part, song, chords);
      else {
        const r = parseMml(part.n, song);
        r.errors.forEach((e) => errors.push(`part ${pi} (${part.i}): ${e}`));
        ev = r.ev;
      }
      for (const e of ev) if (e.t < total) all.push(Object.assign(e, { pi }));
    });
    all.sort((a, b) => a.t - b.t || a.pi - b.pi);
    return { ev: all, errors };
  }

  // ---------- the synth and mixer ----------
  function engine(ctx, out) {
    const mix = ctx.createGain();
    mix.gain.value = .2;
    const comp = ctx.createDynamicsCompressor();
    comp.threshold.value = -16; comp.knee.value = 12; comp.ratio.value = 5; comp.attack.value = .004; comp.release.value = .18;
    const master = ctx.createGain();
    master.gain.value = 1;
    mix.connect(comp); comp.connect(master); master.connect(out);

    // One second of white noise (a fixed generator, so renders repeat exactly) shared by all percussion.
    const noise = ctx.createBuffer(1, ctx.sampleRate, ctx.sampleRate);
    { const d = noise.getChannelData(0); let x = 12345; for (let i = 0; i < d.length; i++) { x = (Math.imul(x, 1664525) + 1013904223) | 0; d[i] = (x / 2147483648); } }
    let nCount = 0;

    // Rectified sine: a brighter carrier, a little like an OPL2 abs-sine wave.
    const absWave = (() => {
      const re = new Float32Array(18), im = new Float32Array(18);
      for (let k = 1; k <= 8; k++) re[2 * k] = -(4 / Math.PI) / (4 * k * k - 1);
      return ctx.createPeriodicWave(re, im);
    })();

    const ramp = (param, to, t0, secs) => {
      param.cancelScheduledValues(t0);
      param.setValueAtTime(param.value, t0);
      param.linearRampToValueAtTime(to, t0 + secs);
    };

    function player(song, o = {}) {
      const compiled = compile(song);
      if (compiled.errors.length) console.warn(`music ${song.id}:`, compiled.errors.join('; '));
      const pg = ctx.createGain();
      pg.gain.value = 0;
      pg.connect(mix);
      const buses = song.parts.map((p) => {
        const g = ctx.createGain();
        g.gain.value = p.v == null ? .8 : p.v;
        let node = g;
        if (p.pan && ctx.createStereoPanner) { const sp = ctx.createStereoPanner(); sp.pan.value = p.pan; g.connect(sp); node = sp; }
        node.connect(pg);
        return g;
      });
      const total = song.bars * song.bar;
      const P = { ev: compiled.ev, errors: compiled.errors, song, voices: [], cursor: 0, stopped: false, endAt: Infinity };
      let loopT = o.t0 == null ? ctx.currentTime + .08 : o.t0;
      let level = 0, first = true;

      // At each pass of the loop: pick the tempo and level (the dusk variant is slower and softer).
      function beginLoop() {
        const dusk = !!(o.dusk && o.dusk());
        const sd = song.dusk || { tempo: .9, level: .8 };
        P.dusk = dusk;
        P.stepS = 60 / (song.bpm * (dusk ? sd.tempo : 1)) / 4;
        const to = dusk ? sd.level : 1;
        if (first) {
          pg.gain.setValueAtTime(.0001, loopT);
          pg.gain.linearRampToValueAtTime(to, loopT + (o.fadeIn == null ? .05 : o.fadeIn));
        } else if (to !== level) {
          pg.gain.setValueAtTime(level, loopT);
          pg.gain.linearRampToValueAtTime(to, loopT + 2);
        }
        level = to; first = false;
      }
      beginLoop();

      const when = (e) => loopT + (e.t + (e.t % 4 === 2 && song.swing ? song.swing * 2 : 0)) * P.stepS;

      P.schedule = (until) => {
        while (!P.stopped) {
          if (P.cursor >= P.ev.length) {
            const next = loopT + total * P.stepS;
            if (next > until) return;
            loopT = next; P.cursor = 0; beginLoop();
            if (!P.ev.length) return;
          }
          const e = P.ev[P.cursor];
          const t = when(e);
          if (t > until || t > P.endAt) return;
          P.cursor++;
          if (t < ctx.currentTime - .12 && ctx.currentTime > 0) continue;   // a stall: skip, never pile up
          const part = song.parts[e.pi];
          if (e.drum) drum(e.drum, t, e.v * (part.v == null ? .8 : part.v), buses[e.pi], P);
          else note(patchFor(part, P.dusk), buses[e.pi], e, t, P);
        }
      };

      P.fadeOut = (secs) => {
        const t = ctx.currentTime;
        ramp(pg.gain, 0.0001, t, secs);
        P.endAt = t;                   // schedule nothing further
        P.stopAt = t + secs + .6;      // the last notes ring out silently
      };
      P.dispose = () => {
        P.stopped = true;
        for (const v of P.voices) v.cut(ctx.currentTime);
        setTimeout(() => { try { pg.disconnect(); } catch { /* gone */ } }, 100);
      };
      return P;
    }

    const patchFor = (part, dusk) => PATCH[(dusk && part.d) || part.i];

    function steal(P, t) {
      P.voices = P.voices.filter((v) => v.end > t);
      if (P.voices.length < MAXV) return;
      let k = 0;
      for (let i = 1; i < P.voices.length; i++) if (P.voices[i].end < P.voices[k].end) k = i;
      P.voices[k].cut(t);
      P.voices.splice(k, 1);
    }

    function note(pt, bus, e, t, P) {
      steal(P, t);
      const f = 440 * Math.pow(2, (e.m - 69) / 12);
      const vel = Math.min(1, e.v);
      const peak = vel * pt.g;
      const len = Math.max(.05, e.d * P.stepS);
      const car = ctx.createOscillator(), mod = ctx.createOscillator();
      const mg = ctx.createGain(), env = ctx.createGain(), kill = ctx.createGain();
      car.frequency.value = f; mod.frequency.value = f * pt.r;
      if (pt.w === 1) car.setPeriodicWave(absWave);
      mod.connect(mg); mg.connect(car.frequency);
      car.connect(env);
      const oscs = [car, mod];
      if (pt.det) {
        const c2 = ctx.createOscillator();
        c2.frequency.value = f; c2.detune.value = pt.det;
        if (pt.w === 1) c2.setPeriodicWave(absWave);
        mg.connect(c2.frequency); c2.connect(env); oscs.push(c2);
      }
      // modulation index envelope (a swell for brass, a quick fall for plucks)
      const ixp = pt.ix * (.55 + .45 * vel) * f * pt.r;
      const floor = Math.max(.001, ixp * pt.ixs);
      if (pt.ixa) {
        mg.gain.setValueAtTime(ixp * pt.ixa, t);
        mg.gain.linearRampToValueAtTime(ixp, t + pt.a);
        mg.gain.exponentialRampToValueAtTime(floor, t + pt.a + pt.ixd);
      } else {
        mg.gain.setValueAtTime(ixp, t);
        mg.gain.exponentialRampToValueAtTime(floor, t + pt.ixd);
      }
      // carrier envelope: attack, decay, sustain until the note ends, release; every step is a ramp
      const g = env.gain;
      g.setValueAtTime(.0001, t);
      g.linearRampToValueAtTime(peak, t + pt.a);
      let end;
      if (pt.s === 0) {
        g.exponentialRampToValueAtTime(.0001, t + pt.a + pt.d);
        end = t + pt.a + pt.d + .03;
      } else {
        const lvl = Math.max(.0002, peak * pt.s);
        g.exponentialRampToValueAtTime(lvl, t + pt.a + pt.d);
        const te = Math.max(t + len * (e.st ? .5 : (e.gate || .96)), t + pt.a + pt.d);
        g.setValueAtTime(lvl, te);
        g.exponentialRampToValueAtTime(.0001, te + pt.rl);
        end = te + pt.rl + .03;
      }
      if (pt.vib) {
        const lfo = ctx.createOscillator(), vg = ctx.createGain();
        lfo.frequency.value = 5 + (e.m % 3) * .25;
        vg.gain.setValueAtTime(0, t);
        vg.gain.setValueAtTime(0, t + pt.vd);
        vg.gain.linearRampToValueAtTime(pt.vib, t + pt.vd + .4);
        lfo.connect(vg);
        for (const c of oscs.filter((x) => x !== mod)) vg.connect(c.detune);
        oscs.push(lfo);
      }
      env.connect(kill); kill.connect(bus);
      for (const x of oscs) { x.start(t); x.stop(end); }
      const v = { end, cut(tc) {
        if (tc >= v.end) return;
        kill.gain.setValueAtTime(1, tc);
        kill.gain.linearRampToValueAtTime(0, tc + .02);
        for (const x of oscs) x.stop(tc + .03);
        v.end = tc + .03;
      } };
      car.onended = () => { try { kill.disconnect(); } catch { /* gone */ } };
      P.voices.push(v);
      P.maxV = Math.max(P.maxV || 0, P.voices.length);
    }

    function drum(kind, t, vel, bus, P) {
      steal(P, t);
      const nodes = [];
      const env = ctx.createGain();
      const hit = (secs, peak) => {
        env.gain.setValueAtTime(.0001, t);
        env.gain.linearRampToValueAtTime(peak, t + .002);
        env.gain.exponentialRampToValueAtTime(.0001, t + secs);
      };
      const src = (filterType, freq, q) => {
        const n = ctx.createBufferSource(); n.buffer = noise;
        const f = ctx.createBiquadFilter(); f.type = filterType; f.frequency.value = freq; f.Q.value = q;
        n.connect(f);
        nodes.push(n);
        return { n, f };
      };
      let secs = .2;
      if (kind === 'k') {
        const o = ctx.createOscillator();
        o.frequency.setValueAtTime(150, t); o.frequency.exponentialRampToValueAtTime(46, t + .1);
        o.connect(env); nodes.push(o);
        hit(secs = .3, vel * 1.1);
      } else if (kind === 's') {
        const { f } = src('bandpass', 1900, .7); f.connect(env);
        const o = ctx.createOscillator(); o.type = 'triangle'; o.frequency.setValueAtTime(190, t); o.frequency.exponentialRampToValueAtTime(140, t + .08);
        const og = ctx.createGain(); og.gain.value = .5; o.connect(og); og.connect(env); nodes.push(o);
        hit(secs = .17, vel * .75);
      } else if (kind === 'r') {
        const o = ctx.createOscillator(); o.frequency.value = 1650; o.connect(env); nodes.push(o);
        hit(secs = .04, vel * .45);
      } else {
        const { f } = src('highpass', 7000, .7); f.connect(env);
        hit(secs = kind === 'o' ? .26 : .05, vel * (kind === 'o' ? .3 : .34));
      }
      env.connect(bus);
      for (const n of nodes) {
        if (n.buffer) n.start(t, (nCount++ * .137) % .8, secs + .05); else n.start(t);
        if (!n.buffer) n.stop(t + secs + .03);
      }
      const v = { end: t + secs + .04, cut(tc) {
        if (tc >= v.end) return;
        env.gain.cancelScheduledValues(tc); env.gain.setValueAtTime(env.gain.value, tc); env.gain.linearRampToValueAtTime(0, tc + .01);
        v.end = tc + .02;
      } };
      P.voices.push(v);
      P.maxV = Math.max(P.maxV || 0, P.voices.length);
    }

    return { ctx, player, master, mix, comp };
  }

  // ---------- the page: when to play, fade, pause and stop ----------
  // The music starts as soon as the landing is up and the age notice is answered: at page load for a
  // returning visitor, with the "I am 18 or older" click for a new one. A browser that refuses sound before
  // any interaction leaves the context suspended, and the first press, touch or key resumes it.
  const songs = {};
  const listeners = [];
  const S = { ctx: null, eng: null, cur: null, old: [], timer: 0, gestured: false, dead: false, want: '', loading: 0, mem: null };
  const emit = (what, arg) => { for (const fn of listeners) { try { fn(what, arg); } catch { /* a listener must not stop the music */ } } };

  const saveData = () => {
    try { return matchMedia('(prefers-reduced-data: reduce)').matches || !!(navigator.connection && navigator.connection.saveData); } catch { return false; }
  };
  function enabled() {
    let v = null;
    try { v = localStorage.getItem(KEY); } catch { /* no storage: remembered for this page only */ }
    if (v === null) v = S.mem;
    if (v === null) return !saveData();
    return v === '1';
  }
  // Only on the adventure landing (not ?classic=1, not the game view).
  const landing = () => typeof Adv !== 'undefined' && !!Adv.active && ['setup', 'dialing'].includes(document.body.dataset.view);
  // Silent until the age and terms notice has been answered yes.
  const blocked = () => typeof Site !== 'undefined' && !(Site.ok && Site.ok());
  // Same test as the scenes use for their dusk palette.
  const isDusk = () => { const t = document.documentElement.dataset.theme; return t ? t === 'dark' : !matchMedia('(prefers-color-scheme: light)').matches; };
  const canPlay = () => S.gestured && enabled() && !S.dead && landing() && !blocked() && !document.hidden;

  function ensureCtx() {
    if (S.ctx) return true;
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) { S.dead = true; return false; }
    try {
      S.ctx = new AC({ latencyHint: 'playback' });
      S.eng = engine(S.ctx, S.ctx.destination);
    } catch { S.ctx = null; S.dead = true; return false; }
    return true;
  }

  function load(id) {
    if (songs[id]) return Promise.resolve(songs[id]);
    return new Promise((resolve, reject) => {
      const sc = document.createElement('script');
      sc.src = innAsset(`music/${id}.js`);
      sc.onload = () => (songs[id] ? resolve(songs[id]) : reject(new Error(`no song ${id}`)));
      sc.onerror = () => reject(new Error(`could not load music/${id}.js`));
      document.head.append(sc);
    });
  }

  function tick() {
    if (!S.ctx) return;
    const now = S.ctx.currentTime;
    const live = [S.cur, ...S.old].filter(Boolean);
    for (const p of live) p.P.schedule(now + AHEAD);
    S.old = S.old.filter((p) => { if (now < p.P.stopAt) return true; p.P.dispose(); return false; });
    if (!S.cur && !S.old.length) { clearInterval(S.timer); S.timer = 0; }
  }
  const startTicker = () => { if (!S.timer) S.timer = setInterval(tick, TICK_MS); };

  function retire(p, secs) {
    p.P.fadeOut(secs);
    S.old.push(p);
    while (S.old.length > 2) S.old.shift().P.dispose();
  }

  // Bring the sound in line with what is wanted now: the right song for the scene, or none.
  async function sync() {
    if (!S.ctx || S.dead) return;
    if (!canPlay()) {
      if (S.cur && !enabled()) { retire(S.cur, .4); S.cur = null; startTicker(); }
      return;
    }
    if (S.ctx.state === 'suspended') S.ctx.resume().catch(() => {});
    const id = S.want || (typeof InnScenes !== 'undefined' && InnScenes.id()) || 'town';
    if (S.cur && S.cur.id === id) return;
    const ticket = ++S.loading;
    let song;
    try { song = await load(id); } catch (e) { console.warn(String(e)); return; }
    if (ticket !== S.loading || S.dead || !canPlay()) return;
    const fresh = !S.cur;
    if (S.cur) retire(S.cur, FADE_S);
    S.cur = { id, P: S.eng.player(song, { fadeIn: fresh ? .4 : FADE_S, dusk: isDusk }) };
    startTicker();
    emit('track', song);
  }

  // Try to start now; with autoplay refused the context waits, suspended, for a gesture.
  function start() {
    if (S.gestured || S.dead) return;
    if (blocked() || !landing()) return;
    S.gestured = true;
    if (enabled() && ensureCtx()) sync();
  }

  function gesture() {
    if (S.dead) return;
    if (!S.gestured) { start(); return; }
    if (S.ctx && S.ctx.state === 'suspended' && canPlay()) S.ctx.resume().catch(() => {});
  }

  function toggle(on) {
    on = on === undefined ? !enabled() : !!on;
    try { localStorage.setItem(KEY, on ? '1' : '0'); } catch { /* kept for this page */ }
    S.mem = on ? '1' : '0';
    if (on) {
      S.gestured = true;      // the click or key that got here is the gesture
      if (!S.dead && ensureCtx()) sync();
    } else {
      sync();
    }
    emit('state', on);
    return on;
  }

  // Fade out, then stop and close the context. The game has its own sound; the two never overlap.
  function leave(ms = 1500) {
    if (S.dead) return;
    S.dead = true;
    S.loading++;
    emit('gone');
    const ctx = S.ctx;
    if (!ctx) return;
    const done = () => {
      clearInterval(S.timer); S.timer = 0;
      for (const p of [S.cur, ...S.old]) if (p) p.P.dispose();
      S.cur = null; S.old = [];
      S.ctx = null;
      ctx.close().catch(() => {});
    };
    if (ctx.state !== 'running') return done();
    const t = ctx.currentTime;
    S.eng.master.gain.cancelScheduledValues(t);
    S.eng.master.gain.setValueAtTime(S.eng.master.gain.value, t);
    S.eng.master.gain.linearRampToValueAtTime(0, t + ms / 1000);
    for (const p of [S.cur, ...S.old]) if (p) p.P.endAt = t;
    setTimeout(done, ms + 80);
  }

  function setScene(id) {
    if (!id || id === S.want) return;
    S.want = id;
    sync();
  }

  const typing = (e) => {
    const t = e.target;
    return !!t && (/^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName) || !!t.isContentEditable);
  };
  const hooks = { guard: () => false };

  function wire() {
    // Capture sees clicks that a menu stops; the bubble pass catches the age notice's own button, which
    // answers the notice (and so unblocks us) in its handler.
    document.addEventListener('click', gesture, true);
    document.addEventListener('click', gesture);
    // A press or touch counts as soon as it lands (the browser's own activation rule), before any click.
    document.addEventListener('pointerdown', gesture, true);
    document.addEventListener('touchend', gesture, true);
    document.addEventListener('keydown', (e) => {
      if (e.ctrlKey || e.metaKey || e.altKey || ['Shift', 'Control', 'Alt', 'Meta', 'CapsLock'].includes(e.key)) return;
      if ((e.key === 'm' || e.key === 'M') && !e.repeat) {
        if (!landing() || blocked() || S.dead || typing(e) || hooks.guard()) return;
        toggle();
        return;
      }
      gesture();
    }, true);
    document.addEventListener('visibilitychange', () => {
      if (!S.ctx) return;
      if (document.hidden) S.ctx.suspend().catch(() => {});
      else if (canPlay()) S.ctx.resume().catch(() => {});
    });
    // Leaving the landing (the game view) or meeting the refusal screen ends the music for good.
    new MutationObserver(() => {
      if (S.dead) return;
      if (!landing()) leave(300);
      else if (typeof Site !== 'undefined' && Site.refused && Site.refused()) leave(300);
      else start();
    }).observe(document.body, { attributes: true, attributeFilter: ['data-view', 'class'] });
    // The landing (adv.js) and the stored age answer (site.js, asynchronous) come after this script: keep
    // trying for a few seconds after load, then leave it to the gestures.
    const retry = (n) => { start(); if (!S.gestured && !S.dead && n > 0) setTimeout(() => retry(n - 1), 250); };
    if (document.readyState === 'complete') retry(40);
    else window.addEventListener('load', () => retry(40));
  }
  if (typeof document !== 'undefined' && document.body) wire();
  else if (typeof document !== 'undefined') document.addEventListener('DOMContentLoaded', wire);

  return { engine, compile, parseMml, PATCH, songs, KEY, MAXV, enabled, toggle, leave, setScene, hooks,
    on: (fn) => listeners.push(fn), song: (def) => { songs[def.id] = def; }, playing: () => !!S.cur, current: () => (S.cur ? S.cur.id : ''), ctx: () => S.ctx };
})();
