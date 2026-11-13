// The browser build's reader of the game files (src/platform/web/web_fs.cpp): a worker of the page that makes the
// requests the game's threads put in the fetch table of the shared block (web_shared.h FetchTable) and writes the
// bytes into the game's memory where each request says.
//
// Why a worker of its own: a browser frees what a request made only when the thread that made it returns to its event
// loop, and the game's threads never do, so a game thread reading the files itself would keep every byte it read.
// This worker returns to its event loop between requests, and serves several at once.
//
// A slot: 12 32-bit words (web_shared.h FetchSlot): 0 state, 1 URL address, 2 URL length, 3-4 offset (low, high),
// 5 length, 6 destination address, 7 result, 8 HTTP status. States: 2 requested, 3 running, 4 done.
'use strict';

const SLOTS = 16;
const SLOT_WORDS = 12;
const REQUESTED = 2, RUNNING = 3, DONE = 4;

let memory = null; // the game's WebAssembly.Memory (shared)
let table = 0;     // the fetch table's address
let i32 = null;
const decoder = new TextDecoder();

function words() {
  // (the memory grows: a view made earlier doesn't reach what was allocated since)
  if (!i32 || i32.buffer.byteLength !== memory.buffer.byteLength) i32 = new Int32Array(memory.buffer);
  return i32;
}

async function serve(slot) {
  const base = (table >>> 2) + 4 + slot * SLOT_WORDS;
  let w = words();
  // (TextDecoder takes no view of shared memory: a copy)
  const url = decoder.decode(new Uint8Array(memory.buffer, w[base + 1] >>> 0, w[base + 2]).slice());
  const offset = (w[base + 3] >>> 0) + (w[base + 4] >>> 0) * 4294967296;
  const length = w[base + 5], dst = w[base + 6] >>> 0;
  let result = -1, status = 0;
  try {
    const response = await fetch(url, {headers: {Range: 'bytes=' + offset + '-' + (offset + length - 1)}});
    status = response.status;
    if (status === 206 || status === 200) {
      let bytes = new Uint8Array(await response.arrayBuffer());
      if (status === 200 && bytes.length > length) bytes = bytes.subarray(offset, offset + length);
      const n = Math.min(bytes.length, length);
      new Uint8Array(memory.buffer, dst, n).set(bytes.subarray(0, n));
      result = n;
    }
  } catch (e) {
    console.warn('[web] ' + url + ': ' + e);
  }
  w = words();
  w[base + 7] = result;
  w[base + 8] = status;
  Atomics.store(w, base, DONE);
  Atomics.notify(w, base);
}

async function run() {
  const wake = table >>> 2;
  for (;;) {
    const seen = Atomics.load(words(), wake);
    const w = words();
    for (let s = 0; s < SLOTS; ++s) {
      const base = wake + 4 + s * SLOT_WORDS;
      if (Atomics.compareExchange(w, base, REQUESTED, RUNNING) === REQUESTED) serve(s);
    }
    // until a game thread bumps the wake word (without Atomics.waitAsync: a look every 2 ms)
    if (Atomics.waitAsync) {
      const r = Atomics.waitAsync(words(), wake, seen, 1000);
      if (r.async) await r.value;
    } else {
      await new Promise((resolve) => setTimeout(resolve, 2));
    }
  }
}

onmessage = (e) => {
  if (memory) return;
  memory = e.data.memory;
  table = e.data.table;
  Atomics.store(words(), (table >>> 2) + 1, 1); // connected: the game's threads send their reads here from now on
  run();
};
