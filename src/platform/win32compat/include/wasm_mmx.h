// The few MMX intrinsics the engine uses (the SSE skinning path, src/gfx_d3d/r_model_skin_sse.cpp) for the browser
// build: Emscripten's SSE headers translate SSE to WebAssembly SIMD but have no MMX (64-bit __m64 values); these do
// it with plain integer arithmetic on the 64 bits, with MMX's results (saturation, round-to-nearest conversion).
#pragma once
#if defined(__EMSCRIPTEN__)
#include <math.h>
#include <stdint.h>
#include <string.h>

// (Emscripten's xmmintrin.h declares __m64 as two floats: these treat it as 64 bits)
static inline uint64_t bo1_m64_bits(__m64 a)
{
    uint64_t r;
    memcpy(&r, &a, 8);
    return r;
}
static inline __m64 bo1_m64_make(uint64_t v)
{
    __m64 r;
    memcpy(&r, &v, 8);
    return r;
}

static inline void _mm_empty(void) {}
static inline void _m_empty(void) {}
static inline __m64 _mm_setzero_si64(void) { return bo1_m64_make(0); }
static inline __m64 _mm_cvtsi32_si64(int a) { return bo1_m64_make((uint32_t)a); }
static inline int _mm_cvtsi64_si32(__m64 a) { return (int)(uint32_t)bo1_m64_bits(a); }

// the low halves of a and b interleaved, 16-bit words: a0 b0 a1 b1
static inline __m64 _m_punpcklwd(__m64 a, __m64 b)
{
    const uint64_t x = bo1_m64_bits(a), y = bo1_m64_bits(b);
    return bo1_m64_make((x & 0xFFFF) | ((y & 0xFFFF) << 16) | (((x >> 16) & 0xFFFF) << 32) | (((y >> 16) & 0xFFFF) << 48));
}
// bytes: a0 b0 a1 b1 a2 b2 a3 b3
static inline __m64 _m_punpcklbw(__m64 a, __m64 b)
{
    const uint64_t x = bo1_m64_bits(a), y = bo1_m64_bits(b);
    uint64_t r = 0;
    for (int i = 0; i < 4; ++i)
        r |= (((x >> (8 * i)) & 0xFF) << (16 * i)) | (((y >> (8 * i)) & 0xFF) << (16 * i + 8));
    return bo1_m64_make(r);
}
// the signed 16-bit words of a, then b, saturated to unsigned bytes
static inline __m64 _m_packuswb(__m64 a, __m64 b)
{
    const uint64_t x = bo1_m64_bits(a), y = bo1_m64_bits(b);
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i)
    {
        const int16_t w = (int16_t)(((i < 4 ? x : y) >> (16 * (i & 3))) & 0xFFFF);
        const uint64_t c = w < 0 ? 0 : w > 255 ? 255 : (uint64_t)w;
        r |= c << (8 * i);
    }
    return bo1_m64_make(r);
}
#define _mm_unpacklo_pi16 _m_punpcklwd
#define _mm_unpacklo_pi8 _m_punpcklbw
#define _mm_packs_pu16 _m_packuswb

static inline void _mm_stream_pi(__m64 *p, __m64 a) { memcpy(p, &a, 8); }

// the two low floats to 32-bit integers, rounded to nearest even (MXCSR's default)
static inline __m64 _mm_cvt_ps2pi(__m128 a)
{
    float f[4];
    memcpy(f, &a, 16);
    const uint32_t lo = (uint32_t)(int32_t)lrintf(f[0]), hi = (uint32_t)(int32_t)lrintf(f[1]);
    return bo1_m64_make(lo | ((uint64_t)hi << 32));
}
#define _mm_cvtps_pi32 _mm_cvt_ps2pi
// four unsigned 16-bit words to floats
static inline __m128 _mm_cvtpu16_ps(__m64 a)
{
    const uint64_t x = bo1_m64_bits(a);
    return _mm_set_ps((float)((x >> 48) & 0xFFFF), (float)((x >> 32) & 0xFFFF), (float)((x >> 16) & 0xFFFF),
                      (float)(x & 0xFFFF));
}
#endif
