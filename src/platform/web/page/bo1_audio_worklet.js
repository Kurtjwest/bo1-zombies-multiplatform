// The browser build's sound output (src/platform/web/web_audio.cpp): an AudioWorklet that plays the frames the game's
// mixer thread writes into a ring in the game's shared memory. It reads the ring directly, on the browser's audio
// thread, and wakes the mixer (Atomics.notify) when it has taken frames.
//
// The ring's header (32-bit words, web_audio.cpp AudioRingHeader): 0 write position, 1 read position, 2 wake, 3
// glitches, 4 rendering, 5 capacity (frames), 6 channels, 7 sample rate, 8 pull, 9 connected; the interleaved float
// frames start 64 bytes in.

class Bo1AudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.header = null;
    this.frames = null;
    this.port.onmessage = (e) => {
      const d = e.data;
      if (d.cmd === 'ring') {
        this.header = new Int32Array(d.buffer, d.ptr, 16);
        this.capacity = this.header[5];
        this.channels = this.header[6];
        this.frames = new Float32Array(d.buffer, d.ptr + 64, this.capacity * this.channels);
        Atomics.store(this.header, 9, 1);
      } else if (d.cmd === 'stop') {
        if (this.header) Atomics.store(this.header, 9, 0);
        this.header = null;
      }
    };
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const h = this.header;
    if (!h || !out || !out.length) return true;
    const n = out[0].length;
    const ch = this.channels, cap = this.capacity;
    const w = Atomics.load(h, 0), r = Atomics.load(h, 1);
    const avail = (w - r) & 0x7FFFFFFF;
    const take = Math.min(avail, n);
    const outCh = out.length;
    for (let i = 0; i < take; ++i) {
      const base = ((r + i) & 0x7FFFFFFF) % cap * ch;
      for (let c = 0; c < outCh; ++c) out[c][i] = c < ch ? this.frames[base + c] : 0;
    }
    for (let c = 0; c < outCh; ++c)
      for (let i = take; i < n; ++i) out[c][i] = 0;
    if (take < n && Atomics.load(h, 4)) Atomics.add(h, 3, 1);
    Atomics.store(h, 1, (r + take) & 0x7FFFFFFF);
    Atomics.store(h, 8, n);
    Atomics.add(h, 2, 1);
    Atomics.notify(h, 2);
    return true;
  }
}

registerProcessor('bo1-audio', Bo1AudioProcessor);
