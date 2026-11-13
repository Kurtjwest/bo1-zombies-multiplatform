// x86 SIMD header stand-in for the ports: the compiler's own on x86 (x86_64, i386), sse2neon's NEON translation
// elsewhere (sse_compat.h).
#pragma once
#if defined(__x86_64__) || defined(__i386__)
#include_next <mmintrin.h>
#elif defined(__EMSCRIPTEN__)
#include <xmmintrin.h> // (Emscripten has no MMX header: wasm_mmx.h through xmmintrin.h)
#else
#include "sse_compat.h"
#endif
