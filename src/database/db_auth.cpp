#include "db_auth.h"
#include <universal/assertive.h>
#include <qcommon/common.h>
#include <qcommon/bo1_inflate.h>

int __cdecl DB_AuthLoad_InflateInit(z_stream_s *stream, bool isSecure)
{
    if ( isSecure
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\database\\db_auth.cpp", 944, 0, "%s", "!isSecure") )
    {
        __debugbreak();
    }
    return Bo1_InflateInit2(stream, MAX_WBITS); // mod (perf): retail inflateInit_(stream, "1.2.3", sizeof(z_stream_s)); zlib-ng with BO1_ZLIB_NG
}

void __cdecl DB_AuthLoad_InflateEnd(z_stream_s *stream)
{
    Bo1_InflateEnd(stream); // mod (perf): retail inflateEnd
}

int __cdecl DB_AuthLoad_Inflate(z_stream_s *stream, int flush)
{
    return Bo1_Inflate(stream, flush); // mod (perf): retail inflate
}

void *__cdecl tom_game_malloc(unsigned int size)
{
    return Com_LiveAllocate(size);
}

unsigned __int8 *__cdecl tom_game_realloc(unsigned __int8 *ptr, unsigned int size)
{
    return Com_LiveRealloc(ptr, size);
}

unsigned __int8 *__cdecl tom_game_calloc(unsigned int num, unsigned int size)
{
    return Com_LiveCalloc(num, size);
}

void __cdecl tom_game_free(void *ptr)
{
    Com_LiveDeallocate(ptr);
}

