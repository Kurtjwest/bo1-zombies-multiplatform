// zlib-ng's inflate behind src/platform/zlibng/zlibng_inflate.h (only built with zlib-ng, cmake/zlibng.cmake)
#include "zlibng_inflate.h"

#include <zlib-ng.h>

#include <cstdlib>

namespace
{
void ToStream(zng_stream *s, const Bo1ZngIo *io)
{
    s->next_in = io->next_in;
    s->avail_in = io->avail_in;
    s->next_out = io->next_out;
    s->avail_out = io->avail_out;
}

void FromStream(Bo1ZngIo *io, const zng_stream *s, size_t inBefore, size_t outBefore)
{
    io->next_in = const_cast<unsigned char *>(s->next_in);
    io->avail_in = s->avail_in;
    io->next_out = s->next_out;
    io->avail_out = s->avail_out;
    io->in_done = s->total_in - inBefore;
    io->out_done = s->total_out - outBefore;
    io->msg = s->msg;
    io->data_type = s->data_type;
    io->adler = s->adler;
}
} // namespace

void *Bo1Zng_InflateInit2(int windowBits, Bo1ZngIo *io, int *result)
{
    zng_stream *s = static_cast<zng_stream *>(calloc(1, sizeof(zng_stream)));
    if (!s)
    {
        *result = Z_MEM_ERROR;
        return nullptr;
    }
    ToStream(s, io);
    *result = zng_inflateInit2(s, windowBits);
    if (*result != Z_OK)
    {
        free(s);
        return nullptr;
    }
    FromStream(io, s, s->total_in, s->total_out);
    return s;
}

int Bo1Zng_Inflate(void *stream, Bo1ZngIo *io, int flush)
{
    zng_stream *s = static_cast<zng_stream *>(stream);
    ToStream(s, io);
    const size_t inBefore = s->total_in, outBefore = s->total_out;
    const int result = zng_inflate(s, flush);
    FromStream(io, s, inBefore, outBefore);
    return result;
}

int Bo1Zng_InflateEnd(void *stream)
{
    zng_stream *s = static_cast<zng_stream *>(stream);
    const int result = zng_inflateEnd(s);
    free(s);
    return result;
}
