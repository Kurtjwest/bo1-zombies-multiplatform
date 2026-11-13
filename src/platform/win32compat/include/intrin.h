// <intrin.h> for the macOS arm64 port. The MSVC intrinsics the engine uses are either clang builtins under
// -fms-extensions (_InterlockedXxx, _BitScanXxx, _rotl, __debugbreak, _byteswap_xxx, ...) or shims in
// win32compat/msvc_crt.h; SSE intrinsics come from sse2neon.
#pragma once
#include "sse_compat.h"
#include "win32compat/msvc_crt.h"
#include "win32compat/interlocked.h"

#if defined(__EMSCRIPTEN__)
// _ReturnAddress (a clang builtin under -fms-extensions): WebAssembly code has no addresses in its memory, and
// Emscripten's way of making one up walks a JavaScript stack trace (USE_OFFSET_CONVERTER), far too slow for the
// engine's per-entity diagnostics that ask for it. They get 0.
#define _ReturnAddress() ((void *)0)
#endif
