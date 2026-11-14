// The browser build's sound output (src/platform/web/web_audio.cpp): an AudioWorklet that plays the frames the game's
// mixer thread writes into a ring in the game's shared memory. It reads the ring directly, on the browser's audio
// thread, and wakes the mixer (Atomics.notify) when it has taken frames. A ring at another sample rate than the
// browser's audio is resampled here (linearly).
//
// The ring's header (32-bit words, web_audio.cpp AudioRingHeader): 0 write position, 1 read position, 2 wake, 3
// glitches, 4 rendering, 5 capacity (frames), 6 channels, 7 sample rate, 8 pull, 9 connected, 10 stopped; the
// interleaved float frames start 64 bytes in.

class Bo1AudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.header = null;
    this.frames = null;
    this.frac = 0; // (resampling: how far past the read position the next output frame is, in ring frames)
    this.port.onmessage = (e) => {
      const d = e.data;
      if (d.cmd === 'ring') {
        this.header = new Int32Array(d.buffer, d.ptr, 16);
        this.capacity = this.header[5];
        this.channels = this.header[6];
        this.step = this.header[7] > 0 ? this.header[7] / sampleRate : 1; // ring frames per output frame
        this.frames = new Float32Array(d.buffer, d.ptr + 64, this.capacity * this.channels);
        this.frac = 0;
        Atomics.store(this.header, 9, 1);
      } else if (d.cmd === 'stop') {
        // (the game frees the ring once this says it is no longer read: AudioOut_Close)
        const h = this.header;
        this.header = null;
        this.frames = null;
        if (h) {
          Atomics.store(h, 9, 0);
          Atomics.store(h, 10, 1);
          Atomics.notify(h, 10);
        }
      }
    };
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const h = this.header;
    if (!h || !out || !out.length) return true;
    const n = out[0].length;
    const ch = this.channels, cap = this.capacity, frames = this.frames;
    const w = Atomics.load(h, 0), r = Atomics.load(h, 1);
    const avail = (w - r) & 0x7FFFFFFF;
    const outCh = out.length;
    let taken = 0, done = 0;
    if (this.step === 1) {
      done = taken = Math.min(avail, n);
      for (let i = 0; i < done; ++i) {
        const base = ((r + i) & 0x7FFFFFFF) % cap * ch;
        for (let c = 0; c < outCh; ++c) out[c][i] = c < ch ? frames[base + c] : 0;
      }
    } else {
      // output frame i lies at frac + i * step ring frames past the read position, between two ring frames
      const step = this.step, frac = this.frac;
      const need = Math.floor(frac + step * (n - 1)) + 2;
      if (avail >= need) {
        for (let i = 0; i < n; ++i) {
          const p = frac + i * step;
          const i0 = Math.floor(p), t = p - i0;
          const a = ((r + i0) & 0x7FFFFFFF) % cap * ch, b = ((r + i0 + 1) & 0x7FFFFFFF) % cap * ch;
          for (let c = 0; c < outCh; ++c) out[c][i] = c < ch ? frames[a + c] + (frames[b + c] - frames[a + c]) * t : 0;
        }
        const end = frac + step * n;
        taken = Math.floor(end);
        this.frac = end - taken;
        done = n;
      }
    }
    for (let c = 0; c < outCh; ++c)
      for (let i = done; i < n; ++i) out[c][i] = 0;
    if (done < n && Atomics.load(h, 4)) Atomics.add(h, 3, 1);
    Atomics.store(h, 1, (r + taken) & 0x7FFFFFFF);
    Atomics.store(h, 8, Math.ceil(n * this.step));
    Atomics.add(h, 2, 1);
    Atomics.notify(h, 2);
    return true;
  }
}

registerProcessor('bo1-audio', Bo1AudioProcessor);
