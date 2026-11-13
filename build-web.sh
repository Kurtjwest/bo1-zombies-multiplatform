#!/bin/bash
# build-web.sh - build the browser game: build-web/web (the page and the game, WebAssembly) and build-web/gamedata
#
#   ./build-web.sh                  Release build with the Emscripten found (emcc on the PATH, or $EMSDK)
#   ./build-web.sh Debug            Debug build (in build-web-debug): engine assertions, source maps
#   ./build-web.sh --install-emsdk  first fetch Emscripten itself (the emsdk, into .emsdk/ in the repository) at the
#                                   version this build is tested with, and build with it
#   ./build-web.sh --asserts        keep the engine's assertion checks in a Release build
#
# Then serve it on your network together with the game files (they stay on the machine that serves them):
#
#   python3 tools/web-server.py [--game "/path/to/Call of Duty Black Ops"]
#
# and open the https:// address it prints, on any device on the network (see the browser section of README.md).
#
# Needs: Emscripten (the emsdk; --install-emsdk gets it), CMake 3.16+, Ninja, git, make and python3, and network
# access the first time (FFmpeg, libvpx and zlib-ng are fetched from their official repositories and built).
set -euo pipefail
cd "$(dirname "$0")"

# the Emscripten version the browser build is tested with (WasmFS's backend interface, which web_fs.cpp implements,
# is Emscripten's internal one; another version may need changes there)
EMSDK_VERSION="3.1.52"

config="Release"
install_emsdk=0
asserts=OFF
while [ $# -gt 0 ]; do
    case "$1" in
        --install-emsdk) install_emsdk=1 ;;
        --asserts) asserts=ON ;;
        -h | --help)
            sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
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

if [ "$install_emsdk" = 1 ]; then
    if [ ! -d .emsdk ]; then
        git clone --depth 1 https://github.com/emscripten-core/emsdk.git .emsdk
    fi
    (cd .emsdk && ./emsdk install "$EMSDK_VERSION" && ./emsdk activate "$EMSDK_VERSION")
fi
# the emsdk's environment: the one of this repository, or the one $EMSDK names
if ! command -v emcc > /dev/null; then
    for env in .emsdk/emsdk_env.sh "${EMSDK:-/nonexistent}/emsdk_env.sh"; do
        if [ -f "$env" ]; then
            # shellcheck disable=SC1090
            source "$env" > /dev/null 2>&1
            break
        fi
    done
fi
if ! command -v emcc > /dev/null || ! command -v emcmake > /dev/null; then
    echo "No Emscripten: ./build-web.sh --install-emsdk fetches it (or install the emsdk and source its emsdk_env.sh)." >&2
    exit 1
fi
version=$(emcc --version | head -1 | sed -n 's/.* \([0-9][0-9.]*\) .*/\1/p')
if [ -n "$version" ] && [ "$version" != "$EMSDK_VERSION" ]; then
    echo "Note: Emscripten $version; the browser build is tested with $EMSDK_VERSION (./build-web.sh --install-emsdk)." >&2
fi
missing=()
for tool in cmake ninja git make python3; do
    command -v "$tool" > /dev/null || missing+=("$tool")
done
if [ ${#missing[@]} -gt 0 ]; then
    echo "Missing: ${missing[*]}. macOS: brew install cmake ninja; Debian/Ubuntu: sudo apt install cmake ninja-build git make python3" >&2
    exit 1
fi

suffix=""
[ "$config" = "Release" ] || suffix="-$(echo "$config" | tr '[:upper:]' '[:lower:]')"
dir="build-web$suffix"
# (kept out of git status: this clone's own exclude list, so the repository's .gitignore stays as it is)
if [ -d .git ] && ! grep -qs '^/build-web\*/$' .git/info/exclude; then
    mkdir -p .git/info
    printf '/build-web*/\n/.emsdk/\n' >> .git/info/exclude
fi
emcmake cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE="$config" -DBO1_RELEASE_ASSERTS="$asserts"
cmake --build "$dir"

echo
echo "Built $dir/web (the game) and $dir/gamedata (the repository's mods/ and data/main/)."
echo "Serve it with the game files:  python3 tools/web-server.py$( [ "$dir" = build-web ] || echo " --site $dir/web") [--game \"/path/to/Call of Duty Black Ops\"]"
