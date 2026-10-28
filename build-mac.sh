#!/bin/bash
# build-mac.sh - build the native macOS game: build-mac/BO1Zombies.app
#
#   ./build-mac.sh                     Release build for this Mac's processor (Apple silicon: arm64, Intel: x86_64)
#   ./build-mac.sh Debug               Debug build (in build-mac-debug)
#   ./build-mac.sh --arch x86_64       for Intel Macs, from any Mac (build-mac-x86_64/; --arch arm64: Apple silicon)
#   ./build-mac.sh --universal         one app for both (each processor built in build-mac-<arch>, then joined into
#                                      build-mac-universal/BO1Zombies.app)
#   ./build-mac.sh --no-lto            without whole-program optimization (ThinLTO): a quicker link after a change
#   ./build-mac.sh --asserts           keep the engine's assertion checks (a failed one stops the game with its text)
#                                      in a Release build; Debug builds always have them
#   ./build-mac.sh --cpu v3            Intel Macs (--arch x86_64, the Intel half of --universal): use the newer
#                                      processors' instructions, a little faster (v2 SSE4.2 and v3 AVX2: every Intel Mac
#                                      that runs macOS 13; v4 AVX-512: iMac Pro, Mac Pro 2019, the 2020 Ice Lake
#                                      MacBooks); ignored for Apple silicon
#   ./build-mac.sh --pgo               profile-guided optimization, a few percent faster still: builds the game
#                                      instrumented (build-mac-pgo-gen/), lets its test bot play Kino and Five for 2
#                                      minutes each in a window (tools/pgo-train.sh; needs the game files set up), then
#                                      builds the game with the recorded profile (kept in pgo/). With --universal each
#                                      processor's half is trained (the Intel one under Rosetta)
#   ./build-mac.sh --pgo-profile FILE  the same with a profile recorded before (pgo/*.profdata), without training
#
# Needs: the Xcode command line tools (xcode-select --install), CMake 3.16+ and Ninja (brew install cmake ninja),
# git and network access the first time (FFmpeg and libvpx are fetched from their official repositories and built).
set -euo pipefail
cd "$(dirname "$0")"

config="Release"
lto=ON
asserts=OFF
cpu=""
pgo=0
pgo_profile=""
arch=""
universal=0
while [ $# -gt 0 ]; do
    case "$1" in
        --arch)
            [ $# -ge 2 ] || { echo "--arch needs arm64 or x86_64" >&2; exit 1; }
            arch="$2"
            shift
            ;;
        --arch=*) arch="${1#--arch=}" ;;
        --universal) universal=1 ;;
        --cpu)
            [ $# -ge 2 ] || { echo "--cpu needs v2, v3 or v4" >&2; exit 1; }
            cpu="$2"
            shift
            ;;
        --cpu=*) cpu="${1#--cpu=}" ;;
        --pgo) pgo=1 ;;
        --pgo-profile)
            [ $# -ge 2 ] || { echo "--pgo-profile needs a .profdata file" >&2; exit 1; }
            pgo_profile="$2"
            shift
            ;;
        --pgo-profile=*) pgo_profile="${1#--pgo-profile=}" ;;
        --no-lto) lto=OFF ;;
        --asserts) asserts=ON ;;
        -h | --help)
            sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
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
    "" | arm64 | x86_64) ;;
    *)
        echo "--arch: arm64 or x86_64 (not '$arch')" >&2
        exit 1
        ;;
esac

case "$cpu" in
    "" | v2 | v3 | v4) ;;
    x86-64-v[234]) cpu="${cpu#x86-64-}" ;;
    *)
        echo "--cpu: v2, v3 or v4 (not '$cpu')" >&2
        exit 1
        ;;
esac

if [ "$(uname -s)" != "Darwin" ]; then
    echo "This script builds on macOS. To cross-compile from Linux see cmake/toolchain-macos.cmake." >&2
    exit 1
fi
for tool in cmake ninja git make; do
    if ! command -v "$tool" > /dev/null; then
        echo "Missing '$tool'. Install the Xcode command line tools (xcode-select --install) and Homebrew's cmake and ninja (brew install cmake ninja)." >&2
        exit 1
    fi
done
if ! xcrun --show-sdk-path > /dev/null 2>&1; then
    echo "No macOS SDK found: run xcode-select --install" >&2
    exit 1
fi

suffix=""
[ "$config" = "Release" ] || suffix="-$(echo "$config" | tr '[:upper:]' '[:lower:]')"
dir="build-mac$suffix"

if [ -n "$pgo_profile" ]; then
    [ -f "$pgo_profile" ] || { echo "--pgo-profile: no file $pgo_profile" >&2; exit 1; }
    pgo_profile="$(cd "$(dirname "$pgo_profile")" && pwd)/$(basename "$pgo_profile")"
fi

# one architecture into one build folder (with --pgo: first an instrumented build of it, and its training runs)
build_arch() {
    local a="$1" d="$2" profile="$pgo_profile"
    local common=(-G Ninja -DCMAKE_BUILD_TYPE="$config" -DCMAKE_OSX_ARCHITECTURES="$a"
        -DBO1_RELEASE_ASSERTS="$asserts" -DBO1_X86_64_LEVEL="$cpu")
    if [ "$pgo" -eq 1 ]; then
        if [ "$a" = "arm64" ] && [ "$(uname -m)" != "arm64" ] \
            && [ "$(sysctl -n sysctl.proc_translated 2> /dev/null || echo 0)" != "1" ]; then
            echo "--pgo trains the game on this Mac, which cannot run the Apple silicon build: give --pgo-profile" >&2
            exit 1
        fi
        cmake -S . -B "$d-pgo-gen" "${common[@]}" -DBO1_LTO=OFF -DBO1_PGO_GENERATE=ON -DBO1_PGO_PROFILE=
        cmake --build "$d-pgo-gen"
        profile="$(tools/pgo-train.sh "$d-pgo-gen/BO1Zombies.app" "pgo/mac-$a" | tail -1)"
        profile="$PWD/$profile"
    fi
    cmake -S . -B "$d" "${common[@]}" -DBO1_LTO="$lto" -DBO1_PGO_GENERATE=OFF -DBO1_PGO_PROFILE="$profile"
    cmake --build "$d"
}

if [ "$universal" -eq 1 ]; then
    build_arch arm64 "build-mac-arm64$suffix"
    build_arch x86_64 "build-mac-x86_64$suffix"
    # the app of one build with the executable replaced by both architectures' (libsteam_api.dylib is universal)
    dir="build-mac-universal$suffix"
    app="$dir/BO1Zombies.app"
    rm -rf "$app"
    mkdir -p "$dir"
    cp -R "build-mac-arm64$suffix/BO1Zombies.app" "$app"
    lipo -create "build-mac-arm64$suffix/BO1Zombies.app/Contents/MacOS/BO1Zombies" \
        "build-mac-x86_64$suffix/BO1Zombies.app/Contents/MacOS/BO1Zombies" \
        -output "$app/Contents/MacOS/BO1Zombies"
    codesign --force --sign - "$app" > /dev/null 2>&1 || true
    echo
    echo "Built $app (universal: $(lipo -archs "$app/Contents/MacOS/BO1Zombies"))"
else
    if [ -n "$arch" ]; then
        # another processor's build gets a folder of its own
        dir="build-mac-$arch$suffix"
    else
        if [ "$(uname -m)" = "arm64" ] || [ "$(sysctl -n sysctl.proc_translated 2> /dev/null || echo 0)" = "1" ]; then
            arch=arm64
        else
            arch=x86_64
        fi
        if [ -f "$dir/CMakeCache.txt" ] && ! grep -q "^CMAKE_OSX_ARCHITECTURES:STRING=$arch\$" "$dir/CMakeCache.txt"; then
            echo "$dir was set up for another processor: build with --arch (into build-mac-<arch>), or remove $dir/CMakeCache.txt" >&2
            exit 1
        fi
    fi
    build_arch "$arch" "$dir"
    echo
    echo "Built $dir/BO1Zombies.app for $arch (its game folder: ~/Library/Application Support/BO1Zombies)"
fi

if [ -d build-mac/zone ] || [ -d "$dir/zone" ]; then
    echo "Next: ./setup-mac.sh moves the game files an earlier setup put next to the app to the game folder (one time)."
elif [ ! -d "$HOME/Library/Application Support/BO1Zombies/zone" ]; then
    echo "Next: ./setup-mac.sh \"/path/to/Call of Duty Black Ops\" copies your game files to the game folder (one time)."
fi
