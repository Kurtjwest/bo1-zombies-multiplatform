#!/bin/bash
# Co-op on one computer (macOS or Linux), to try it out: runs two copies of the game side by side, each in a window
# of its own, one to host and one to join it (README: Co-op on a local network).
#
#   ./tools/coop-test.sh          both copies, the host's window on the left, the other one on the right
#   ./tools/coop-test.sh host     only the host (to start one of them again)
#   ./tools/coop-test.sh guest    only the copy that joins
#
# Then, in the host's window, open the console (the key below Esc) and start a map:  /map zombie_theater
# (the host waits for the second player on a black screen), and in the other window, at the main menu (a key past
# the title screen first):  /connect 127.0.0.1:28960
# (with the slash: the console sends what doesn't start with / or \ as a chat message)
# Cmd-Tab (macOS) or Alt-Tab switches between the two windows; only the one in front gets the keyboard and mouse,
# so the other player stands still meanwhile. Ctrl-C here (or quitting both) ends the test.
#
# Each copy runs on a game folder of its own, made the first time in ~/BO1Zombies-coop (host/ and guest/): links to
# the game files in your game folder, and copies of your settings (players/) and shader cache. The two copies don't
# write over each other's settings and logs, and your own game folder and settings stay as they are. To make the two
# folders again (after setup-mac.sh / setup-linux.sh changed the game files, say): rm -rf ~/BO1Zombies-coop
# Logs: ~/BO1Zombies-coop/host/main/console_mp.log (and terminal.log, with the platform's own lines), guest likewise.
#
# BO1_APP       the game to run (default: this repository's build for this computer)
# BO1_GAME_DIR  the game folder with the game files (default: the game's own, as setup-mac.sh / setup-linux.sh made it)
# BO1_COOP_DIR  where the two folders go (default: ~/BO1Zombies-coop)

repo="$(cd "$(dirname "$0")/.." && pwd)"
which="${1:-both}"
case "$which" in
    both|host|guest) ;;
    *) echo "usage: $0 [host|guest]" >&2; exit 2 ;;
esac

if [ "$(uname)" = Darwin ]; then
    game="${BO1_GAME_DIR:-$HOME/Library/Application Support/BO1Zombies}"
    candidates="build-mac build-mac-universal build-mac-arm64 build-mac-x86_64"
    exe=BO1Zombies.app/Contents/MacOS/BO1Zombies
    setup=./setup-mac.sh
else
    game="${BO1_GAME_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/BO1Zombies}"
    candidates="build-linux build-linux-x86_64 build-linux-aarch64 build-linux-armhf build-linux-i686"
    exe=BO1Zombies
    setup=./setup-linux.sh
fi
app="$BO1_APP"
if [ -z "$app" ]; then
    for b in $candidates; do
        if [ -x "$repo/$b/$exe" ]; then
            app="$repo/$b/$exe"
            break
        fi
    done
fi
if [ -z "$app" ] || [ ! -x "$app" ]; then
    echo "No build of the game found (build it first, or name it with BO1_APP=...)" >&2
    exit 1
fi
if [ ! -d "$game/zone" ] || [ ! -d "$game/main" ]; then
    echo "No game files in $game (run $setup first, or name the folder with BO1_GAME_DIR=...)" >&2
    exit 1
fi
coop="${BO1_COOP_DIR:-$HOME/BO1Zombies-coop}"

lower() { echo "$1" | tr 'A-Z' 'a-z'; }

# a game folder for one copy: links to everything in the game folder but what the game writes there, its own
# main/ (its logs) with links to main's files, and copies of the settings and the shader cache
make_folder() {
    local dir="$1" e n
    [ -d "$dir/main" ] && return 0
    echo "Making $dir"
    mkdir -p "$dir/main" || exit 1
    for e in "$game"/*; do
        n="${e##*/}"
        case "$(lower "$n")" in
            main|players|shadercache|shaderdumps|screenshots) ;;
            *) ln -s "$e" "$dir/$n" ;;
        esac
    done
    for e in "$game"/main/*; do
        n="${e##*/}"
        case "$(lower "$n")" in
            *.log|*.csv|*.dat|screenshots) ;; # written by the game
            *) ln -s "$e" "$dir/main/$n" ;;
        esac
    done
    if [ -d "$game/players" ]; then cp -R "$game/players" "$dir/players"; else mkdir "$dir/players"; fi
    if [ -d "$game/shadercache" ]; then cp -R "$game/shadercache" "$dir/shadercache"; fi
}

pids=()
# run <host|guest> <port> <window x> <name> [engine arguments...]
run() {
    local side="$1" port="$2" x="$3" name="$4"
    shift 4
    make_folder "$coop/$side"
    echo "Starting the $side (port $port), log: $coop/$side/terminal.log"
    "$app" --gamedir "$coop/$side" +set bo1_zombies 1 +set logfile 2 +set net_ip 127.0.0.1 +set net_port "$port" \
        +set r_fullscreen 0 +set r_mode 720x480 +set vid_xpos "$x" +set vid_ypos 40 +set name "$name" "$@" \
        > "$coop/$side/terminal.log" 2>&1 &
    pids+=($!)
}

trap 'kill "${pids[@]}" 2>/dev/null' INT TERM
if [ "$which" != guest ]; then
    run host 28960 0 Host +set bo1_coop_players 2
fi
if [ "$which" = both ]; then
    sleep 3
fi
if [ "$which" != host ]; then
    run guest 28961 730 Guest
fi
echo
if [ "$which" != guest ]; then
    echo "Host window: open the console and type   /map zombie_theater   (it waits for the other player)"
fi
if [ "$which" != host ]; then
    echo "Guest window: at the main menu, open the console and type   /connect 127.0.0.1:28960"
fi
echo "Ctrl-C here ends the test."
wait
