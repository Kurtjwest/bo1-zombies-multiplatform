#!/bin/bash
# tools/benchmark.sh - a repeatable performance run of the macOS or Linux game: its test bot plays a map in a real
# window (god mode, a fixed random seed, no frame limit, no vsync) and the game prints its frame times when it quits.
#
#   tools/benchmark.sh                       Kino, 3 minutes of play, with this computer's build (build-mac/, build-linux/)
#   tools/benchmark.sh --map zombie_cosmodrome --seconds 300
#   tools/benchmark.sh --profile             also record where the game's CPU time goes (macOS: sample, which comes
#                                            with macOS; Linux: perf, if it is installed)
#   tools/benchmark.sh --app path/to/BO1Zombies.app (or path/to/BO1Zombies)   another build
#   tools/benchmark.sh --set "r_mode 1920x1080"   (repeatable) any other setting for the run
#   tools/benchmark.sh --gamedir DIR         the game on another game folder (the game's own --gamedir)
#
# The results go to benchmarks/<date>-<map>/ (not part of the repository): summary.txt (frame rate, frame time
# percentiles, slow frames), game-output.txt (the game's console output) and with --profile the profile. The same map,
# length and settings give comparable numbers on one computer; compare builds by running each the same way. The game
# needs its game files set up first (setup-mac.sh, setup-linux.sh). Close heavy programs, and leave the game's window
# in front: a hidden window draws less.
set -euo pipefail
cd "$(dirname "$0")/.."

map="zombie_theater"
seconds=180
profile=0
app=""
gamedir=()
extra=()
while [ $# -gt 0 ]; do
    case "$1" in
        --map) map="$2"; shift ;;
        --map=*) map="${1#--map=}" ;;
        --seconds) seconds="$2"; shift ;;
        --seconds=*) seconds="${1#--seconds=}" ;;
        --profile) profile=1 ;;
        --app) app="$2"; shift ;;
        --app=*) app="${1#--app=}" ;;
        --gamedir) gamedir=(--gamedir "$2"); shift ;;
        --gamedir=*) gamedir=(--gamedir "${1#--gamedir=}") ;;
        --set)
            read -r -a kv <<< "$2"
            extra+=(+set "${kv[@]}")
            shift
            ;;
        -h | --help)
            sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "Unknown option: $1 (--help lists them)" >&2
            exit 1
            ;;
    esac
    shift
done
case "$seconds" in
    '' | *[!0-9]*) echo "--seconds takes a number of seconds" >&2; exit 1 ;;
esac

os="$(uname -s)"
if [ -z "$app" ]; then
    case "$os" in
        Darwin) app="build-mac/BO1Zombies.app" ;;
        Linux) app="build-linux/BO1Zombies" ;;
        *) echo "This script runs the macOS and Linux games (see the README for the Windows command line)." >&2; exit 1 ;;
    esac
fi
exe="$app"
[ -d "$app" ] && exe="$app/Contents/MacOS/BO1Zombies"
if [ ! -x "$exe" ]; then
    echo "No game at $exe: build it first (./build-mac.sh or ./build-linux.sh), or give its path with --app." >&2
    exit 1
fi

out="benchmarks/$(date +%Y%m%d-%H%M%S)-$map"
mkdir -p "$out"
log="$out/game-output.txt"
# the bot's level starts, then bo1_benchmark measures from 15 s later for --seconds and quits the game;
# bo1_autoquit is only the safety for a run that never gets into the map
safety_ms=$(( (seconds + 300) * 1000 ))
"$exe" ${gamedir[@]+"${gamedir[@]}"} +set bo1_zombies 1 +set logfile 2 \
    +set bo1_testclient_realwindow 1 +set bo1_testclient 1 +set bo1_testclient_fight 1 +set bo1_testclient_god 1 \
    +set bo1_testclient_seed 1 +set bo1_benchmark 1 +set bo1_benchmark_start 15000 +set bo1_benchmark_seconds "$seconds" \
    +set com_maxfps 0 +set r_vsync 0 +set bo1_autoquit "$safety_ms" ${extra[@]+"${extra[@]}"} +devmap "$map" \
    > "$log" 2>&1 &
pid=$!
echo "Running $map for $seconds s of play (process $pid); output in $out/"

if [ "$profile" -eq 1 ]; then
    # from when the measured part starts (the bot is in the map, plus bo1_benchmark_start) for most of it
    while kill -0 "$pid" 2> /dev/null && ! grep -q "^bo1_fight:" "$log"; do
        sleep 1
    done
    sleep 20
    psecs=$(( seconds > 70 ? seconds - 30 : seconds / 2 + 1 ))
    if kill -0 "$pid" 2> /dev/null; then
        case "$os" in
            Darwin)
                echo "Sampling the game for $psecs s (macOS sample)..."
                sample "$pid" "$psecs" 1 -mayDie -file "$out/sample.txt" > /dev/null 2>&1 \
                    || echo "sample failed (it needs the game started from this terminal; see $out/sample.txt)" >&2
                ;;
            Linux)
                if command -v perf > /dev/null; then
                    echo "Recording the game for $psecs s (perf)..."
                    if perf record -F 499 -g -p "$pid" -o "$out/perf.data" -- sleep "$psecs" > /dev/null 2>&1; then
                        perf report -i "$out/perf.data" --no-children -g none --sort comm,dso,sym --stdio \
                            > "$out/perf-report.txt" 2> /dev/null || true
                    else
                        echo "perf failed (kernel.perf_event_paranoid may need lowering)" >&2
                    fi
                else
                    echo "perf is not installed (Debian/Ubuntu: linux-tools); no profile" >&2
                fi
                ;;
        esac
    fi
fi

wait "$pid" || true
if grep -q "^bo1_benchmark:" "$log"; then
    grep "^bo1_benchmark:" "$log" | tail -1 | tee "$out/summary.txt"
else
    echo "No benchmark result: the game ended before the measured part finished (see $log)." >&2
    exit 1
fi
