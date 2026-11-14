// The page of the browser build: starts the game when the player clicks Play, and is the game's window while it runs.
// The game runs in Web Workers; this, the page's own thread, keeps the DOM: it passes input to the game through shared
// memory, draws the frames the render thread reads back, plays the sound the game mixes (an AudioWorklet reading the
// game's ring) and reads the controllers. See src/platform/web/web_shared.h for the shared block's layout (the offsets
// below are its).
'use strict';

(function() {
  // ---- the shared block (web_shared.h) ----
  const OFF = {
    ringWrite: 0, ringRead: 4, wake: 8, version: 12,
    canvasW: 16, canvasH: 20, drawW: 24, drawH: 28, dpr: 32, focused: 36, pointerLocked: 40, fullscreen: 44,
    screenW: 48, screenH: 52, mouseX: 56, mouseY: 60,
    framesPresented: 64, framesDisplayed: 68,
    wantCapture: 72, cursorVisible: 76, wantFullscreen: 80, titleSerial: 84, title: 88,
    keyState: 216, modifiers: 248, loading: 252, exitCode: 256,
    framePixels: 260, frameWidth: 272, frameHeight: 284, frameAspectW: 296, frameAspectH: 300, frameFill: 304,
    pageDrawMicros: 308, pageDraws: 312,
    pads: 320, ring: 512, fetch: 33280,
  };
  const SHARED_VERSION = 4; // web_shared.h kSharedVersion: the game and this page are from the same build
  const RING_SIZE = 1024;
  const EV = {KEY_DOWN: 1, KEY_UP: 2, CHAR: 3, MOUSE_MOVE: 4, MOUSE_DOWN: 5, MOUSE_UP: 6, WHEEL: 7, FOCUS: 8,
              RESIZE: 9, POINTER_LOCK: 10, QUIT: 11};
  const MOD = {SHIFT: 1, CONTROL: 2, ALT: 4, COMMAND: 8, CAPSLOCK: 16};

  // KeyboardEvent.code -> macOS virtual key code (src/platform/win32compat/mac_keycodes.h), as the SDL bridge maps
  // SDL's scan codes
  const KEYS = {
    KeyA: 0x00, KeyS: 0x01, KeyD: 0x02, KeyF: 0x03, KeyH: 0x04, KeyG: 0x05, KeyZ: 0x06, KeyX: 0x07, KeyC: 0x08,
    KeyV: 0x09, KeyB: 0x0B, KeyQ: 0x0C, KeyW: 0x0D, KeyE: 0x0E, KeyR: 0x0F, KeyY: 0x10, KeyT: 0x11,
    Digit1: 0x12, Digit2: 0x13, Digit3: 0x14, Digit4: 0x15, Digit6: 0x16, Digit5: 0x17, Equal: 0x18, Digit9: 0x19,
    Digit7: 0x1A, Minus: 0x1B, Digit8: 0x1C, Digit0: 0x1D, BracketRight: 0x1E, KeyO: 0x1F, KeyU: 0x20,
    BracketLeft: 0x21, KeyI: 0x22, KeyP: 0x23, KeyL: 0x25, KeyJ: 0x26, Quote: 0x27, KeyK: 0x28, Semicolon: 0x29,
    Backslash: 0x2A, Comma: 0x2B, Slash: 0x2C, KeyN: 0x2D, KeyM: 0x2E, Period: 0x2F, Backquote: 0x32,
    IntlBackslash: 0x0A, IntlYen: 0x5D, IntlRo: 0x5E,
    NumpadDecimal: 0x41, NumpadMultiply: 0x43, NumpadAdd: 0x45, NumLock: 0x47, NumpadDivide: 0x4B,
    NumpadEnter: 0x4C, NumpadSubtract: 0x4E, NumpadEqual: 0x51, Numpad0: 0x52, Numpad1: 0x53, Numpad2: 0x54,
    Numpad3: 0x55, Numpad4: 0x56, Numpad5: 0x57, Numpad6: 0x58, Numpad7: 0x59, Numpad8: 0x5B, Numpad9: 0x5C,
    Enter: 0x24, Tab: 0x30, Space: 0x31, Backspace: 0x33, Escape: 0x35, MetaLeft: 0x37, ShiftLeft: 0x38,
    CapsLock: 0x39, AltLeft: 0x3A, ControlLeft: 0x3B, MetaRight: 0x36, ShiftRight: 0x3C, AltRight: 0x3D,
    ControlRight: 0x3E, F17: 0x40, AudioVolumeUp: 0x48, AudioVolumeDown: 0x49, AudioVolumeMute: 0x4A,
    F18: 0x4F, F19: 0x50, F5: 0x60, F6: 0x61, F7: 0x62, F3: 0x63, F8: 0x64, F9: 0x65, F11: 0x67, F13: 0x69,
    F16: 0x6A, F14: 0x6B, F10: 0x6D, F12: 0x6F, F15: 0x71, Insert: 0x72, Home: 0x73, PageUp: 0x74, Delete: 0x75,
    F4: 0x76, End: 0x77, F2: 0x78, PageDown: 0x79, F1: 0x7A, ArrowLeft: 0x7B, ArrowRight: 0x7C, ArrowDown: 0x7D,
    ArrowUp: 0x7E, PrintScreen: 0x69, ScrollLock: 0x6B, Pause: 0x71,
  };
  // keys the browser keeps for itself (developer tools, full screen)
  const BROWSER_KEYS = new Set(['F12']);

  const $ = (id) => document.getElementById(id);
  const canvas = $('screen');
  const overlay = $('overlay');
  const status = $('status');
  const playButton = $('play');
  const hint = $('hint');
  const logLines = [];

  let shared = 0;       // the block's address
  let i32 = null;       // views of the game's memory
  let f32 = null;
  let u8 = null;
  let started = false, running = false, quit = false;
  let audioContext = null;
  const audioNodes = new Map();
  let shownFrame = 0;   // the render thread's frame number the canvas shows

  // the game's console, kept for the error message and for bo1Log() (in the browser's console: copy(bo1Log()) copies
  // all of it, which the console itself keeps only the end of)
  const LOG_KEEP = 50000;
  function log(line) {
    logLines.push(line);
    if (logLines.length > LOG_KEEP + 5000) logLines.splice(0, logLines.length - LOG_KEEP);
  }
  window.bo1Log = () => logLines.join('\n');

  function setStatus(text, isError) {
    status.textContent = text;
    status.className = isError ? 'error' : '';
  }

  function settings() {
    try {
      return JSON.parse(localStorage.getItem('bo1web') || '{}');
    } catch (e) {
      return {};
    }
  }
  function saveSettings(s) {
    try {
      localStorage.setItem('bo1web', JSON.stringify(s));
    } catch (e) {
    }
  }

  // ---- what the browser must have ----
  function checkBrowser() {
    const missing = [];
    if (!self.crossOriginIsolated)
      missing.push(location.protocol === 'https:' || location.hostname === 'localhost'
                       ? 'cross-origin isolation (the server must send the COOP/COEP headers: use tools/web-server.py)'
                       : 'a secure connection: open the https:// address the server prints');
    if (typeof SharedArrayBuffer === 'undefined') missing.push('SharedArrayBuffer');
    if (typeof WebAssembly === 'undefined') missing.push('WebAssembly');
    if (typeof OffscreenCanvas === 'undefined') missing.push('OffscreenCanvas');
    else {
      try {
        if (!new OffscreenCanvas(1, 1).getContext('webgl2')) missing.push('WebGL 2');
      } catch (e) {
        missing.push('WebGL 2');
      }
    }
    if (!window.AudioWorkletNode) missing.push('AudioWorklet (the game will be silent)');
    return missing;
  }

  // ---- input records ----
  function push(type, code, x, y, dx, dy, mods, value) {
    if (!i32) return;
    const w = Atomics.load(i32, (shared + OFF.ringWrite) >> 2);
    const r = Atomics.load(i32, (shared + OFF.ringRead) >> 2);
    if (((w - r) & 0x7FFFFFFF) >= RING_SIZE - 1) return; // (full: the game is not reading)
    const base = (shared + OFF.ring + (w & (RING_SIZE - 1)) * 32) >> 2;
    i32[base] = type;
    i32[base + 1] = code | 0;
    i32[base + 2] = x | 0;
    i32[base + 3] = y | 0;
    f32[base + 4] = dx || 0;
    f32[base + 5] = dy || 0;
    i32[base + 6] = mods | 0;
    i32[base + 7] = value | 0;
    Atomics.store(i32, (shared + OFF.ringWrite) >> 2, (w + 1) & 0x7FFFFFFF);
    Atomics.add(i32, (shared + OFF.wake) >> 2, 1);
    Atomics.notify(i32, (shared + OFF.wake) >> 2);
  }
  function setShared(off, v) {
    if (i32) Atomics.store(i32, (shared + off) >> 2, v | 0);
  }
  function getShared(off) {
    return i32 ? Atomics.load(i32, (shared + off) >> 2) : 0;
  }

  function mods(e) {
    let m = 0;
    if (e.shiftKey) m |= MOD.SHIFT;
    if (e.ctrlKey) m |= MOD.CONTROL;
    if (e.altKey) m |= MOD.ALT;
    if (e.metaKey) m |= MOD.COMMAND;
    if (e.getModifierState && e.getModifierState('CapsLock')) m |= MOD.CAPSLOCK;
    return m;
  }

  function setKeyBit(code, down) {
    if (!i32) return;
    const idx = (shared + OFF.keyState + (code >> 5) * 4) >> 2;
    if (down) Atomics.or(i32, idx, 1 << (code & 31));
    else Atomics.and(i32, idx, ~(1 << (code & 31)));
  }

  function onKey(e) {
    if (!running) return;
    if (BROWSER_KEYS.has(e.code)) return;
    const code = KEYS[e.code];
    const down = e.type === 'keydown';
    setShared(OFF.modifiers, mods(e));
    if (code !== undefined) {
      push(down ? EV.KEY_DOWN : EV.KEY_UP, code, 0, 0, 0, 0, mods(e), down && e.repeat ? 1 : 0);
      setKeyBit(code, down);
    }
    if (down) {
      // the characters Windows sends with WM_CHAR: typed text, and the control characters of the console's keys
      let ch = 0;
      if (e.key === 'Backspace') ch = 8;
      else if (e.key === 'Enter') ch = 13;
      else if (e.key === 'Tab') ch = 9;
      else if (e.key === 'Escape') ch = 27;
      else if (e.ctrlKey && !e.altKey && !e.metaKey && e.code.startsWith('Key')) ch = e.code.charCodeAt(3) - 64;
      else if (e.key.length >= 1 && [...e.key].length === 1 && !e.ctrlKey && !e.metaKey) ch = e.key.codePointAt(0);
      if (ch) push(EV.CHAR, code === undefined ? 0 : code, 0, 0, 0, 0, 0, ch);
    }
    // (the game has the keys: no browser shortcuts while it runs, except reload and the browser's own)
    if (!(e.metaKey && (e.key === 'r' || e.key === 'q' || e.key === 'w'))) e.preventDefault();
  }

  let lastX = 0, lastY = 0;
  function canvasPos(e) {
    const r = canvas.getBoundingClientRect();
    return [Math.round(e.clientX - r.left), Math.round(e.clientY - r.top)];
  }

  function onMouseMove(e) {
    if (!running) return;
    const locked = document.pointerLockElement === canvas;
    if (!locked) {
      [lastX, lastY] = canvasPos(e);
      setShared(OFF.mouseX, lastX);
      setShared(OFF.mouseY, lastY);
    }
    push(EV.MOUSE_MOVE, 0, lastX, lastY, e.movementX || 0, e.movementY || 0, mods(e), 0);
  }

  const BUTTONS = [0, 2, 1, 3, 4]; // DOM: left, middle, right, back, forward -> left 0, right 1, middle 2
  function onMouseButton(e) {
    if (!running) return;
    if (e.type === 'mousedown') {
      if (e.target !== canvas) return;
      canvas.focus();
      if (getShared(OFF.wantCapture) && document.pointerLockElement !== canvas) requestCapture();
    }
    if (e.button > 4) return;
    if (document.pointerLockElement !== canvas) {
      [lastX, lastY] = canvasPos(e);
      setShared(OFF.mouseX, lastX);
      setShared(OFF.mouseY, lastY);
    }
    push(e.type === 'mousedown' ? EV.MOUSE_DOWN : EV.MOUSE_UP, BUTTONS[e.button], lastX, lastY, 0, 0, mods(e), 0);
    e.preventDefault();
  }

  let wheelAccum = 0;
  function onWheel(e) {
    if (!running) return;
    e.preventDefault();
    // one notch: the game's step (the next menu item, the next weapon); smooth scrolling made into notches
    const unit = e.deltaMode === 1 ? 3 : e.deltaMode === 2 ? 1 : 100;
    const d = -e.deltaY / unit;
    if ((d > 0) !== (wheelAccum > 0)) wheelAccum = 0;
    wheelAccum += d;
    while (Math.abs(wheelAccum) >= 1) {
      const dir = wheelAccum > 0 ? 1 : -1;
      wheelAccum -= dir;
      push(EV.WHEEL, 0, 0, 0, 0, dir, 0, 0);
    }
  }

  // ---- pointer lock: what the game's mouse capture is here ----
  let capturePending = false;
  function requestCapture() {
    try {
      const p = canvas.requestPointerLock({unadjustedMovement: true});
      if (p && p.catch) p.catch(() => {
        try {
          canvas.requestPointerLock();
        } catch (e) {
        }
      });
    } catch (e) {
      try {
        canvas.requestPointerLock();
      } catch (e2) {
      }
    }
  }

  let releasedByGame = false;
  function onPointerLockChange() {
    const locked = document.pointerLockElement === canvas;
    setShared(OFF.pointerLocked, locked ? 1 : 0);
    push(EV.POINTER_LOCK, 0, 0, 0, 0, 0, 0, locked ? 1 : 0);
    if (locked) {
      hint.hidden = true;
      // (Esc in full screen reaches the game, held to leave: Keyboard Lock)
      if (document.fullscreenElement && navigator.keyboard && navigator.keyboard.lock)
        navigator.keyboard.lock(['Escape']).catch(() => {});
    } else if (running && getShared(OFF.wantCapture) && !releasedByGame) {
      // The player took the pointer back (Esc, which the browser keeps for that): Esc for the game too, which opens
      // its menu (and releases the mouse)
      push(EV.KEY_DOWN, KEYS.Escape, 0, 0, 0, 0, 0, 0);
      push(EV.CHAR, KEYS.Escape, 0, 0, 0, 0, 0, 27);
      push(EV.KEY_UP, KEYS.Escape, 0, 0, 0, 0, 0, 0);
    }
    releasedByGame = false;
  }

  // ---- the page's state ----
  function updateSize() {
    const dpr = Math.min(window.devicePixelRatio || 1, 3);
    const w = Math.max(1, canvas.clientWidth), h = Math.max(1, canvas.clientHeight);
    // (the game's frames are drawn at the canvas's size in device pixels, at most 4K)
    let dw = Math.round(w * dpr), dh = Math.round(h * dpr);
    const scale = Math.min(1, 3840 / dw, 2160 / dh);
    dw = Math.max(1, Math.round(dw * scale));
    dh = Math.max(1, Math.round(dh * scale));
    const changed = getShared(OFF.canvasW) !== w || getShared(OFF.canvasH) !== h;
    setShared(OFF.canvasW, w);
    setShared(OFF.canvasH, h);
    setShared(OFF.drawW, dw);
    setShared(OFF.drawH, dh);
    setShared(OFF.dpr, Math.round(dpr * scale * 1000));
    setShared(OFF.screenW, screen.width);
    setShared(OFF.screenH, screen.height);
    setShared(OFF.fullscreen, document.fullscreenElement ? 1 : 0);
    if (changed && running) push(EV.RESIZE, 0, w, h, 0, 0, 0, 0);
  }

  let lastFocus = -1;
  function updateFocus() {
    const f = document.hasFocus() && document.visibilityState === 'visible' ? 1 : 0;
    if (f === lastFocus) return;
    lastFocus = f;
    setShared(OFF.focused, f);
    if (running) push(EV.FOCUS, 0, 0, 0, 0, 0, 0, f);
  }

  // ---- frames from the render thread ----
  // The render thread reads each frame back into one of three slots in the game's memory (web_shared.h; frame n is in
  // slot n % 3, RGBA, bottom row first) and the page draws the newest on its canvas with WebGL, scaled to the canvas.
  const view = {gl: null, program: null, texture: null, texW: 0, texH: 0, staging: null, shared: true, lost: false,
                uScale: null, drawn: false};

  function setupView() {
    const gl = canvas.getContext('webgl2', {alpha: false, depth: false, stencil: false, antialias: false,
                                            premultipliedAlpha: false, preserveDrawingBuffer: false,
                                            powerPreference: 'high-performance'});
    if (!gl) return false;
    view.gl = gl;
    const shader = (type, src) => {
      const sh = gl.createShader(type);
      gl.shaderSource(sh, src);
      gl.compileShader(sh);
      if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) log('[page] shader: ' + gl.getShaderInfoLog(sh));
      return sh;
    };
    const p = gl.createProgram();
    gl.attachShader(p, shader(gl.VERTEX_SHADER, `#version 300 es
      uniform vec2 scale;
      out vec2 uv;
      void main() {
        vec2 c = vec2(float((gl_VertexID & 1) << 1), float(gl_VertexID & 2)) - 1.0;
        uv = c * 0.5 + 0.5;
        gl_Position = vec4(c * scale, 0.0, 1.0);
      }`));
    gl.attachShader(p, shader(gl.FRAGMENT_SHADER, `#version 300 es
      precision mediump float;
      uniform sampler2D frame;
      in vec2 uv;
      out vec4 color;
      void main() { color = vec4(texture(frame, uv).rgb, 1.0); }`));
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) log('[page] program: ' + gl.getProgramInfoLog(p));
    view.program = p;
    view.uScale = gl.getUniformLocation(p, 'scale');
    view.texture = gl.createTexture();
    view.texW = view.texH = 0;
    gl.bindTexture(gl.TEXTURE_2D, view.texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    return true;
  }

  canvas.addEventListener('webglcontextlost', (e) => {
    e.preventDefault();
    view.lost = true;
  });
  canvas.addEventListener('webglcontextrestored', () => {
    view.lost = false;
    setupView();
    shownFrame = 0; // (draw the next frame whatever it is)
  });

  // the game's memory, a shared WebAssembly.Memory (bo1_web_lib.js; older Emscriptens also have it on Module)
  function gameMemory() {
    const m = Module.bo1Memory ? Module.bo1Memory() : null;
    return m || Module.wasmMemory || null;
  }

  function heapBytes() {
    // (the game's memory grows: a view made earlier doesn't reach what was allocated since)
    const memory = gameMemory();
    const buffer = memory ? memory.buffer : Module.HEAPU8.buffer;
    if (!u8 || u8.byteLength !== buffer.byteLength) {
      i32 = new Int32Array(buffer);
      f32 = new Float32Array(buffer);
      u8 = new Uint8Array(buffer);
    }
    return buffer;
  }

  // the newest frame, if the canvas doesn't show it yet (and the canvas, if its size changed)
  function drawFrame() {
    const frame = Atomics.load(i32, (shared + OFF.framesPresented) >> 2);
    const resized = canvas.width !== getShared(OFF.drawW) || canvas.height !== getShared(OFF.drawH);
    if ((frame === shownFrame || frame <= 0) && !resized) return;
    if (!view.gl && !setupView()) return;
    if (view.lost) return;
    const gl = view.gl;
    if (resized) {
      canvas.width = getShared(OFF.drawW);
      canvas.height = getShared(OFF.drawH);
    }
    if (frame !== shownFrame && frame > 0) {
      const started = performance.now();
      const slot = frame % 3;
      const ptr = getShared(OFF.framePixels + slot * 4) >>> 0;
      const w = getShared(OFF.frameWidth + slot * 4), h = getShared(OFF.frameHeight + slot * 4);
      if (ptr && w > 0 && h > 0) {
        const buffer = heapBytes();
        const pixels = new Uint8Array(buffer, ptr, w * h * 4);
        gl.bindTexture(gl.TEXTURE_2D, view.texture);
        if (w !== view.texW || h !== view.texH) {
          gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
          view.texW = w;
          view.texH = h;
        }
        try {
          if (!view.shared) throw new TypeError('copy');
          gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
        } catch (e) {
          // (a browser whose WebGL doesn't take shared memory: through a copy)
          view.shared = false;
          if (!view.staging || view.staging.length < pixels.length) view.staging = new Uint8Array(pixels.length);
          const copy = view.staging.subarray(0, pixels.length);
          copy.set(pixels);
          gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, copy);
        }
      }
      shownFrame = frame;
      // (for the performance line in the log: webgl_rhi.cpp)
      Atomics.add(i32, (shared + OFF.pageDrawMicros) >> 2, Math.round((performance.now() - started) * 1000));
      Atomics.add(i32, (shared + OFF.pageDraws) >> 2, 1);
    }
    if (!view.texW) return;
    // over the canvas: stretched, or at the game's shape with black bars
    const cw = canvas.width, ch = canvas.height;
    let sx = 1, sy = 1;
    if (!getShared(OFF.frameFill)) {
      const aw = getShared(OFF.frameAspectW) || view.texW, ah = getShared(OFF.frameAspectH) || view.texH;
      const a = (aw / ah) / (cw / ch);
      if (a > 1) sy = 1 / a;
      else sx = a;
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, cw, ch);
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.useProgram(view.program);
    gl.uniform2f(view.uScale, sx, sy);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, view.texture);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    if (!view.drawn) {
      view.drawn = true;
      if (!running) startedRunning();
    }
  }

  // ---- controllers (the Gamepad API is the page's thread's only) ----
  const padState = [{}, {}, {}, {}];
  function pollGamepads() {
    if (!i32 || !navigator.getGamepads) return;
    let pads;
    try {
      pads = navigator.getGamepads();
    } catch (e) {
      return;
    }
    for (let i = 0; i < 4; ++i) {
      const base = (shared + OFF.pads + i * 48) >> 2;
      const p = pads[i];
      if (!p || !p.connected) {
        if (i32[base]) Atomics.store(i32, base, 0);
        continue;
      }
      const b = (k) => p.buttons[k] && (p.buttons[k].pressed || p.buttons[k].value > 0.5);
      const v = (k) => (p.buttons[k] ? p.buttons[k].value : 0);
      const ax = (k) => Math.max(-32768, Math.min(32767, Math.round((p.axes[k] || 0) * 32767)));
      let buttons = 0;
      if (b(12)) buttons |= 0x0001;
      if (b(13)) buttons |= 0x0002;
      if (b(14)) buttons |= 0x0004;
      if (b(15)) buttons |= 0x0008;
      if (b(9)) buttons |= 0x0010;
      if (b(8)) buttons |= 0x0020;
      if (b(10)) buttons |= 0x0040;
      if (b(11)) buttons |= 0x0080;
      if (b(4)) buttons |= 0x0100;
      if (b(5)) buttons |= 0x0200;
      if (b(0)) buttons |= 0x1000;
      if (b(1)) buttons |= 0x2000;
      if (b(2)) buttons |= 0x4000;
      if (b(3)) buttons |= 0x8000;
      const vals = [buttons, Math.round(v(6) * 255), Math.round(v(7) * 255), ax(0), -ax(1), ax(2), -ax(3)];
      let changed = false;
      for (let k = 0; k < 7; ++k)
        if (i32[base + 1 + k] !== vals[k]) {
          i32[base + 1 + k] = vals[k];
          changed = true;
        }
      if (changed) Atomics.add(i32, base + 10, 1);
      Atomics.store(i32, base, 1);
      // rumble
      const left = i32[base + 8], right = i32[base + 9];
      const st = padState[i];
      const now = performance.now();
      if ((left || right || st.rumbling) && p.vibrationActuator &&
          (left !== st.left || right !== st.right || now - (st.at || 0) > 150)) {
        st.left = left;
        st.right = right;
        st.at = now;
        st.rumbling = !!(left || right);
        try {
          p.vibrationActuator.playEffect('dual-rumble',
                                         {duration: 200, strongMagnitude: left / 65535, weakMagnitude: right / 65535})
              .catch(() => {});
        } catch (e) {
        }
      }
    }
  }

  // ---- every animation frame ----
  let titleSerial = -1, lastCursor = -1, lastWantCapture = 0;
  let versionChecked = false;
  function tick() {
    requestAnimationFrame(tick);
    if (!i32) return;
    heapBytes();
    if (!versionChecked && getShared(OFF.version)) {
      versionChecked = true;
      if (getShared(OFF.version) !== SHARED_VERSION) {
        fatal('This page (bo1_page.js) and the game (BO1Zombies.js, BO1Zombies.wasm) are from different builds: ' +
              'copy all of build-web/web to the server, then reload the page.');
        return;
      }
    }
    updateFocus();
    drawFrame();
    // (the render thread waits for the page to have shown its last frame: one per display refresh)
    Atomics.store(i32, (shared + OFF.framesDisplayed) >> 2, shownFrame);
    Atomics.notify(i32, (shared + OFF.framesDisplayed) >> 2);
    const ts = getShared(OFF.titleSerial);
    if (ts !== titleSerial) {
      titleSerial = ts;
      let t = '';
      for (let k = 0; k < 128; ++k) {
        const c = u8[shared + OFF.title + k];
        if (!c) break;
        t += String.fromCharCode(c);
      }
      try {
        t = decodeURIComponent(escape(t));
      } catch (e) {
      }
      document.title = t || 'BO1 Zombies';
    }
    const cursor = getShared(OFF.cursorVisible);
    if (cursor !== lastCursor) {
      lastCursor = cursor;
      canvas.style.cursor = cursor ? 'default' : 'none';
    }
    // the pointer back whenever the game doesn't want it captured (its menus): a pointer left captured would move
    // nothing on the screen
    const want = getShared(OFF.wantCapture);
    lastWantCapture = want;
    if (!want && running && document.pointerLockElement === canvas && !releasedByGame) {
      releasedByGame = true;
      document.exitPointerLock();
    }
    hint.hidden = !(running && want && document.pointerLockElement !== canvas && lastFocus);
    pollGamepads();
    const exit = getShared(OFF.exitCode);
    if (running && exit > 0 && !quit) gameQuit();
  }

  // ---- sound (web_audio.cpp) ----
  async function audioOpen(ptr) {
    if (!audioContext || !window.AudioWorkletNode) return;
    try {
      const buffer = heapBytes(); // (the memory as it is now: the ring may be past what an older view reaches)
      const h = new Int32Array(buffer, ptr, 16);
      const rate = h[7], channels = h[6];
      if (audioContext.sampleRate !== rate) {
        try {
          const ctx = new AudioContext({sampleRate: rate, latencyHint: 'interactive'});
          audioContext.close();
          audioContext = ctx;
        } catch (e) {
          log('[page] audio: this browser cannot play at ' + rate + ' Hz: ' + e);
        }
      }
      await audioContext.audioWorklet.addModule('bo1_audio_worklet.js');
      const node = new AudioWorkletNode(audioContext, 'bo1-audio',
                                        {numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [channels]});
      node.channelInterpretation = 'speakers';
      node.port.postMessage({cmd: 'ring', buffer: buffer, ptr: ptr});
      node.connect(audioContext.destination);
      audioNodes.set(ptr, node);
      if (audioContext.state !== 'running') audioContext.resume().catch(() => {});
    } catch (e) {
      log('[page] audio: ' + e);
    }
  }

  function audioClose(ptr) {
    const node = audioNodes.get(ptr);
    if (!node) return;
    node.port.postMessage({cmd: 'stop'});
    node.disconnect();
    audioNodes.delete(ptr);
  }

  let wakeLock = null;
  function setWakeLock(on) {
    if (on && !wakeLock && navigator.wakeLock)
      navigator.wakeLock.request('screen').then((l) => (wakeLock = l)).catch(() => {});
    else if (!on && wakeLock) {
      wakeLock.release().catch(() => {});
      wakeLock = null;
    }
  }

  // ---- the game files: read by a worker of the page for the game's threads (bo1_fetch_worker.js) ----
  function startFetchWorker() {
    try {
      const worker = new Worker('bo1_fetch_worker.js');
      worker.onerror = (e) => log('[page] fetch worker: ' + (e.message || e));
      const memory = gameMemory();
      if (!memory) throw new Error('no WebAssembly.Memory to give it');
      worker.postMessage({memory: memory, table: shared + OFF.fetch});
    } catch (e) {
      // (the game's threads then make their requests themselves)
      log('[page] no fetch worker: ' + e);
    }
  }

  // ---- the game ----
  function startedRunning() {
    running = true;
    overlay.hidden = true;
    canvas.focus();
    updateSize();
    push(EV.RESIZE, 0, getShared(OFF.canvasW), getShared(OFF.canvasH), 0, 0, 0, 0);
    lastFocus = -1;
    updateFocus();
  }

  function gameQuit() {
    quit = true;
    running = false;
    if (document.pointerLockElement) document.exitPointerLock();
    if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
    overlay.hidden = false;
    playButton.hidden = true;
    setStatus('The game has quit. Reload the page to play again.');
  }

  function fatal(text) {
    running = false;
    if (document.pointerLockElement) document.exitPointerLock();
    overlay.hidden = false;
    playButton.hidden = true;
    setStatus(text, true);
    const pre = $('log');
    pre.textContent = logLines.slice(-60).join('\n');
    pre.hidden = false;
  }

  function parseArgs(line) {
    const out = [];
    const re = /"([^"]*)"|(\S+)/g;
    let m;
    while ((m = re.exec(line))) out.push(m[1] !== undefined ? m[1] : m[2]);
    return out;
  }

  function start() {
    if (started) return;
    started = true;
    const s = settings();
    s.fullscreen = $('fullscreen').checked;
    s.args = $('args').value;
    saveSettings(s);
    playButton.disabled = true;
    // what browsers only allow in a click: sound, full screen, the pointer
    try {
      // (?noaudio in the address: no sound, for finding out whether a problem is the sound's)
      if (!/[?&]noaudio\b/.test(location.search))
        audioContext = new AudioContext({sampleRate: 48000, latencyHint: 'interactive'});
    } catch (e) {
      log('[page] no audio: ' + e);
    }
    if (s.fullscreen && document.documentElement.requestFullscreen)
      document.documentElement.requestFullscreen({navigationUI: 'hide'}).catch(() => {});
    // (the pointer is captured when the game asks for it: the game starts in its menus, which use the pointer as it is)
    setStatus('Loading the game…');
    const args = parseArgs(s.args || '');
    window.Module = {
      arguments: args,
      print: (t) => {
        console.log(t);
        log(t);
      },
      printErr: (t) => {
        console.warn(t);
        log(t);
      },
      setStatus: (t) => {
        if (t && !running) setStatus(t);
      },
      onRuntimeInitialized: () => {
        shared = Module._Bridge_WebSharedState();
        heapBytes();
        startFetchWorker();
        updateSize();
        lastFocus = -1;
        updateFocus();
        setStatus('Reading the game files from the server…');
      },
      onAbort: (what) => fatal('The game stopped: ' + what),
      bo1AudioOpen: audioOpen,
      bo1AudioClose: audioClose,
      bo1WakeLock: setWakeLock,
      bo1ClipboardText: '',
    };
    const script = document.createElement('script');
    script.src = 'BO1Zombies.js';
    script.onerror = () => fatal('BO1Zombies.js could not be loaded: is the browser build in the server\'s folder?');
    document.body.appendChild(script);
  }

  // ---- setup ----
  window.addEventListener('error', (e) => {
    log(String(e.message || e));
    if (started && !running && !quit) fatal('The game could not start: ' + (e.message || e));
  });
  window.addEventListener('keydown', onKey, true);
  window.addEventListener('keyup', onKey, true);
  document.addEventListener('mousemove', onMouseMove);
  document.addEventListener('mousedown', onMouseButton);
  document.addEventListener('mouseup', onMouseButton);
  canvas.addEventListener('wheel', onWheel, {passive: false});
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  document.addEventListener('pointerlockchange', onPointerLockChange);
  document.addEventListener('fullscreenchange', updateSize);
  window.addEventListener('resize', updateSize);
  window.addEventListener('focus', updateFocus);
  window.addEventListener('blur', updateFocus);
  document.addEventListener('visibilitychange', updateFocus);
  document.addEventListener('paste', (e) => {
    const t = e.clipboardData && e.clipboardData.getData('text');
    if (t && window.Module) Module.bo1ClipboardText = t;
  });
  window.addEventListener('beforeunload', () => {
    if (running) push(EV.QUIT, 0, 0, 0, 0, 0, 0, 0);
  });
  hint.addEventListener('click', () => requestCapture());

  const s = settings();
  $('fullscreen').checked = s.fullscreen !== false;
  $('args').value = s.args || '';
  const missing = checkBrowser();
  if (missing.length) {
    setStatus('This browser cannot run the game here. Missing: ' + missing.join('; ') + '.', true);
    if (missing.some((m) => !m.includes('silent'))) playButton.disabled = true;
  } else
    setStatus('Ready. The game\'s files stay on the server; the first start compiles the game, which takes a moment.');
  playButton.addEventListener('click', start);
  requestAnimationFrame(tick);
})();
