// The processor generation a 64-bit Intel/AMD build was compiled for (BO1_X86_64_LEVEL, cmake/clang_flags.cmake;
// the build scripts' --cpu v2|v3|v4). Checked before main and the other files' static initializers, so a processor
// without those instructions gets a message instead of a crash ("illegal instruction") somewhere in the engine.
// Builds for any x86-64 processor (the default) and the other processors' builds compile nothing here.
#if defined(BO1_X86_64_LEVEL) && defined(__x86_64__)

#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#elif __has_include(<SDL.h>)
#include <SDL.h>
#define BO1_CPU_CHECK_SDL 1
#endif

namespace
{
// these two functions are compiled for any x86-64 processor (the rest of the program is not)
__attribute__((target("arch=x86-64"))) bool CpuHasLevel()
{
    __builtin_cpu_init();
#if BO1_X86_64_LEVEL >= 2
    // x86-64-v2: SSSE3, SSE4.1, SSE4.2, POPCNT (CMPXCHG16B and LAHF come with them on every such processor)
    if (!__builtin_cpu_supports("ssse3") || !__builtin_cpu_supports("sse4.1") || !__builtin_cpu_supports("sse4.2")
        || !__builtin_cpu_supports("popcnt"))
        return false;
#endif
#if BO1_X86_64_LEVEL >= 3
    // x86-64-v3: AVX, AVX2, BMI1, BMI2, FMA (F16C, LZCNT and MOVBE come with them)
    if (!__builtin_cpu_supports("avx") || !__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("bmi")
        || !__builtin_cpu_supports("bmi2") || !__builtin_cpu_supports("fma"))
        return false;
#endif
#if BO1_X86_64_LEVEL >= 4
    // x86-64-v4: AVX-512 F, BW, CD, DQ, VL
    if (!__builtin_cpu_supports("avx512f") || !__builtin_cpu_supports("avx512bw") || !__builtin_cpu_supports("avx512cd")
        || !__builtin_cpu_supports("avx512dq") || !__builtin_cpu_supports("avx512vl"))
        return false;
#endif
    return true;
}

#define BO1_CPU_LEVEL_STR2(n) #n
#define BO1_CPU_LEVEL_STR(n) BO1_CPU_LEVEL_STR2(n)

// (priority 101: before the static initializers of the engine's files, which run at the default priority)
__attribute__((constructor(101), target("arch=x86-64"))) void CheckCpuLevel()
{
    if (CpuHasLevel())
        return;
    static const char kText[] =
        "This copy of BO1 Zombies was built for newer processors (x86-64-v" BO1_CPU_LEVEL_STR(BO1_X86_64_LEVEL)
        ", the build script's --cpu v" BO1_CPU_LEVEL_STR(BO1_X86_64_LEVEL)
        ") and this processor lacks some of their instructions. Build it without --cpu to run it here.";
    fprintf(stderr, "%s\n", kText);
#if defined(_WIN32)
    MessageBoxA(nullptr, kText, "BO1 Zombies", MB_OK | MB_ICONERROR);
#elif defined(__APPLE__)
    CFStringRef title = CFStringCreateWithCString(nullptr, "BO1 Zombies", kCFStringEncodingUTF8);
    CFStringRef text = CFStringCreateWithCString(nullptr, kText, kCFStringEncodingUTF8);
    CFOptionFlags response = 0;
    CFUserNotificationDisplayAlert(0, kCFUserNotificationStopAlertLevel, nullptr, nullptr, nullptr, title, text, nullptr,
        nullptr, nullptr, &response);
#elif defined(BO1_CPU_CHECK_SDL)
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "BO1 Zombies", kText, nullptr);
#endif
    _Exit(1);
}
} // namespace

#endif
