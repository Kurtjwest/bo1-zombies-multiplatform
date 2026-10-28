#!/bin/bash
# build-windows.sh - build the Windows game with clang and MinGW-w64: build-windows-x64/BO1Zombies.exe
#
#   ./build-windows.sh                  Release build of the 64-bit (x64) game
#   ./build-windows.sh --arch i686      the 32-bit (x86) game, in build-windows-x86
#   ./build-windows.sh Debug            Debug build (in build-windows-x64-debug)
#   ./build-windows.sh --no-lto         without whole-program optimization (ThinLTO): a quicker link after a change
#   ./build-windows.sh --asserts        keep the engine's assertion checks (a failed one stops the game with its
#                                       text) in a Release build; Debug builds always have them
#   ./build-windows.sh --cpu v3         the 64-bit game for a newer processor generation, a little faster; it then does
#                                       not start on older ones: v2 SSE4.2 (Intel 2009+, AMD 2011+), v3 AVX2 (Intel
#                                       2013+ Core, AMD 2015+), v4 AVX-512. Without --cpu: any x64 processor
#   ./build-windows.sh --pgo-generate   profile-guided optimization, step 1: an instrumented game (in
#                                       build-windows-<arch>-pgo-gen); on Windows, tools\pgo-train.bat plays Kino and
#                                       Five with it (2 minutes each) and records what ran (*.profraw files)
#   ./build-windows.sh --pgo-profile P  step 2: the game built with that recording: P is the folder of the .profraw
#                                       files (merged into pgo/ with llvm-profdata) or a .profdata file
#                                       (the toolchain needs its profile runtime for step 1: llvm-mingw and MSYS2 have it)
#
# Runs on Linux and macOS (cross-compiling) and on Windows in an MSYS2 or Git Bash shell. Needs clang and lld with the
# MinGW-w64 headers and libraries: llvm-mingw on any of them (https://github.com/mstorsjo/llvm-mingw, its bin folder
# on PATH), or Debian/Ubuntu's packages (sudo apt install clang lld mingw-w64), or MSYS2's
# (pacman -S mingw-w64-clang-x86_64-toolchain); and CMake 3.16+, Ninja, git and make (FFmpeg and libvpx are fetched
# from their official repositories and built the first time). The Visual Studio project (CMakeLists.txt) still builds
# the 32-bit game with MSVC and the June 2010 DirectX SDK.
set -euo pipefail
cd "$(dirname "$0")"

config="Release"
lto=ON
asserts=OFF
cpu=""
pgo_generate=0
pgo_profile=""
arch="x86_64"
while [ $# -gt 0 ]; do
    case "$1" in
        --arch)
            [ $# -ge 2 ] || { echo "--arch needs x86_64 or i686" >&2; exit 1; }
            arch="$2"
            shift
            ;;
        --arch=*) arch="${1#--arch=}" ;;
        --cpu)
            [ $# -ge 2 ] || { echo "--cpu needs v2, v3 or v4" >&2; exit 1; }
            cpu="$2"
            shift
            ;;
        --cpu=*) cpu="${1#--cpu=}" ;;
        --pgo-generate) pgo_generate=1 ;;
        --pgo-profile)
            [ $# -ge 2 ] || { echo "--pgo-profile needs the folder of the .profraw files or a .profdata file" >&2; exit 1; }
            pgo_profile="$2"
            shift
            ;;
        --pgo-profile=*) pgo_profile="${1#--pgo-profile=}" ;;
        --no-lto) lto=OFF ;;
        --asserts) asserts=ON ;;
        -h | --help)
            sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        -*)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
        *) config="$1" ;;
    esac
    shift
done

case "$arch" in
    x86_64 | amd64 | x64) arch=x86_64; name=x64 ;;
    i686 | i386 | x86 | win32) arch=i686; name=x86 ;;
    *)
        echo "--arch: x86_64 or i686 (not '$arch')" >&2
        exit 1
        ;;
esac
triple="$arch-w64-mingw32"
case "$cpu" in
    "" | v2 | v3 | v4) ;;
    x86-64-v[234]) cpu="${cpu#x86-64-}" ;;
    *)
        echo "--cpu: v2, v3 or v4 (not '$cpu')" >&2
        exit 1
        ;;
esac

missing=()
for tool in cmake ninja git make; do
    command -v "$tool" > /dev/null || missing+=("$tool")
done
if ! command -v "$triple-clang++" > /dev/null; then
    found=""
    # (the names cmake/toolchain-mingw.cmake looks for: a versioned clang 21 or 22 alone is enough)
    for v in -22 -21 -20 "" -19 -18 -17 -16 -15; do
        if command -v "clang++$v" > /dev/null; then
            found=1
            break
        fi
    done
    [ -n "$found" ] || missing+=(clang)
    if ! command -v ld.lld > /dev/null && ! command -v lld > /dev/null && ! ls /usr/bin/ld.lld-* > /dev/null 2>&1; then
        missing+=(lld)
    fi
    if [ ! -d "/usr/$triple" ] && [ ! -d "/usr/lib/gcc/$triple" ] && [ -z "${MSYSTEM:-}" ]; then
        missing+=("MinGW-w64 for $triple")
    fi
fi
if [ ${#missing[@]} -gt 0 ]; then
    echo "Missing: ${missing[*]}." >&2
    echo "Debian/Ubuntu: sudo apt install clang lld mingw-w64 cmake ninja-build git make; or llvm-mingw on PATH" >&2
    echo "(https://github.com/mstorsjo/llvm-mingw)" >&2
    exit 1
fi

suffix=""
[ "$config" = "Release" ] || suffix="-$(echo "$config" | tr '[:upper:]' '[:lower:]')"
dir="build-windows-$name$suffix"
args=(-G Ninja -DCMAKE_BUILD_TYPE="$config" -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-mingw.cmake"
    -DBO1_WINDOWS_TARGET_ARCH="$arch" -DBO1_RELEASE_ASSERTS="$asserts" -DBO1_X86_64_LEVEL="$cpu")
if [ "$pgo_generate" -eq 1 ]; then
    gen="$dir-pgo-gen"
    cmake -S . -B "$gen" "${args[@]}" -DBO1_LTO=OFF -DBO1_PGO_GENERATE=ON -DBO1_PGO_PROFILE=
    cmake --build "$gen"
    echo
    echo "Built $gen/BO1Zombies.exe ($name), instrumented for profile-guided optimization. On Windows:"
    echo "  powershell -NoProfile -ExecutionPolicy Bypass -File setup.ps1 -Build $gen"
    echo "puts the game files next to it (once), then tools\\pgo-train.bat $gen"
    echo "plays two maps with its test bot (keep its window in front) and leaves *.profraw files in that folder."
    echo "Then: ./build-windows.sh --pgo-profile $gen   builds the game with them."
    exit 0
fi
if [ -n "$pgo_profile" ]; then
    if [ -d "$pgo_profile" ]; then
        # the training's recordings: merged with the llvm-profdata of the compiler that builds the game
        cxx="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$dir-pgo-gen/CMakeCache.txt" 2> /dev/null || true)"
        ver=""
        case "$(basename "${cxx:-clang++}")" in clang++-[0-9]*) ver="-${cxx##*clang++-}" ;; esac
        profdata=""
        for p in "$triple-llvm-profdata" "llvm-profdata$ver" llvm-profdata; do
            if command -v "$p" > /dev/null; then
                profdata="$p"
                break
            fi
        done
        [ -n "$profdata" ] || { echo "No llvm-profdata$ver to merge the recordings (Debian/Ubuntu: llvm$ver)" >&2; exit 1; }
        ls "$pgo_profile"/*.profraw > /dev/null 2>&1 || { echo "--pgo-profile: no .profraw files in $pgo_profile" >&2; exit 1; }
        mkdir -p pgo
        merged="pgo/windows-$name-$(date +%Y%m%d-%H%M%S).profdata"
        "$profdata" merge -o "$merged" "$pgo_profile"/*.profraw
        pgo_profile="$merged"
    fi
    [ -f "$pgo_profile" ] || { echo "--pgo-profile: no file or folder $pgo_profile" >&2; exit 1; }
    pgo_profile="$(cd "$(dirname "$pgo_profile")" && pwd)/$(basename "$pgo_profile")"
fi
cmake -S . -B "$dir" "${args[@]}" -DBO1_LTO="$lto" -DBO1_PGO_GENERATE=OFF -DBO1_PGO_PROFILE="$pgo_profile"
cmake --build "$dir"
echo
echo "Built $dir/BO1Zombies.exe ($name)."
echo "It runs in its own folder with the game's files next to it: on Windows,"
echo "  powershell -NoProfile -ExecutionPolicy Bypass -File setup.ps1 -Build $dir"
echo "copies them there from your Call of Duty: Black Ops install (one time), then start it with Play.bat or directly."
