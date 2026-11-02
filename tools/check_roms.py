#!/usr/bin/env python3
"""cps-pi: check CPS1 ROM zips against NJEMU's database before copying them
to the card.

    python tools/check_roms.py <roms folder> [rominfo.cps1]

For every <set>.zip in the folder: is the set known, is every ROM (by CRC)
present in the zip or in its parent zip, and if not, which database set the
zip's contents do satisfy (the file may just need another name).
"""
import os
import re
import sys
import zipfile

LINE = re.compile(r"^\s*(FILENAME|ROMX?|REGION|END)\s*\(?\s*(.*?)\s*\)?\s*$")


def parse_db(path):
    """{set: {'parent': str, 'roms': [(name, crc)]}}"""
    sets, cur = {}, None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.match(line)
            if not m:
                continue
            kind, args = m.group(1), [a.strip() for a in m.group(2).split(",")]
            if kind == "FILENAME" and len(args) >= 2:
                cur = {"parent": args[1], "roms": []}
                sets[args[0]] = cur
            elif kind in ("ROM", "ROMX") and cur is not None and len(args) >= 5:
                name, crc = args[1], args[4]
                if name.startswith("0x"):
                    continue
                try:
                    crc = int(crc, 16)
                except ValueError:
                    continue
                if crc:
                    cur["roms"].append((name, crc))
            elif kind == "END":
                cur = None
    return sets


def zip_crcs(path):
    try:
        with zipfile.ZipFile(path) as z:
            return {i.CRC & 0xFFFFFFFF: i.filename for i in z.infolist() if not i.is_dir()}
    except (zipfile.BadZipFile, OSError) as e:
        print("  cannot read %s: %s" % (path, e))
        return None


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    folder = sys.argv[1]
    db = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "dist", "sd", "cpspi", "rominfo.cps1")
    sets = parse_db(db)
    print("database: %d sets (%s)" % (len(sets), db))

    zips = sorted(f for f in os.listdir(folder) if f.lower().endswith(".zip"))
    crcs = {}
    for f in zips:
        c = zip_crcs(os.path.join(folder, f))
        if c is not None:
            crcs[f[:-4].lower()] = c

    ok = 0
    for name in sorted(crcs):
        have = crcs[name]
        if name not in sets:
            # which set does this zip satisfy on its own?
            matches = [s for s, d in sets.items() if d["roms"] and all(c in have for _, c in d["roms"])]
            print("%-10s NOT a supported set%s" % (name, (", but its contents match: " + ", ".join(matches)) if matches else ""))
            continue
        d = sets[name]
        pool = dict(have)
        parent = d["parent"] if d["parent"] != "cps1" else None
        note = ""
        if parent:
            if parent in crcs:
                pool.update(crcs[parent])
            else:
                note = " (parent %s.zip missing)" % parent
        missing = [(n, c) for n, c in d["roms"] if c not in pool]
        if not missing:
            print("%-10s OK  (%d ROMs%s)" % (name, len(d["roms"]), ", parent " + parent if parent else ""))
            ok += 1
            continue
        print("%-10s MISSING %d of %d ROMs%s:" % (name, len(missing), len(d["roms"]), note))
        for n, c in missing:
            print("             %-16s crc %08x" % (n, c))
        matches = [s for s, e in sets.items() if s != name and e["roms"]
                   and all(c in have for _, c in e["roms"])]
        if matches:
            print("             the zip's contents do match the set%s: %s -> rename the file"
                  % ("s" if len(matches) > 1 else "", ", ".join(matches)))
        else:
            # closest sets of the same family
            best = sorted(((sum(1 for _, c in e["roms"] if c in have), s) for s, e in sets.items() if e["roms"]), reverse=True)[:3]
            print("             closest sets: " + ", ".join("%s (%d ROMs present)" % (s, k) for k, s in best))
    print("%d of %d zips ready to play" % (ok, len(crcs)))


if __name__ == "__main__":
    main()
