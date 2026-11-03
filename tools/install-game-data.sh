#!/bin/bash
# tools/install-game-data.sh [GAME_FOLDER] - installs the repository's own game files in the game folder: mods/ (the
# mods, and any map mod put there) and data/main/ (as main/). The macOS build runs it after every build
# (cmake/macos.cmake), and setup-mac.sh does too: the app itself holds no game files.
#
# GAME_FOLDER: by default the game's own, ~/Library/Application Support/BO1Zombies on macOS
# (${XDG_DATA_HOME:-~/.local/share}/BO1Zombies elsewhere).
#
# A file is copied when the game folder lacks it or has another version of it (a file is the repository's while its
# size and modification time are, or, under 16 MB, its contents; a copy keeps the repository's time). Copies are
# cloned where the file system can (APFS on the same volume, Btrfs, XFS): a map mod's gigabyte takes no more room.
# .installed-files lists what was installed (size, time, path), so that a file the repository no longer has is
# removed from the game folder again, unless it was changed there. Everything else in the game folder is left alone,
# and so are links in it (a mods folder kept elsewhere, say): nothing is written through them.
set -euo pipefail
unset CDPATH

repo="$(cd "$(dirname "$0")/.." && pwd)"
if [ $# -ge 1 ]; then
    game="$1"
elif [ "$(uname)" = Darwin ]; then
    game="$HOME/Library/Application Support/BO1Zombies"
else
    game="${XDG_DATA_HOME:-$HOME/.local/share}/BO1Zombies"
fi
shown="$game"
case "$game" in
    "$HOME"/*) shown="~${game#"$HOME"}" ;;
esac
mkdir -p "$game"

list="$game/.installed-files"
app_list="$game/.bundled-files" # (the list of the app that installed these files itself, before)
big=$((16 << 20))

# "size time" of a file (GNU stat, else the BSD one of macOS)
if stat -c '%s %Y' / > /dev/null 2>&1; then
    stamp() { stat -c '%s %Y' "$1"; }
else
    stamp() { stat -f '%z %m' "$1"; }
fi

# copy_file SRC DST: SRC to DST with its time, cloned where the file system can; written beside DST and renamed over
# it, so an interrupted copy leaves the old file, not half of the new one
copy_file() {
    local tmp="$2.installing"
    rm -f "$tmp"
    if cp -c -p "$1" "$tmp" 2> /dev/null || cp --reflink=auto -p "$1" "$tmp" 2> /dev/null || cp -p "$1" "$tmp"; then
        if mv -f "$tmp" "$2"; then return 0; fi
    fi
    rm -f "$tmp"
    return 1
}

# what folder REL (a path below the game folder) is: "" if it isn't there, else dir, link or file. Walks it from the
# game folder, so a link anywhere along it is found.
path_kind() {
    local p="$game" part rest="$1"
    while [ -n "$rest" ]; do
        part="${rest%%/*}"
        if [ "$part" = "$rest" ]; then rest=""; else rest="${rest#*/}"; fi
        p="$p/$part"
        if [ -L "$p" ]; then
            echo link
            return 0
        elif [ ! -e "$p" ]; then
            echo ""
            return 0
        elif [ ! -d "$p" ]; then
            if [ -n "$rest" ]; then echo blocked; else echo file; fi
            return 0
        fi
    done
    echo dir
}

work=$(mktemp -d "${TMPDIR:-/tmp}/bo1-gamedata.XXXXXX")
trap 'rm -rf "$work"' EXIT
: > "$work/want" # every file the repository has (paths in the game folder)
: > "$work/now"  # the ones that are in the game folder as the repository has them: "size time path"
installed=0
failed=0
removed=0

# install_tree SRC DST: the files of folder SRC (no names starting with a dot: .DS_Store) as DST/... in the game folder
install_tree() {
    local src="$1" dst="$2" f rel s d kind
    [ -d "$src" ] || return 0
    while IFS= read -r -d '' f; do
        rel="$dst/${f#"$src"/}"
        echo "$rel" >> "$work/want"
        kind=$(path_kind "$rel")
        case "$kind" in
            link) continue ;; # (not written through a link)
            dir | blocked)
                echo "install-game-data: could not install $rel: something else is in the way" >&2
                failed=$((failed + 1))
                continue
                ;;
        esac
        s=$(stamp "$f")
        if [ "$kind" = file ]; then
            d=$(stamp "$game/$rel")
            if [ "$d" = "$s" ]; then
                echo "$d $rel" >> "$work/now"
                continue
            fi
            # (the same contents with another time: given the repository's time)
            if [ "${d%% *}" = "${s%% *}" ] && [ "${s%% *}" -lt "$big" ] && cmp -s "$f" "$game/$rel" &&
                touch -r "$f" "$game/$rel"; then
                echo "$s $rel" >> "$work/now"
                continue
            fi
        else
            mkdir -p "$(dirname "$game/$rel")"
        fi
        if copy_file "$f" "$game/$rel"; then
            installed=$((installed + 1))
            echo "$(stamp "$game/$rel") $rel" >> "$work/now"
        else
            echo "install-game-data: could not install $rel" >&2
            failed=$((failed + 1))
        fi
    done < <(find "$src" -mindepth 1 -name '.*' -prune -o -type f -print0)
}
install_tree "$repo/mods" mods
install_tree "$repo/data/main" main

# what an earlier run installed that the repository no longer has: removed, unless it was changed since (after a
# failure nothing is removed: the list of what the repository has may be incomplete)
sort -u -o "$work/want" "$work/want"
if [ -f "$list" ]; then
    while IFS= read -r line; do
        case "$line" in
            "#"* | "") continue ;;
        esac
        rest="${line#* }"
        rel="${rest#* }"
        if grep -Fxq -- "$rel" "$work/want"; then continue; fi
        if [ "$failed" -gt 0 ]; then
            echo "$line" >> "$work/now" # (still listed: tried again next time)
            continue
        fi
        if [ "$(path_kind "$rel")" != file ] || [ "$(stamp "$game/$rel")" != "${line% "$rel"}" ]; then
            continue # (changed, moved or gone: no longer the repository's)
        fi
        if ! rm -f "$game/$rel"; then
            echo "$line" >> "$work/now"
            continue
        fi
        removed=$((removed + 1))
        # and the folders that leaves empty
        d=$(dirname "$rel")
        while [ "$d" != . ] && rmdir "$game/$d" 2> /dev/null; do d=$(dirname "$d"); done
    done < "$list"
fi

{
    echo "# Written by tools/install-game-data.sh: the repository's files it installed in this folder (size, time,"
    echo "# path). One the repository no longer has is removed again, unless it was changed."
    sort -k3 "$work/now"
} > "$list.new"
mv -f "$list.new" "$list"
# (the app installed these files itself before, and kept its own list: this one replaces it)
rm -f "$app_list"

if [ "$failed" -gt 0 ]; then
    echo "The repository's mods/ and data/main/ in $shown: $installed file(s) installed, $failed could not be" \
        "(nothing removed)" >&2
    exit 1
fi
if [ "$installed" -gt 0 ] || [ "$removed" -gt 0 ]; then
    echo "The repository's mods/ and data/main/ in $shown: $installed file(s) installed, $removed removed"
fi
