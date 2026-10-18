// GPU display filters for the game picture (docs/web.md, "Display filters").
//
// The emulator draws every frame into a 32-bit buffer. With Sharp or Smooth the SDL canvas shows it as before.
// With any other filter this module takes over: the frontend (frontend_sdl.c) hands it the buffer through
// inn_gpu_frame, it is uploaded to a texture and a chain of fragment-shader passes writes the result to its own
// canvas (#glcanvas) laid exactly over #canvas, which keeps all the input and the focus.
//
// The shaders are third-party libretro-style GLSL files kept as published under third_party/ and fetched on first
// use. This file supplies what they expect (the VERTEX / FRAGMENT split, VertexCoord, TexCoord, MVPMatrix,
// Texture, TextureSize, InputSize, OutputSize, OrigTexture, PassPrevNTexture, LUT and the #pragma parameters) and
// a small reader for the mpv-hook style used by the line-art upscaler. Nothing here is evaluated as script.
'use strict';

const Filters = (() => {
  const GROUPS = ['Plain', 'Pixel art', 'Smooth upscalers', 'Monitor', 'Experimental'];

  // One entry per choice in Settings. `passes` is a function of the picture and output sizes because a few
  // filters pick their scale from them. A pass: file (under shaders/), scale ('viewport' or a factor of its input),
  // linear (how its input is sampled), float (16 or 32: a floating-point target), lut (a PNG), params (overrides),
  // shift (how far the filter moves the picture, in source pixels, measured with tools/filter_test: the resample
  // that follows moves it back).
  const hq = (fw, fh, ow) => {
    const n = ow / fw < 2.5 ? 2 : ow / fw < 3.5 ? 3 : 4;
    return [{ file: 'hqx-pass1.glsl', scale: 1 },
            { file: 'hqx-pass2.glsl', scale: n, lut: `hq${n}x.png`, params: { SCALE: n } }];
  };
  const DEFS = [
    { id: 'sharp', label: 'Sharp', group: 0, gpu: false,
      desc: 'Hard pixel edges, no blur. The default.' },
    { id: 'smooth', label: 'Smooth', group: 0, gpu: false,
      desc: 'Soft edges: the picture is stretched with plain interpolation.' },
    { id: 'xbr2', heavy: true, label: 'xBR', group: 1, passes: () => [{ file: 'xbr-lv2.glsl', scale: 'viewport' }],
      desc: 'Redraws diagonal and curved edges of pixel art as clean lines. Keeps most fine detail.' },
    { id: 'xbr3', heavy: true, label: 'xBR, strong', group: 1, passes: () => [{ file: 'xbr-lv3.glsl', scale: 'viewport' }],
      desc: 'Like xBR but rounder: smooths more of the stair steps, at the price of some detail.' },
    { id: 'xbrz', heavy: true, label: 'xBRZ', group: 1, passes: () => [{ file: 'xbrz-freescale.glsl', scale: 'viewport' }],
      desc: 'Edge smoothing that keeps colours and shapes steady. Good with text and icons.' },
    { id: 'scalefx', label: 'ScaleFX', group: 1, needFloat: 32,
      passes: () => [
        { file: 'scalefx-pass0.glsl', scale: 1, float: 32 }, { file: 'scalefx-pass1.glsl', scale: 1, float: 32 },
        { file: 'scalefx-pass2.glsl', scale: 1 }, { file: 'scalefx-pass3.glsl', scale: 1 },
        { file: 'scalefx-pass4.glsl', scale: 3 }],
      desc: 'Finds the shapes in the picture and redraws their outlines. Crisp corners, very little blur.' },
    { id: 'hqx', label: 'HQx', group: 1, passes: hq,
      desc: 'The classic HQ2x, HQ3x and HQ4x filters (picked to suit the window size). Soft, smooth shading.' },
    { id: 'sai2', label: '2xSaI', group: 1, passes: () => [{ file: '2xsai.glsl', scale: 2, shift: [-0.25, -0.25] }],
      desc: 'The old emulator favourite: smooths diagonals and blends the colours a little.' },
    { id: 'supersai', label: 'Super 2xSaI', group: 1, passes: () => [{ file: 'super-2xsai.glsl', scale: 2, shift: [-0.25, -0.5] }],
      desc: 'A stronger relative of 2xSaI with softer, rounder curves.' },
    { id: 'supereagle', label: 'Super Eagle', group: 1, passes: () => [{ file: 'supereagle.glsl', scale: 2, shift: [-0.5, -0.5] }],
      desc: 'Smooths corners and diagonals, keeping the picture bright and fairly sharp.' },
    { id: 'fsr', heavy: true, label: 'FSR 1', group: 2, linear: true,
      passes: () => [{ file: 'fsr-pass0.glsl', scale: 'viewport', linear: true },
                     { file: 'fsr-pass1.glsl', scale: 1, linear: true }],
      desc: 'AMD FidelityFX Super Resolution 1: a sharp, edge-aware enlarger plus a light sharpen.' },
    { id: 'crt', heavy: true, label: 'CRT', group: 3, noScan: true, passes: () => [{ file: 'crt-lottes.glsl', scale: 'viewport' }],
      desc: 'An old monitor: scanlines, a phosphor mask, glow and a slightly curved glass.' },
    { id: 'anime4k', label: 'Line-art enhancer', group: 4, needFloat: 16, mpv: 'Anime4K_Upscale_CNN_x2_S.glsl',
      desc: 'Experimental. A small neural network that doubles the picture and sharpens lines. Heavy on slow GPUs.' },
  ];
  const byId = Object.fromEntries(DEFS.map((d) => [d.id, d]));

  // ---- the shaders we write ourselves ----

  const STOCK_VERT = `in vec4 VertexCoord; in vec4 TexCoord; out vec2 vTex; uniform mat4 MVPMatrix;
void main() { gl_Position = MVPMatrix * VertexCoord; vTex = TexCoord.xy; }`;
  const FRAG_HEAD = '#version 300 es\nprecision highp float; precision highp int;\n';
  // The emulator's pixels are B, G, R, A bytes; this puts them in order. Texel centres are sampled exactly.
  const CONVERT_FRAG = `${FRAG_HEAD}in vec2 vTex; out vec4 FragColor; uniform sampler2D Texture;
void main() { vec4 t = texture(Texture, vTex); FragColor = vec4(t.b, t.g, t.r, 1.0); }`;
  const STOCK_FRAG = `${FRAG_HEAD}in vec2 vTex; out vec4 FragColor; uniform sampler2D Texture; uniform vec2 TextureSize; uniform vec2 Shift;
void main() { FragColor = vec4(texture(Texture, vTex - Shift / TextureSize).rgb, 1.0); }`;
  // Scanlines for any filter: one dark gap per source line, fading out when the lines get too thin to show.
  const SCAN_FRAG = `${FRAG_HEAD}in vec2 vTex; out vec4 FragColor; uniform sampler2D Texture;
uniform vec2 OutputSize; uniform vec2 OrigTextureSize;
void main() {
  vec3 c = texture(Texture, vTex).rgb;
  float lines = OrigTextureSize.y, per = OutputSize.y / lines;
  float k = 0.42 * clamp((per - 2.0) * 0.5, 0.0, 1.0);
  float s = 0.5 + 0.5 * cos(6.2831853 * (vTex.y * lines - 0.5));
  float w = (1.0 - k) + k * pow(s, 0.7);
  FragColor = vec4(c * w * (1.0 + 0.32 * k), 1.0);
}`;
  const IDENTITY = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
  const FLIP_Y = new Float32Array([1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);

  // ---- state ----

  let canvas = null, gl = null, vao = null, samplerNear = null, samplerLin = null, ext = {};
  let supportInfo = null;
  let cur = null;          // the active chain: { def, scan, files, texts, passes, built }
  let raw = null;          // the uploaded frame, as the emulator wrote it
  let src = null;          // the frame in RGBA order: the first input of every chain
  let frameW = 0, frameH = 0;
  let outW = 0, outH = 0;  // the canvas in device pixels
  let needFrame = false, shown = false, frameCount = 0, onLost = null;
  const programs = new Map();
  const luts = new Map();
  const stats = { draws: 0, uploads: 0 };
  // Filters that do a lot of work for every output pixel (heavy: true above) draw at most this many pixels; the browser
  // stretches the canvas the rest of the way. A 1440p window is 2.8 million, 4K 6.2 million.
  let capPixels = 5e6;

  function support() {
    if (supportInfo) return supportInfo;
    const info = { webgl2: false, float32: false, float16: false, reason: '' };
    try {
      const c = document.createElement('canvas');
      const g = c.getContext('webgl2', { failIfMajorPerformanceCaveat: false });
      if (g) {
        info.webgl2 = true;
        const cbf = g.getExtension('EXT_color_buffer_float');
        info.float32 = info.float16 = !!cbf;
        if (!cbf && g.getExtension('EXT_color_buffer_half_float')) info.float16 = true;
        const lose = g.getExtension('WEBGL_lose_context');
        if (lose) lose.loseContext();
      }
    } catch { /* no WebGL */ }
    if (!info.webgl2) info.reason = 'This browser cannot do WebGL 2.';
    supportInfo = info;
    return info;
  }

  // Why a choice cannot be used here, or '' when it can.
  function why(id) {
    const d = byId[id];
    if (!d || d.gpu === false) return '';
    const s = support();
    if (!s.webgl2) return s.reason;
    if (d.needFloat === 32 && !s.float32) return 'Needs floating-point render targets, which this graphics driver lacks.';
    if (d.needFloat === 16 && !s.float16) return 'Needs floating-point render targets, which this graphics driver lacks.';
    return '';
  }

  // ---- GL setup ----

  function ensureContext() {
    if (gl && !gl.isContextLost()) return true;
    canvas = document.getElementById('glcanvas');
    if (!canvas) return false;
    gl = canvas.getContext('webgl2', { alpha: false, antialias: false, depth: false, stencil: false,
                                       preserveDrawingBuffer: false, powerPreference: 'high-performance' });
    if (!gl) return false;
    canvas.addEventListener('webglcontextlost', (e) => { e.preventDefault(); lose('The graphics context was lost.'); });
    ext.cbf = gl.getExtension('EXT_color_buffer_float');
    if (!ext.cbf) gl.getExtension('EXT_color_buffer_half_float');
    vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    const buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    // x y 0 1 | u v 0 0, as a strip. v = 0 is the picture's first row in every texture, so the last pass flips.
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
      -1, -1, 0, 1, 0, 0, 0, 0,
       1, -1, 0, 1, 1, 0, 0, 0,
      -1,  1, 0, 1, 0, 1, 0, 0,
       1,  1, 0, 1, 1, 1, 0, 0]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 4, gl.FLOAT, false, 32, 0);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 32, 16);
    samplerNear = gl.createSampler();
    samplerLin = gl.createSampler();
    for (const [s, f] of [[samplerNear, gl.NEAREST], [samplerLin, gl.LINEAR]]) {
      gl.samplerParameteri(s, gl.TEXTURE_MIN_FILTER, f);
      gl.samplerParameteri(s, gl.TEXTURE_MAG_FILTER, f);
      gl.samplerParameteri(s, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.samplerParameteri(s, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    }
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.BLEND);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    return true;
  }

  function lose(reason) {
    // Everything on the old context is gone; the page falls back to the plain canvas.
    programs.clear(); luts.clear();
    cur = null; raw = null; src = null; gl = null; shown = false;
    setShown(false);
    if (onLost) onLost(reason);
  }

  function setShown(on) {
    shown = on;
    const stage = document.getElementById('stage');
    if (stage) stage.dataset.gpu = on ? '1' : '0';
  }

  function makeTexture(w, h, fmt) {
    const t = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t);
    if (fmt === 32) gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, w, h, 0, gl.RGBA, gl.FLOAT, null);
    else if (fmt === 16) gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA16F, w, h, 0, gl.RGBA, gl.HALF_FLOAT, null);
    else gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    // The samplers set the filters, but a texture with default (mipmapped) parameters would be incomplete.
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    return t;
  }

  function makeTarget(w, h, fmt) {
    const tex = makeTexture(w, h, fmt);
    const fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
    const status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    if (status !== gl.FRAMEBUFFER_COMPLETE) throw new Error(`render target ${w}x${h} (format ${fmt}) is not usable`);
    return { tex, fbo, w, h, fmt };
  }

  function freeTarget(t) {
    if (!t) return;
    gl.deleteTexture(t.tex);
    if (t.fbo) gl.deleteFramebuffer(t.fbo);
  }

  // ---- programs ----

  function compile(type, text, label) {
    const s = gl.createShader(type);
    gl.shaderSource(s, text);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
      const log = gl.getShaderInfoLog(s);
      gl.deleteShader(s);
      throw new Error(`${label}: ${log}`);
    }
    return s;
  }

  function link(vs, fs, label) {
    const p = gl.createProgram();
    gl.attachShader(p, vs);
    gl.attachShader(p, fs);
    gl.bindAttribLocation(p, 0, 'VertexCoord');
    gl.bindAttribLocation(p, 1, 'TexCoord');
    gl.bindAttribLocation(p, 2, 'COLOR');
    gl.bindAttribLocation(p, 2, 'Color');
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(`${label}: ${gl.getProgramInfoLog(p)}`);
    gl.deleteShader(vs);
    gl.deleteShader(fs);
    const uniforms = [];
    const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
    for (let i = 0; i < n; i++) {
      const u = gl.getActiveUniform(p, i);
      uniforms.push({ name: u.name.replace(/\[0\]$/, ''), type: u.type, loc: gl.getUniformLocation(p, u.name) });
    }
    return { prog: p, uniforms };
  }

  // A libretro-style file: one source with #if defined(VERTEX) / #elif defined(FRAGMENT) halves. We drop its
  // #version line (they say 130), give it ES 3.00 and make every precision highp: its mediump is only a
  // mobile hint, and fp16 coordinates would shift the picture.
  function libretroProgram(file, text) {
    const key = `lr:${file}`;
    if (programs.has(key)) return programs.get(key);
    const body = text.replace(/^\s*#version[^\n]*\n/m, '\n').replace(/\bmediump\b/g, 'highp').replace(/^(\s*)#pragma\s+parameter/gm, '$1//#pragma parameter');
    const head = (stage) => `#version 300 es\n#define ${stage}\n#define PARAMETER_UNIFORM\n${stage === 'FRAGMENT' ? 'precision highp float;\nprecision highp int;\n' : ''}`;
    const params = {};
    for (const m of text.matchAll(/^\s*#pragma\s+parameter\s+(\w+)\s+"[^"]*"\s+(\S+)/gm)) params[m[1]] = parseFloat(m[2]);
    const prog = link(compile(gl.VERTEX_SHADER, head('VERTEX') + body, `${file} (vertex)`),
                      compile(gl.FRAGMENT_SHADER, head('FRAGMENT') + body, `${file} (fragment)`), file);
    prog.params = params;
    programs.set(key, prog);
    return prog;
  }

  function ownProgram(key, frag) {
    if (programs.has(key)) return programs.get(key);
    const prog = link(compile(gl.VERTEX_SHADER, `#version 300 es\nprecision highp float;\n${STOCK_VERT}`, `${key} (vertex)`),
                      compile(gl.FRAGMENT_SHADER, frag, `${key} (fragment)`), key);
    prog.params = {};
    programs.set(key, prog);
    return prog;
  }

  // ---- the mpv-hook shader format (Anime4K) ----

  function parseMpv(text) {
    const out = [];
    let cur = null;
    for (const line of text.split('\n')) {
      const d = /^\/\/!(\w+)\s*(.*)$/.exec(line);
      if (d) {
        if (d[1] === 'DESC') { cur = { desc: d[2], binds: [], save: 'MAIN', width: 'MAIN.w', height: 'MAIN.h', body: [] }; out.push(cur); }
        else if (cur && d[1] === 'BIND') cur.binds.push(d[2].trim());
        else if (cur && d[1] === 'SAVE') cur.save = d[2].trim();
        else if (cur && d[1] === 'WIDTH') cur.width = d[2].trim();
        else if (cur && d[1] === 'HEIGHT') cur.height = d[2].trim();
        continue;
      }
      if (cur && !/^\s*\/\/(?!!)/.test(line)) cur.body.push(line);
    }
    return out;
  }

  function mpvProgram(file, i, pass) {
    const key = `mpv:${file}:${i}`;
    if (programs.has(key)) return programs.get(key);
    let head = `${FRAG_HEAD}in vec2 vTex; out vec4 FragColor;\n#define vTexCoord vTex\n`;
    for (const b of pass.binds) {
      head += `uniform sampler2D ${b}_s; uniform vec2 ${b}_size;\n`
        + `#define ${b}_pos vTexCoord\n#define ${b}_pt (1.0 / ${b}_size)\n`
        + `vec4 ${b}_tex(vec2 p) { return texture(${b}_s, p); }\n`
        + `vec4 ${b}_texOff(vec2 o) { return ${b}_tex(vTexCoord + o / ${b}_size); }\n`;
    }
    const frag = `${head}${pass.body.join('\n')}\nvoid main() { FragColor = hook(); }\n`;
    const prog = ownProgram(key, frag);
    return prog;
  }

  // WIDTH / HEIGHT: a reverse-Polish expression such as "conv2d_last_tf.w 2 *".
  function mpvSize(expr, named) {
    const st = [];
    for (const t of expr.split(/\s+/)) {
      if (t === '*') st.push(st.pop() * st.pop());
      else if (t === '+') st.push(st.pop() + st.pop());
      else if (t === '/') { const b = st.pop(), a = st.pop(); st.push(a / b); }
      else if (t === '-') { const b = st.pop(), a = st.pop(); st.push(a - b); }
      else if (/^[\w]+\.[wh]$/.test(t)) { const [n, c] = t.split('.'); st.push(named[n][c]); }
      else st.push(parseFloat(t));
    }
    return Math.max(1, Math.round(st.pop()));
  }

  // ---- building a chain ----

  async function fetchText(file) {
    const r = await fetch(innAsset(`shaders/${file}`));
    if (!r.ok) throw new Error(`could not load ${file}`);
    return r.text();
  }

  async function fetchLut(file) {
    if (luts.has(file)) return luts.get(file);
    const r = await fetch(innAsset(`shaders/${file}`));
    if (!r.ok) throw new Error(`could not load ${file}`);
    const bmp = await createImageBitmap(await r.blob(), { premultiplyAlpha: 'none', colorSpaceConversion: 'none' });
    luts.set(file, bmp);
    return bmp;
  }

  // The list of passes for the current picture and output sizes, with sizes worked out. Each: { kind, file, text,
  // w, h, linear, fmt, lut, params, binds, save }
  function plan(c) {
    const def = c.def, list = [];
    const fw = frameW, fh = frameH;
    let w = fw, h = fh, shift = null;
    if (def.mpv) {
      const named = { MAIN: { w: fw, h: fh } };
      c.mpv.forEach((p, i) => {
        const pw = mpvSize(p.width, named), ph = mpvSize(p.height, named);
        list.push({ kind: 'mpv', file: def.mpv, index: i, mp: p, w: pw, h: ph, fmt: i === c.mpv.length - 1 ? 0 : 16, linear: false });
        named[p.save] = { w: pw, h: ph };
        w = pw; h = ph;
      });
    } else if (def.passes) {
      for (const p of def.passes(fw, fh, outW)) {
        const pw = p.scale === 'viewport' ? outW : Math.max(1, Math.round(w * p.scale));
        const ph = p.scale === 'viewport' ? outH : Math.max(1, Math.round(h * p.scale));
        list.push({ kind: 'lr', file: p.file, w: pw, h: ph, linear: !!p.linear, fmt: p.float || 0, lut: p.lut, params: p.params || {} });
        w = pw; h = ph;
        if (p.shift) shift = [-p.shift[0] * w / fw, -p.shift[1] * h / fh];
      }
    } else {
      list.push({ kind: 'stock', w: outW, h: outH, linear: def.id === 'smooth', fmt: 0 });
      w = outW; h = outH;
    }
    // Whatever the chain ends at is fitted to the output with plain interpolation.
    if (w !== outW || h !== outH || shift) list.push({ kind: 'stock', w: outW, h: outH, linear: true, fmt: 0, params: shift ? { Shift: shift } : {} });
    if (c.scan && !def.noScan) list.push({ kind: 'scan', w: outW, h: outH, linear: false, fmt: 0 });
    return list;
  }

  function neededFiles(def) {
    const files = new Set();
    if (def.mpv) files.add(def.mpv);
    else if (def.passes) {
      // hqx picks by size; fetch all three LUTs' passes (the shaders are the same two files).
      for (const sample of [def.passes(frameW || 320, frameH || 200, 960), def.passes(frameW || 320, frameH || 200, 1920)]) {
        for (const p of sample) { files.add(p.file); if (p.lut) files.add(p.lut); }
      }
      if (def.id === 'hqx') for (const n of [2, 3, 4]) files.add(`hq${n}x.png`);
    }
    return [...files];
  }

  async function load(def, scan) {
    const c = { def, scan, texts: {}, pass: [], bound: false };
    const files = neededFiles(def).filter((f) => !f.endsWith('.png'));
    const texts = await Promise.all(files.map(fetchText));
    files.forEach((f, i) => { c.texts[f] = texts[i]; });
    if (def.mpv) c.mpv = parseMpv(c.texts[def.mpv]);
    if (def.id === 'hqx') await Promise.all([2, 3, 4].map((n) => fetchLut(`hq${n}x.png`)));
    return c;
  }

  // Programs and render targets for the current sizes. Safe to call again after a resize: only what changed is redone.
  function build(c) {
    const specs = plan(c);
    const old = c.pass;
    const next = [];
    specs.forEach((s, i) => {
      let p = old[i];
      const same = p && p.kind === s.kind && p.file === s.file && p.index === s.index && p.lut === s.lut;
      if (!same) {
        if (p) freeTarget(p.target);
        p = { kind: s.kind, file: s.file, index: s.index, lut: s.lut, target: null };
        if (s.kind === 'lr') p.prog = libretroProgram(s.file, c.texts[s.file]);
        else if (s.kind === 'mpv') p.prog = mpvProgram(s.file, s.index, s.mp);
        else if (s.kind === 'stock') p.prog = ownProgram('stock', STOCK_FRAG);
        else p.prog = ownProgram('scan', SCAN_FRAG);
      }
      Object.assign(p, { w: s.w, h: s.h, linear: s.linear, fmt: s.fmt, params: s.params || {}, mp: s.mp });
      const last = i === specs.length - 1;
      if (last) { freeTarget(p.target); p.target = null; }
      else if (!p.target || p.target.w !== s.w || p.target.h !== s.h || p.target.fmt !== s.fmt) {
        freeTarget(p.target);
        p.target = makeTarget(s.w, s.h, s.fmt);
      }
      next.push(p);
    });
    for (let i = specs.length; i < old.length; i++) freeTarget(old[i].target);
    c.pass = next;
  }

  function setUniform(u, v) {
    switch (u.type) {
      case gl.FLOAT: gl.uniform1f(u.loc, v); break;
      case gl.INT: gl.uniform1i(u.loc, v); break;
      case gl.FLOAT_VEC2: gl.uniform2f(u.loc, v[0], v[1]); break;
      case gl.FLOAT_MAT4: gl.uniformMatrix4fv(u.loc, false, v); break;
      default: break;
    }
  }

  // ---- drawing ----

  function drawChain(c) {
    const passes = c.pass;
    // outs[0] is the picture; outs[i + 1] is pass i's result.
    const outs = [src];
    const named = { MAIN: src };
    gl.bindVertexArray(vao);
    for (let i = 0; i < passes.length; i++) {
      const p = passes[i];
      const last = i === passes.length - 1;
      const input = outs[i];
      gl.bindFramebuffer(gl.FRAMEBUFFER, last ? null : p.target.fbo);
      gl.viewport(0, 0, p.w, p.h);
      gl.useProgram(p.prog.prog);
      let unit = 0;
      const bindTex = (tex, lin) => {
        gl.activeTexture(gl.TEXTURE0 + unit);
        gl.bindTexture(gl.TEXTURE_2D, tex);
        gl.bindSampler(unit, lin ? samplerLin : samplerNear);
        return unit++;
      };
      for (const u of p.prog.uniforms) {
        if (!u.loc) continue;
        const n = u.name;
        if (u.type === gl.SAMPLER_2D) {
          let t = null, lin = false;
          if (p.kind === 'mpv') {
            const m = /^(\w+)_s$/.exec(n);
            if (m && named[m[1]]) t = named[m[1]].tex;
          } else if (n === 'Texture' || n === 'decal' || n === 'Source') { t = input.tex; lin = p.linear; }
          else if (n === 'Original' || n === 'OrigTexture') t = src.tex;
          else if (n === 'LUT') t = lutTexture(p.lut);
          else {
            const m = /^PassPrev(\d+)Texture$/.exec(n);
            if (m) t = (outs[Math.max(0, i + 1 - Number(m[1]))] || src).tex;
          }
          if (t) gl.uniform1i(u.loc, bindTex(t, lin));
          continue;
        }
        if (n === 'MVPMatrix') setUniform(u, last ? FLIP_Y : IDENTITY);
        else if (n === 'FrameCount') setUniform(u, frameCount);
        else if (n === 'FrameDirection') setUniform(u, 1);
        else if (n === 'OutputSize') setUniform(u, [p.w, p.h]);
        else if (n === 'TextureSize' || n === 'InputSize') setUniform(u, [input.w, input.h]);
        else if (n === 'OrigTextureSize' || n === 'OrigInputSize' || n === 'OriginalSize') setUniform(u, [src.w, src.h]);
        else if (p.kind === 'mpv' && /_size$/.test(n)) { const b = named[n.slice(0, -5)]; if (b) setUniform(u, [b.w, b.h]); }
        else {
          const m = /^PassPrev(\d+)(TextureSize|InputSize)$/.exec(n);
          if (m) { const o = outs[Math.max(0, i + 1 - Number(m[1]))] || src; setUniform(u, [o.w, o.h]); }
          else if (n in p.params) setUniform(u, p.params[n]);
          else if (n in p.prog.params) setUniform(u, p.prog.params[n]);
        }
      }
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      const result = last ? null : p.target;
      outs.push(result);
      if (p.kind === 'mpv') named[p.mp.save] = p.target || named[p.mp.save];
    }
    for (let u = 0; u < 8; u++) gl.bindSampler(u, null);
  }

  function lutTexture(file) {
    if (!file) return null;
    let t = lutTextures.get(file);
    if (!t) {
      t = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, t);
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, luts.get(file));
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
      lutTextures.set(file, t);
    }
    return t;
  }
  const lutTextures = new Map();

  // Convert the raw upload into the picture texture. Separate so a filter change or resize can redraw
  // without asking the emulator for another frame.
  function convert() {
    gl.bindVertexArray(vao);
    gl.bindFramebuffer(gl.FRAMEBUFFER, src.fbo);
    gl.viewport(0, 0, frameW, frameH);
    const p = ownProgram('convert', CONVERT_FRAG);
    gl.useProgram(p.prog);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, raw.tex);
    gl.bindSampler(0, samplerNear);
    for (const u of p.uniforms) {
      if (u.name === 'Texture') gl.uniform1i(u.loc, 0);
      else if (u.name === 'MVPMatrix') setUniform(u, IDENTITY);
    }
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.bindSampler(0, null);
  }

  function draw() {
    if (!gl || !cur || !src) return;
    convert();
    drawChain(cur);
    frameCount++;
    stats.draws++;
    if (!shown) setShown(true);
  }

  // The canvas size for a filter: the frame area at device pixels, within what the GPU allows and, for the heavy
  // filters, within capPixels.
  let want = null;
  function sizeFor(def) {
    let w = Math.max(1, Math.round(want.cssW * want.dpr)), h = Math.max(1, Math.round(want.cssH * want.dpr));
    let k = 1;
    if (gl) k = Math.min(gl.getParameter(gl.MAX_TEXTURE_SIZE), gl.getParameter(gl.MAX_RENDERBUFFER_SIZE)) / Math.max(w, h);
    if (def && def.heavy) k = Math.min(k, Math.sqrt(capPixels / (w * h)));
    if (k < 1) { w = Math.max(1, Math.floor(w * k)); h = Math.max(1, Math.floor(h * k)); }
    return [w, h];
  }

  function setOutput(w, h) {
    outW = w; outH = h;
    if (canvas && (canvas.width !== w || canvas.height !== h)) { canvas.width = w; canvas.height = h; }
  }

  function applyLayout() {
    if (!want) return;
    const [w, h] = sizeFor(cur && cur.def);
    if (w === outW && h === outH) return;
    setOutput(w, h);
    if (cur) { build(cur); if (raw) draw(); }
  }

  // ---- public ----

  return {
    list: DEFS.map(({ id, label, group, desc, gpu, noScan }) => ({ id, label, group: GROUPS[group], desc, gpu: gpu !== false, noScan: !!noScan })),
    stats,
    why,
    get active() { return !!cur; },
    get shown() { return shown; },
    set onLost(f) { onLost = f; },

    // Choose a filter. Resolves true when the GPU path is on, false when the plain canvas shows the picture
    // (Sharp and Smooth without scanlines). Rejects, leaving the previous choice in place, when it cannot work.
    async use(id, scan, force) {
      const def = byId[id];
      if (!def) throw new Error('unknown filter');
      if (def.gpu === false && !scan && !force) {
        this.release();
        return false;
      }
      const reason = why(id);
      if (reason) throw new Error(reason);
      if (!ensureContext()) throw new Error('This browser cannot do WebGL 2.');
      if (!frameW) { frameW = 320; frameH = 200; }
      const c = await load(def, !!scan);
      if (!src) src = makeTarget(frameW, frameH, 0);
      const [w, h] = want ? sizeFor(def) : [outW || 1, outH || 1];
      setOutput(w, h);
      build(c);   // compiles now, so a broken shader is reported here and not on the first frame
      const old = cur;
      cur = c;
      if (old) for (const p of old.pass) freeTarget(p.target);
      needFrame = true;
      return true;
    },

    release() {
      if (cur) for (const p of cur.pass) freeTarget(p.target);
      cur = null;
      setShown(false);
    },

    // The frame area changed (CSS pixels); output is the canvas at device-pixel size, within what the GPU allows.
    layout(cssW, cssH, dpr) {
      want = { cssW, cssH, dpr };
      applyLayout();
    },

    // For the frontend: 0 = nothing to do, 1 = send a frame when it changed, 2 = send the current frame now.
    wants() {
      if (!cur) return 0;
      return needFrame ? 2 : 1;
    },

    // heap is the wasm memory as bytes (HEAPU8); ptr/w/h describe the frame (B, G, R, A bytes per pixel).
    frame(heap, ptr, w, h) {
      if (!cur || !gl) return;
      if (gl.isContextLost()) { lose('The graphics context was lost.'); return; }
      if (w !== frameW || h !== frameH || !raw) {
        frameW = w; frameH = h;
        freeTarget(raw); freeTarget(src);
        raw = { tex: makeTexture(w, h, 0), w, h };
        src = makeTarget(w, h, 0);
        build(cur);
      }
      gl.bindTexture(gl.TEXTURE_2D, raw.tex);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, heap.subarray(ptr, ptr + w * h * 4));
      stats.uploads++;
      needFrame = false;
      draw();
    },

    // Redraw from the last upload (a test hook and the resize path). Returns false when there is nothing to draw.
    redraw() {
      if (!cur || !raw) return false;
      draw();
      return true;
    },

    // Test hook: wait for the GPU to finish `n` draws and return the mean milliseconds each took.
    async time(n) {
      if (!cur || !raw) return NaN;
      const px = new Uint8Array(4);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);   // reading back waits for everything queued
      const t0 = performance.now();
      for (let i = 0; i < n; i++) { draw(); gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px); }
      return (performance.now() - t0) / n;
    },

    // Test hook: set the picture from an array of 0xAARRGGBB pixels, as the frontend would.
    feed(argb, w, h) {
      const bytes = new Uint8Array(argb.buffer, argb.byteOffset, w * h * 4);
      this.frame(bytes, 0, w, h);
    },

    set capPixels(n) { capPixels = n; },
    sizes() { return { frameW, frameH, outW, outH }; },
  };
})();
