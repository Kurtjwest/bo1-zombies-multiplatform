#!/usr/bin/env python3
# merge_wds_waw.py [<mods folder>] [--wds <folder>] [--waw <folder>] [--out <folder>]
#
# Makes mods/wds_waw, the folder that lets Waffen der Schoenheit (mods/wds, by Sidzzz) and the WaW weapon mod
# (mods/WAW_Weapons_for_BO1) play in one game (README.md, "Mod-tools mods"). Both replace the scripts of Kino der Toten,
# Five, Ascension and Moon (maps/<map>.gsc, clientscripts/<map>.csc). wds_waw's are wds's, with the WaW guns wds hasn't
# added to include_weapons(): their include_weapon lines and, for the guns wds doesn't register itself (init_weapons in
# its maps/_zombiemode_weapons.gsc), the WaW mod's add_zombie_weapon lines (cost, hint). Its mod.txt says "combines":
# the game loads both mods' mod.ff and stacks wds_waw after them while both are on.
#
# The scripts are read from each mod's folder or its .iwd files. Run it again when either mod is updated. It also gives
# each of the two mods a mod.txt (its name and description in the Mods menu) if it has none. Default <mods folder>:
# ./mods; the mods' folders are found in it by name (wds, WAW_Weapons_for_BO1) unless given.
import argparse
import glob
import os
import re
import sys
import zipfile

MAPS = [("zombie_theater", "Kino der Toten"), ("zombie_pentagon", "Five"), ("zombie_cosmodrome", "Ascension"),
        ("zombie_moon", "Moon")]

WDS_TXT = """// The Mods menu's entry for Waffen der Schoenheit (wds, by Sidzzz, UGX-Mods). Written for BO1 Zombies, not part of
// the mod; the format is README.md's "Mods menu". The mod plays on the original maps it has a script for (found by
// the game: Kino der Toten, Five, Ascension and Moon). With the WaW weapons mod on too, mods/wds_waw combines the two.
name        "Waffen der Schoenheit"
description "Sidzzz's weapon mod for Kino der Toten, Five, Ascension and Moon: guns from later Call of Duty games, wonder weapons (Ray Gun Mark II, Blundergat, Sliquifier, the Origins staffs), Galvaknuckles, Double Tap 2.0 and the Origins crew."
order       61
"""

WAW_TXT = """// The Mods menu's entry for the WaW weapon mod (v2.13, UGX-Mods: "WaW Weapon Mod for BO1 Maps"). Written for BO1
// Zombies, not part of the mod; the format is README.md's "Mods menu". The mod plays on the ten original maps it has
// a script for (found by the game; no map lines needed). With Waffen der Schoenheit on too, mods/wds_waw combines the
// two on Kino der Toten, Five, Ascension and Moon.
name        "WaW weapons"
description "World at War's guns in the Mystery Box on all ten maps: PPSh-41, MG42, BAR, M1 Garand, PTRS-41, flamethrower, Panzerschreck and more (Thompson, STG-44 and Trench Gun on the WaW maps). Thundergun on Five and Nacht der Untoten, Wunderwaffe on Verruckt."
order       60
"""

COMBINED_TXT = """// Lets Waffen der Schoenheit (mods/{wds}) and the WaW weapon mod (mods/{waw}) play in one game. Not part of
// either mod, and not a row of the Mods menu: it is on while both are (README.md, "Mods menu": combines).
//
// Both mods replace the scripts of Kino der Toten, Five, Ascension and Moon. Here, maps/ and clientscripts/ have wds's
// scripts of those maps with the WaW guns wds hasn't added to their include_weapons() (made by the repository's
// tools/merge_wds_waw.py; run it again if either mod is updated). Both mods' mod.ff load, the WaW mod's first, so
// wds's version of a gun both have (AK-74u, M14, MP40, Thundergun, ...) wins, and this folder is stacked after both.
// On the other six maps only the WaW mod plays.
name        "Waffen der Schoenheit + WaW weapons"
description "Both weapon mods in one game: wds's guns and wonder weapons, with World at War's added to the Mystery Box, on Kino der Toten, Five, Ascension and Moon."
order       62
combines    {wds_id} {waw_id}
"""


class ModFiles:
    """A mod's files: loose in its folder, else in its .iwd files (the last one in name order that has it)."""

    def __init__(self, folder):
        self.folder = folder
        self.iwds = []
        for path in sorted(glob.glob(os.path.join(folder, "*.iwd")), key=str.lower):
            try:
                z = zipfile.ZipFile(path)
            except zipfile.BadZipFile:
                print("warning: %s isn't a zip file, skipped" % path, file=sys.stderr)
                continue
            names = {n.replace("\\", "/").lower(): n for n in z.namelist()}
            self.iwds.append((z, names))

    def read(self, rel):
        loose = os.path.join(self.folder, rel)
        if os.path.isfile(loose):
            with open(loose, "rb") as f:
                return f.read().decode("latin-1")
        for z, names in reversed(self.iwds):
            if rel.lower() in names:
                return z.read(names[rel.lower()]).decode("latin-1")
        raise SystemExit("%s: no %s (in the folder or its .iwd files)" % (self.folder, rel))


def mod_id(folder):
    # the mod's name: the folder's, other characters than letters, digits and '_' as '_' (FS_ModIdForFolder)
    return re.sub(r"[^A-Za-z0-9_]", "_", os.path.basename(os.path.normpath(folder)))


def func_span(text, name):
    m = re.search(r"^" + re.escape(name) + r"\s*\(\s*\)\s*\r?\n\s*\{", text, re.M)
    if not m:
        raise SystemExit("no %s()" % name)
    i = m.end() - 1  # the '{'
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return i + 1, j  # body start, the closing brace
    raise SystemExit("unclosed %s()" % name)


def code_lines(body):
    # the lines not commented out (// at their start)
    return [l for l in body.splitlines() if not l.strip().startswith("//")]


INC = re.compile(r'include_weapon\s*\(\s*"([^"]+)"')
ADD = re.compile(r'add_zombie_weapon\s*\(\s*"([^"]+)"')


def included(body):
    return {INC.search(l).group(1).lower() for l in code_lines(body) if INC.search(l)}


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(text.encode("latin-1"))


def main():
    ap = argparse.ArgumentParser(description="Make mods/wds_waw (wds and the WaW weapon mod in one game).")
    ap.add_argument("mods", nargs="?", default="mods", help="the mods folder (default: ./mods)")
    ap.add_argument("--wds", help="wds's folder (default: <mods>/wds)")
    ap.add_argument("--waw", help="the WaW weapon mod's folder (default: <mods>/WAW_Weapons_for_BO1)")
    ap.add_argument("--out", help="the folder to make (default: <mods>/wds_waw)")
    args = ap.parse_args()
    wds_dir = args.wds or os.path.join(args.mods, "wds")
    waw_dir = args.waw or os.path.join(args.mods, "WAW_Weapons_for_BO1")
    out = args.out or os.path.join(args.mods, "wds_waw")
    for d in (wds_dir, waw_dir):
        if not os.path.isdir(d):
            raise SystemExit("no folder %s (put the mod in the mods folder as it comes, or name it with --wds / --waw)" % d)
    wds, waw = ModFiles(wds_dir), ModFiles(waw_dir)

    # the guns wds registers itself (its _zombiemode_weapons.gsc's init_weapons)
    wds_weapons = wds.read("maps/_zombiemode_weapons.gsc")
    s, e = func_span(wds_weapons, "init_weapons")
    wds_registered = {ADD.search(l).group(1).lower() for l in code_lines(wds_weapons[s:e]) if ADD.search(l)}

    report = []
    for kind, sub, ext in (("gsc", "maps", ".gsc"), ("csc", "clientscripts", ".csc")):
        for m, label in MAPS:
            rel = sub + "/" + m + ext
            a = waw.read(rel)
            b = wds.read(rel)
            nl = "\r\n" if "\r\n" in b else "\n"
            sa, ea = func_span(a, "include_weapons")
            sb, eb = func_span(b, "include_weapons")
            have = included(b[sb:eb])
            incs, adds = [], []
            for l in code_lines(a[sa:ea]):
                mi, ma = INC.search(l), ADD.search(l)
                if mi and mi.group(1).lower() not in have:
                    incs.append(l.rstrip())
                elif ma and ma.group(1).lower() not in wds_registered and ma.group(1).lower() not in have:
                    adds.append(l.rstrip())
            # (the WaW mod registers some guns outside include_weapons in other maps: those too)
            if kind == "gsc":
                for l in code_lines(a[:sa] + a[ea:]):
                    ma = ADD.search(l)
                    if ma and ma.group(1).lower() not in wds_registered and ma.group(1).lower() not in have \
                            and l.rstrip() not in adds:
                        adds.append(l.rstrip())
            block = ["", "\t// mods/wds_waw: the WaW weapon mod's guns (mods/%s) that wds hasn't" % os.path.basename(
                os.path.normpath(waw_dir))] + incs
            if adds:
                block += ["", "\t// (their costs and hints: the WaW mod's registrations; wds registers its own in init_weapons)"] + adds
            merged = b[:eb].rstrip() + nl + nl.join(block) + nl + b[eb:]
            header = ("// WDS_WAW - not part of the original game or of either mod: wds's " + rel +
                      " with the WaW weapon mod's guns added (tools/merge_wds_waw.py)" + nl)
            write(os.path.join(out, sub, m + ext), header + merged)
            report.append("%s %s (%s): +%d include_weapon, +%d add_zombie_weapon" % (kind, m, label, len(incs), len(adds)))

    write(os.path.join(out, "mod.txt"), COMBINED_TXT.format(wds=os.path.basename(os.path.normpath(wds_dir)),
                                                             waw=os.path.basename(os.path.normpath(waw_dir)),
                                                             wds_id=mod_id(wds_dir), waw_id=mod_id(waw_dir)))
    for d, text in ((wds_dir, WDS_TXT), (waw_dir, WAW_TXT)):
        path = os.path.join(d, "mod.txt")
        if not os.path.exists(path):
            write(path, text)
            report.append("wrote %s" % path)
    print("\n".join(report))
    print("made %s: turn on both mods in the Mods menu" % out)


if __name__ == "__main__":
    main()
