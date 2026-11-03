#!/usr/bin/env python3
"""cps-pi: package dist/sd as dist/cps-pi-<version>-sd.zip, without any ROM
or user data (run after build.sh and after copying the kernel to dist/sd)."""
import os
import re
import sys
import zipfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sd = os.path.join(root, "dist", "sd")
with open(os.path.join(root, "src", "cpspi.h"), encoding="utf-8") as f:
    version = re.search(r'CPSPI_VERSION\s+"([^"]+)"', f.read()).group(1)
out = os.path.join(root, "dist", "cps-pi-%s-sd.zip" % version)

if not os.path.exists(os.path.join(sd, "kernel8.img")):
    sys.exit("dist/sd/kernel8.img missing: build first and copy it there")

names = []
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for dp, dn, fn in os.walk(sd):
        for name in fn:
            path = os.path.join(dp, name)
            rel = os.path.relpath(path, sd).replace(os.sep, "/")
            if rel.startswith("cpspi/roms/") and rel.lower().endswith(".zip"):
                continue
            if rel.startswith("cpspi/nvram/") or rel.startswith("cpspi/shots/"):
                continue
            if rel.upper() in ("CPSPI/LOG.TXT", "CPSPI/LAST.TXT", "CPSPI/OVERLAY.PNG"):
                continue		# the bezel is the user's own, never distributed
            z.write(path, rel)
            names.append(rel)
    z.writestr("cpspi/roms/put-your-cps1-zips-here.txt",
               "Copy your CPS1 ROM sets (MAME-style zips, see README.md) into this folder.\n")
    names.append("cpspi/roms/put-your-cps1-zips-here.txt")

print("%s: %d files, %d bytes" % (out, len(names), os.path.getsize(out)))
for n in sorted(names):
    print("  " + n)
