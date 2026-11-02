#!/usr/bin/env python3
"""cps-pi: patch applied by build.sh to the build copy of NJEMU, for the
timing of the real board.

1. emucfg.h: the CPS1 frame is 262 lines of 512 pixel clocks at 8 MHz
   (MAME, verified on the board): 16768 us, 59.637405 Hz. NJEMU used a round
   60.0, which ran the games 0.6% fast; the kernel displays at the real rate
   (dist/sd/config.txt) and the sound is produced for it.
2. cps1/timer.c: the 68000 clock comes from lr_m68k_clock() (njemu_pi.c):
   10 MHz on the original CPS1, 12 MHz on the later boards (CPS-Dash and the
   QSound games), as MAME lists them; NJEMU had 10 MHz for every game.

    python patches/njemu-cps1-timing.py build/rpi3/njemu
"""
import os
import sys

root = sys.argv[1]


def patch(path, old, new, where):
    p = os.path.join(root, path)
    with open(p, encoding="utf-8", errors="surrogateescape", newline="") as f:
        s = f.read()
    nl = "\r\n" if "\r\n" in s else "\n"
    old_nl, new_nl = old.replace("\n", nl), new.replace("\n", nl)
    if new_nl in s:
        print("njemu-cps1-timing: %s already patched" % path)
        return
    if s.count(old_nl) != 1:
        sys.exit("njemu-cps1-timing: %s not found in %s" % (where, path))
    s = s.replace(old_nl, new_nl)
    with open(p, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write(s)
    print("njemu-cps1-timing: patched %s" % path)


patch("emucfg.h",
      "#define EMU_SYSTEM\t\t\t\tCPS1\n#define SYSTEM_NAME\t\t\t\t\"CPS1\"\n"
      "#define FPS\t\t\t\t\t\t60.0\n#define TICKS_PER_FRAME\t\t\t16666.666666\n",
      "#define EMU_SYSTEM\t\t\t\tCPS1\n#define SYSTEM_NAME\t\t\t\t\"CPS1\"\n"
      "#define FPS\t\t\t\t\t\t59.637405\t/* cps-pi: 8 MHz / (512 * 262), the real board */\n"
      "#define TICKS_PER_FRAME\t\t\t16768.0\n",
      "CPS1 FPS")

patch("cps1/timer.c",
      "\tcpu[CPU_M68000].usec_to_cycles = 10000000.0/1000000.0;\n"
      "\tcpu[CPU_M68000].cycles_to_usec = 1000000.0/10000000.0;\n",
      "\tcpu[CPU_M68000].usec_to_cycles = lr_m68k_clock()/1000000.0;\t/* cps-pi: 10 or 12 MHz */\n"
      "\tcpu[CPU_M68000].cycles_to_usec = 1000000.0/lr_m68k_clock();\n",
      "68000 clock")
