// x86 SIMD header stand-in for the ports: the compiler's own on x86 (x86_64, i386), sse2neon's NEON translation
// elsewhere (sse_compat.h).
#pragma once
#if defined(__x86_64__) || defined(__i386__)
#include_next <xmmintrin.h>
#elif defined(__EMSCRIPTEN__)
// (Emscripten's headers name a struct __unaligned, a keyword with -fms-extensions)
#pragma push_macro("__unaligned")
#define __unaligned bo1_wasm_unaligned
#include_next <xmmintrin.h>
#pragma pop_macro("__unaligned")
#include "wasm_mmx.h"
#else
#include "sse_compat.h"
#endif
