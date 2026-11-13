// JavaScript the browser build links in (--js-library, cmake/web.cmake): what the game's threads do outside the
// WebAssembly module. Every function here runs on the thread that calls it (the game's threads are Web Workers);
// nothing touches the DOM, which belongs to the page's own thread (page/bo1_page.js).

addToLibrary({
  // ---- the game folder on the server (web_fs.cpp): synchronous requests, which workers may make ----

  // Reads [offset, offset + length) of a server file into dst. Returns the bytes read, or -1.
  bo1_web_http_read: function(url, offset, length, dst) {
    var xhr = new XMLHttpRequest();
    xhr.open('GET', UTF8ToString(url), false);
    xhr.responseType = 'arraybuffer';
    xhr.setRequestHeader('Range', 'bytes=' + offset + '-' + (offset + length - 1));
    try {
      xhr.send(null);
    } catch (e) {
      err('[web] ' + UTF8ToString(url) + ': ' + e);
      return -1;
    }
    if (xhr.status != 206 && xhr.status != 200) return -1;
    var bytes = new Uint8Array(xhr.response);
    // (a server that ignores Range sends the whole file)
    if (xhr.status == 200 && bytes.length > length) bytes = bytes.subarray(offset, offset + length);
    var n = Math.min(bytes.length, length);
    HEAPU8.set(bytes.subarray(0, n), dst);
    return n;
  },

  // A text file of the server's, malloc'd (the caller frees it), or 0; *status gets the HTTP status.
  bo1_web_http_get_text__deps: ['malloc'],
  bo1_web_http_get_text: function(url, status) {
    var xhr = new XMLHttpRequest();
    xhr.open('GET', UTF8ToString(url), false);
    try {
      xhr.send(null);
    } catch (e) {
      HEAP32[status >> 2] = 0;
      return 0;
    }
    HEAP32[status >> 2] = xhr.status;
    if (xhr.status != 200) return 0;
    var text = xhr.responseText;
    var size = lengthBytesUTF8(text) + 1;
    var p = _malloc(size);
    stringToUTF8(text, p, size);
    return p;
  },

  // Sends a file the game wrote back to the server. Returns the HTTP status (0: no answer).
  bo1_web_http_put: function(url, data, length) {
    var xhr = new XMLHttpRequest();
    xhr.open('PUT', UTF8ToString(url), false);
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    try {
      // (a copy: requests do not take views of shared memory)
      data >>>= 0; // (an address above 2 GB arrives negative)
      xhr.send(HEAPU8.slice(data, data + length));
    } catch (e) {
      return 0;
    }
    return xhr.status;
  },

  bo1_web_http_delete: function(url) {
    var xhr = new XMLHttpRequest();
    xhr.open('DELETE', UTF8ToString(url), false);
    try {
      xhr.send(null);
    } catch (e) {
      return 0;
    }
    return xhr.status;
  },

  // ---- the renderer's canvas (webgl_rhi.cpp) ----
  // The render thread draws into a canvas of its own (an OffscreenCanvas made here, on its thread) and hands each
  // finished frame to the page as an ImageBitmap, which the page shows in the visible canvas: the thread never has to
  // return to its event loop for a frame to reach the screen, as a canvas transferred to it would need.

  // Makes the canvas and its WebGL 2 context, current on the calling thread. Returns the context handle, or 0.
  bo1_web_gl_create__deps: ['$GL'],
  // The render thread's WebGL 2 context. Everything is drawn into textures: the frames go to the page as pixels
  // (webgl_rhi.cpp Present), so the canvas itself is never shown.
  bo1_web_gl_create: function() {
    if (typeof OffscreenCanvas == 'undefined') {
      err('[web] this browser has no OffscreenCanvas in workers');
      return 0;
    }
    var canvas = new OffscreenCanvas(16, 16);
    var attrs = {
      majorVersion: 2, minorVersion: 0, alpha: false, depth: false, stencil: false, antialias: false,
      premultipliedAlpha: false, preserveDrawingBuffer: false, powerPreference: 'high-performance',
      failIfMajorPerformanceCaveat: false
    };
    var handle = GL.createContext(canvas, attrs);
    if (!handle) {
      err('[web] this browser could not make a WebGL 2 context');
      return 0;
    }
    GL.makeContextCurrent(handle);
    var gl = GL.currentContext.GLctx;
    // the extensions the renderer uses (enabled for the context's lifetime)
    var want = ['EXT_color_buffer_float', 'EXT_color_buffer_half_float', 'EXT_float_blend', 'OES_texture_float_linear',
                'WEBGL_compressed_texture_s3tc', 'WEBGL_compressed_texture_s3tc_srgb', 'EXT_texture_compression_rgtc',
                'EXT_texture_filter_anisotropic', 'EXT_texture_norm16', 'OES_draw_buffers_indexed',
                'EXT_clip_control', 'EXT_polygon_offset_clamp', 'EXT_depth_clamp', 'WEBGL_provoking_vertex'];
    var have = [];
    for (var i = 0; i < want.length; ++i)
      if (gl.getExtension(want[i])) have.push(want[i]);
    Module['bo1GlExtensions'] = have;
    gl.canvas.addEventListener && gl.canvas.addEventListener('webglcontextlost', function(e) {
      e.preventDefault();
      err('[web] the WebGL context was lost');
    });
    return handle;
  },

  // Whether the context has an extension (one of those bo1_web_gl_create enables).
  bo1_web_gl_has: function(name) {
    var list = Module['bo1GlExtensions'] || [];
    return list.indexOf(UTF8ToString(name)) >= 0 ? 1 : 0;
  },

  // OES_draw_buffers_indexed's per-attachment blend state, which Emscripten's GL has no C functions for.
  bo1_web_gl_blend_indexed: function(buf, enable, srcRGB, dstRGB, srcA, dstA, opRGB, opA, mask) {
    var gl = GL.currentContext.GLctx;
    var ext = gl.getExtension('OES_draw_buffers_indexed');
    if (!ext) return;
    if (enable) ext.enableiOES(0x0BE2 /* BLEND */, buf);
    else ext.disableiOES(0x0BE2, buf);
    ext.blendFuncSeparateiOES(buf, srcRGB, dstRGB, srcA, dstA);
    ext.blendEquationSeparateiOES(buf, opRGB, opA);
    ext.colorMaskiOES(buf, !!(mask & 8), !!(mask & 4), !!(mask & 2), !!(mask & 1));
  },

  // EXT_clip_control: Direct3D's depth range and +y at row 0, as the OpenGL renderer has it (else the vertex shaders
  // do it themselves).
  bo1_web_gl_clip_control: function() {
    var gl = GL.currentContext.GLctx;
    var ext = gl.getExtension('EXT_clip_control');
    if (!ext) return 0;
    ext.clipControlEXT(ext.UPPER_LEFT_EXT, ext.ZERO_TO_ONE_EXT);
    return 1;
  },
});
