#pragma once
// zlib-ng's inflate for the engine's decompression of fastfiles and .iwd files (src/qcommon/bo1_inflate.h; the build
// adds it with BO1_ZLIBNG, cmake/zlibng.cmake). zlib-ng's header and the bundled zlib's cannot be in one file, so the
// stream's fields cross here one by one.
#include <cstddef>

struct Bo1ZngIo
{
    unsigned char *next_in;
    unsigned int avail_in;
    unsigned char *next_out;
    unsigned int avail_out;
    size_t in_done; // what the call consumed and produced (the caller's z_stream keeps its own totals)
    size_t out_done;
    const char *msg;
    int data_type;
    unsigned int adler;
};

// a new inflate stream (windowBits as zlib's inflateInit2: 15 a zlib stream, -15 raw deflate); null and *result on failure
void *Bo1Zng_InflateInit2(int windowBits, Bo1ZngIo *io, int *result);
int Bo1Zng_Inflate(void *stream, Bo1ZngIo *io, int flush);
int Bo1Zng_InflateEnd(void *stream);
