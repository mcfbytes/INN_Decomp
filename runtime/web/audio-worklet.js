// The browser's audio sink (docs/web.md, "Sound"): an AudioWorklet that plays the frames the runtime posts
// from the main thread, so a busy main thread no longer stops the output between its turns.
// Messages in: an Int16Array of interleaved stereo frames (transferred). Messages out, about every 10 ms:
// { taken, starved }: the frames that have left the queue so far (played, or dropped when it was full) and
// the render quanta that found it short once playing had begun.
'use strict';

const RING_SECONDS = 1;
const REPORT_QUANTA = 4;

class InnSink extends AudioWorkletProcessor {
  constructor() {
    super();
    this.cap = Math.ceil(sampleRate * RING_SECONDS);
    this.left = new Float32Array(this.cap);
    this.right = new Float32Array(this.cap);
    this.read = 0;
    this.count = 0;
    this.taken = 0;
    this.started = false;
    this.starved = 0;
    this.quanta = 0;
    this.port.onmessage = (e) => this.push(e.data);
  }

  push(frames) {
    if (!(frames instanceof Int16Array)) return;
    const n = frames.length >> 1;
    for (let i = 0; i < n; i++) {
      if (this.count === this.cap) { // full: the oldest frame goes, as in the runtime's own ring
        this.read = (this.read + 1) % this.cap;
        this.count--;
        this.taken++;
      }
      const w = (this.read + this.count) % this.cap;
      this.left[w] = frames[2 * i] / 32768;
      this.right[w] = frames[2 * i + 1] / 32768;
      this.count++;
    }
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const l = out[0], r = out[1] || out[0];
    const want = l.length;
    const n = Math.min(want, this.count);
    for (let i = 0; i < n; i++) {
      l[i] = this.left[this.read];
      r[i] = this.right[this.read];
      this.read = (this.read + 1) % this.cap;
    }
    for (let i = n; i < want; i++) l[i] = r[i] = 0;
    this.count -= n;
    this.taken += n;
    if (n) this.started = true;
    else if (this.started) this.starved++;
    if (n && n < want) this.starved++;
    if (++this.quanta % REPORT_QUANTA === 0) this.port.postMessage({ taken: this.taken, starved: this.starved });
    return true;
  }
}

registerProcessor('inn-sink', InnSink);
