#!/bin/bash
# tools/pgo-train.sh APP PREFIX [MAP...] - the training of a profile-guided build (the build scripts' --pgo; see the
# README). The instrumented game APP (a build with BO1_PGO_GENERATE) plays each map with its test bot in a real window
# (tools/benchmark.sh: 2 minutes of play each; BO1_PGO_SECONDS changes that), writing what it ran when it quits; the
# recordings are then merged (llvm-profdata) into PREFIX-<date>.profdata, whose path is the last line printed.
# Maps: Kino der Toten and Five unless given. Runs the macOS and Linux games; the game files must be set up.
set -euo pipefail
cd "$(dirname "$0")/.."

if [ $# -lt 2 ]; then
    sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 1
fi
app="$1"
prefix="$2"
shift 2
maps=("$@")
[ ${#maps[@]} -gt 0 ] || maps=(zombie_theater zombie_pentagon)
seconds="${BO1_PGO_SECONDS:-120}"

# llvm-profdata of the compiler that built APP (the profile format is the compiler's)
profdata=()
if [ "$(uname -s)" = "Darwin" ]; then
    profdata=(xcrun llvm-profdata)
else
    ver=""
    [ -n "${BO1_PGO_CLANG_VERSION:-}" ] && ver="-$BO1_PGO_CLANG_VERSION"
    for p in "llvm-profdata$ver" llvm-profdata; do
        if command -v "$p" > /dev/null; then
            profdata=("$p")
            break
        fi
    done
    if [ ${#profdata[@]} -eq 0 ]; then
        echo "No llvm-profdata$ver (Debian/Ubuntu: llvm${ver}; Fedora, Arch: llvm)" >&2
        exit 1
    fi
fi

raw="$prefix-raw"
rm -rf "$raw"
mkdir -p "$raw"
rawabs="$(cd "$raw" && pwd)"
for map in "${maps[@]}"; do
    echo "PGO training: $map, $seconds s of play (the instrumented game runs slower than the real one)" >&2
    LLVM_PROFILE_FILE="$rawabs/$map-%p-%m.profraw" tools/benchmark.sh --app "$app" --map "$map" --seconds "$seconds" >&2 \
        || echo "PGO training: the $map run did not finish; using what it recorded" >&2
done
if ! ls "$raw"/*.profraw > /dev/null 2>&1; then
    echo "PGO training recorded nothing (did the game start? see benchmarks/)" >&2
    exit 1
fi
out="$prefix-$(date +%Y%m%d-%H%M%S).profdata"
"${profdata[@]}" merge -o "$out" "$raw"/*.profraw
rm -rf "$raw"
echo "$out"
