#pragma once
// mod (perf): the inflate of the fastfile loader (src/database/db_auth.cpp) and of the .iwd reader
// (src/qcommon/unzip.cpp), the decompression a map load spends most of its database thread's time on. With
// BO1_ZLIB_NG (the clang builds, cmake/zlibng.cmake) zlib-ng decompresses, about 1.5x as fast as the bundled zlib 1.2.3
// (Kino's fastfile, 111 MB out: 0.30 s against 0.44 s), into the same bytes; the stream stays the bundled zlib's
// z_stream for its callers, its state is zlib-ng's. Without it (the MSVC build) these are the bundled zlib's calls.
// Compression, and the other readers, keep the bundled zlib.
#include <zlib/zlib.h>

#if defined(BO1_ZLIB_NG)
#include <platform/zlibng/zlibng_inflate.h>

namespace bo1_inflate_detail
{
inline Bo1ZngIo In(const z_stream *s)
{
    Bo1ZngIo io = {};
    io.next_in = s->next_in;
    io.avail_in = s->avail_in;
    io.next_out = s->next_out;
    io.avail_out = s->avail_out;
    return io;
}
// the stream's totals advance by what the call consumed and produced (a caller may change them between calls, as
// unzip.cpp does for stored entries)
inline void Out(z_stream *s, const Bo1ZngIo &io)
{
    s->next_in = io.next_in;
    s->avail_in = io.avail_in;
    s->next_out = io.next_out;
    s->avail_out = io.avail_out;
    s->total_in += (uLong)io.in_done;
    s->total_out += (uLong)io.out_done;
    s->msg = const_cast<char *>(io.msg);
    s->data_type = io.data_type;
    s->adler = io.adler;
}
} // namespace bo1_inflate_detail

inline int Bo1_InflateInit2(z_stream *s, int windowBits)
{
    Bo1ZngIo io = bo1_inflate_detail::In(s);
    int result = Z_OK;
    void *stream = Bo1Zng_InflateInit2(windowBits, &io, &result);
    s->state = reinterpret_cast<internal_state *>(stream);
    if (stream)
        bo1_inflate_detail::Out(s, io);
    return result;
}

inline int Bo1_Inflate(z_stream *s, int flush)
{
    if (!s->state)
        return Z_STREAM_ERROR;
    Bo1ZngIo io = bo1_inflate_detail::In(s);
    const int result = Bo1Zng_Inflate(s->state, &io, flush);
    bo1_inflate_detail::Out(s, io);
    return result;
}

inline int Bo1_InflateEnd(z_stream *s)
{
    if (!s->state)
        return Z_STREAM_ERROR;
    const int result = Bo1Zng_InflateEnd(s->state);
    s->state = nullptr;
    return result;
}
#else
inline int Bo1_InflateInit2(z_stream *s, int windowBits)
{
    return inflateInit2_(s, windowBits, ZLIB_VERSION, sizeof(z_stream));
}
inline int Bo1_Inflate(z_stream *s, int flush)
{
    return inflate(s, flush);
}
inline int Bo1_InflateEnd(z_stream *s)
{
    return inflateEnd(s);
}
#endif
