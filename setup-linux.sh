#!/bin/bash
# setup-linux.sh - put your Call of Duty: Black Ops game files in the Linux game's folder (one time):
#
#     ${XDG_DATA_HOME:-~/.local/share}/BO1Zombies
#
# The game reads and writes nothing outside that folder, so the executable runs from anywhere (the repository's own
# files, mods/ and data/main/, are in gamedata/ next to it, and the game installs them into the folder itself).
#
#   ./setup-linux.sh "/path/to/Call of Duty Black Ops"          copies the game files there (about 11 GB)
#   ./setup-linux.sh --move "/path/to/Call of Duty Black Ops"   moves them there instead (instant on the same disk)
#
# The source is a Black Ops PC install (Steam or retail): the folder with BlackOps.exe, zone/ and main/ in it, from
# a Windows disk, a Steam library (Proton installs it under steamapps/common/Call of Duty Black Ops), or a copy. Only
# what the game reads is taken from it: zone/, main/ (the .iwd files, the videos), mods/ (if it has one) and
# localization.txt. The rest of it (BlackOps.exe and its other Windows programs and DLLs, Redist/, Steam's, the
# uninstaller's and other launchers' files, the mod tools' folders, players/: the Windows game's settings) is left
# out, and files the game folder has already are kept. What earlier versions of this script copied to the game folder
# that the game has no use for (those parts of an install) is removed from it.
set -euo pipefail
unset CDPATH

data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
game="$data_home/BO1Zombies"
shown="$game"
case "$game" in
    "$HOME"/*) shown="~${game#"$HOME"}" ;;
esac

usage() {
    echo "Usage: $0 [--move] \"/path/to/Call of Duty Black Ops\"" >&2
    echo "         (the folder with BlackOps.exe, zone/ and main/ in it; --move moves its files instead of copying them)" >&2
}

move=0
src=""
for a in "$@"; do
    case "$a" in
        --move) move=1 ;;
        --copy) move=0 ;;
        -h | --help)
            usage
            exit 0
            ;;
        -*)
            echo "Unknown option: $a" >&2
            usage
            exit 1
            ;;
        *)
            if [ -n "$src" ]; then
                usage
                exit 1
            fi
            src="$a"
            ;;
    esac
done

if pgrep -x BO1Zombies > /dev/null 2>&1; then
    echo "BO1 Zombies is running: quit it first." >&2
    exit 1
fi

# true if the game folder has the game files (zone/ and main/*.iwd, whatever their case)
has_game_files() {
    local m f
    find_child "$game" zone > /dev/null || return 1
    m=$(find_child "$game" main) || return 1
    for f in "$game/$m"/*; do
        case "$(printf '%s\n' "${f##*/}" | tr '[:upper:]' '[:lower:]')" in
            *.iwd) if [ -f "$f" ]; then return 0; fi ;;
        esac
    done
    return 1
}

# true if folder $1 is folder $2 or inside it
is_within() {
    local p="$1"
    while :; do
        if [ "$p" -ef "$2" ]; then return 0; fi
        case "$p" in
            / | . | "") return 1 ;;
        esac
        p=$(dirname "$p")
    done
}

# the name of a child of folder $1 matching $2 whatever its case (Windows names: Zone, MAIN, ...), or nothing
find_child() {
    local e
    for e in "$1"/*; do
        if [ "$(basename "$e" | tr '[:upper:]' '[:lower:]')" = "$2" ]; then
            basename "$e"
            return 0
        fi
    done
    return 1
}

# the game folder's name for a part of a Black Ops install that the game reads (the zones, main/ with the .iwd files
# and the videos, retail's mods/ folder, and localization.txt, which names the language), or nothing: the name the game
# folder has for it already, whatever its case, or the lowercase one
install_entry() {
    local n
    n=$(printf '%s\n' "$1" | tr '[:upper:]' '[:lower:]')
    case "$n" in
        zone | main | mods | localization.txt) ;;
        *) return 1 ;;
    esac
    find_child "$game" "$n" || printf '%s\n' "$n"
}

# what earlier versions of this script copied to the game folder that the game has no use for: the parts of a Windows
# install now left out (programs, DLLs, the DirectX and Visual C++ installers, Steam's, the uninstaller's and other
# launchers' files, the splash pictures). Only names in the game folder itself, none in its folders.
is_leftover() {
    case "$(printf '%s\n' "$1" | tr '[:upper:]' '[:lower:]')" in
        *.exe | *.dll | *.cdx | *.vdf | *.ini | *.nfo | *.url | *.bmp | *.ico | \
            redist | _redist | _commonredist | nodvd | unins*)
            return 0
            ;;
    esac
    return 1
}

# copy_entry SRC DST: copies SRC, a file or folder, to DST in the game folder, keeping the files it has already
copy_entry() {
    if [ -d "$1" ]; then
        if command -v rsync > /dev/null; then
            rsync -a --ignore-existing "$1"/ "$2"/
        else
            (cd "$1" && find . -type f -print0) |
                while IFS= read -r -d '' f; do
                    if [ ! -e "$2/$f" ]; then
                        mkdir -p "$2/$(dirname "$f")"
                        cp -p "$1/$f" "$2/$f"
                    fi
                done
        fi
    elif [ ! -e "$2" ] && [ ! -L "$2" ]; then
        cp -p "$1" "$2"
    fi
    return 0
}

mkdir -p "$game"

if [ -n "$src" ]; then
    if [ ! -d "$src" ]; then
        echo "No folder $src" >&2
        exit 1
    fi
    src=$(cd "$src" && pwd)
    if [ "$src" -ef "$game" ]; then
        : # the game folder itself
    elif ! find_child "$src" zone > /dev/null || ! find_child "$src" main > /dev/null; then
        echo "No Black Ops game files in $src" >&2
        echo "(the folder of a Black Ops PC install has BlackOps.exe, zone/ and main/ in it)" >&2
        exit 1
    elif is_within "$game" "$src"; then
        echo "$shown is inside $src: copy the game files to another folder first." >&2
        exit 1
    else
        # (what the game reads of it: install_entry)
        entries=()
        for e in "$src"/*; do
            if [ ! -e "$e" ] && [ ! -L "$e" ]; then continue; fi
            if install_entry "${e##*/}" > /dev/null; then entries+=("$e"); fi
        done
        taken="  $(for e in "${entries[@]}"; do printf '%s ' "${e##*/}"; done)(the rest of the install is no use to this game)"
        same_disk=0
        if [ "$(stat -L -c %d "$src")" = "$(stat -L -c %d "$game")" ]; then same_disk=1; fi
        if [ "$move" = 1 ] && [ "$same_disk" = 1 ]; then
            echo "Moving the game files from $src to $shown..."
            echo "$taken"
            for e in "${entries[@]}"; do
                d="$game/$(install_entry "${e##*/}")"
                if [ -e "$d" ]; then
                    # (merged: what the game folder has already stays)
                    if [ -d "$e" ]; then
                        rsync -a --ignore-existing --remove-source-files "$e"/ "$d"/ 2> /dev/null || copy_entry "$e" "$d"
                    fi
                else
                    mv "$e" "$d"
                fi
            done
        else
            if [ "$move" = 1 ]; then
                echo "($src is on another disk than $shown: its files are copied, not moved)"
            fi
            if has_game_files; then
                echo "Copying what $shown lacks from $src..."
            else
                echo "Copying the game files from $src to $shown" \
                    "($(du -shc "${entries[@]}" | tail -n 1 | cut -f1); this takes a few minutes)..."
            fi
            echo "$taken"
            for e in "${entries[@]}"; do
                copy_entry "$e" "$game/$(install_entry "${e##*/}")"
            done
        fi
    fi
fi

# what earlier versions of this script copied to the game folder that the game has no use for
removed=()
for e in "$game"/* "$game"/.[!.]* "$game"/..?*; do
    if [ ! -e "$e" ] && [ ! -L "$e" ]; then continue; fi
    if is_leftover "${e##*/}"; then removed+=("$e"); fi
done
if [ ${#removed[@]} -gt 0 ]; then
    echo "Removing from $shown what an earlier setup copied there that the game has no use for:"
    for e in "${removed[@]}"; do
        echo "  ${e##*/}"
        rm -rf "$e"
    done
fi

echo
if ! has_game_files; then
    if [ -z "$src" ]; then usage; fi
    echo "No game files in $shown yet: run" >&2
    echo "  $0 \"/path/to/Call of Duty Black Ops\"" >&2
    exit 1
fi
echo "The game folder is ready: $shown ($(du -sh "$game" | cut -f1))"
built=""
for b in build-linux build-linux-*; do
    if [ -x "$(dirname "$0")/$b/BO1Zombies" ]; then
        built="$(dirname "$0")/$b/BO1Zombies"
        break
    fi
done
if [ -n "$built" ]; then
    echo "Start $built (it needs gamedata/ and libsteam_api.so next to it: move the folder as a whole)."
else
    echo "Next: ./build-linux.sh builds build-linux/BO1Zombies"
fi
