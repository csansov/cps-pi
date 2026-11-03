#!/usr/bin/env python3
"""cps-pi: patch applied by build.sh to the build copy of NJEMU, for the
timing of the real board.

1. emucfg.h: the CPS1 frame is 262 lines of 512 pixel clocks at 8 MHz
   (MAME, verified on the board): 16768 us, 59.637405 Hz. NJEMU used a round
   60.0, which ran the games 0.6% fast; the kernel displays at the real rate
   (dist/sd/config.txt) and the sound is produced for it.
2. cps1/timer.c: the 68000 clock comes from lr_m68k_clock() (njemu_pi.c):
   10 MHz on the original CPS1, 12 MHz on the later boards (CPS-Dash and the
   QSound games), as MAME lists them, less the share that stands in for the
   board's wait states; NJEMU had 10 MHz for every game. timer_set_m68k_clock()
   applies it at reset and whenever the share changes while playing.

3. CPS2 (second argument cps2): the same frame period (the CPS2 video
   timing is the CPS1's: NJEMU had 59.633 Hz) and the 68000 clock from
   lr_m68k_clock() in place of the 11.8 MHz NJEMU copied from MAME (16 MHz
   less the share that stands in for the wait states).

    python patches/njemu-cps1-timing.py build/rpi3/njemu [cps1|cps2]
"""
import os
import sys

root = sys.argv[1]
system = sys.argv[2] if len(sys.argv) > 2 else "cps1"


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


if system == "cps2":
    patch("emucfg.h",
          "#define EMU_SYSTEM\t\t\t\tCPS2\n#define SYSTEM_NAME\t\t\t\t\"CPS2\"\n"
          "#define FPS\t\t\t\t\t\t59.633333\n#define TICKS_PER_FRAME\t\t\t16769.144773\n",
          "#define EMU_SYSTEM\t\t\t\tCPS2\n#define SYSTEM_NAME\t\t\t\t\"CPS2\"\n"
          "#define FPS\t\t\t\t\t\t59.637405\t/* cps-pi: 8 MHz / (512 * 262), as the CPS1 */\n"
          "#define TICKS_PER_FRAME\t\t\t16768.0\n",
          "CPS2 FPS")

    patch("cps2/timer.c",
          "\t\tm68000_execute((int)(timer_ticks * (11800000.0 / 1000000.0)));\n",
          "\t\tm68000_execute((int)(timer_ticks * m68k_cycles_per_usec));\t/* cps-pi */\n",
          "68000 execute")

    patch("cps2/timer.c",
          "void timer_reset(void)\n{\n",
          "/* cps-pi: the 68000 clock from lr_m68k_clock() (njemu_pi.c): 16 MHz less\n"
          "   the share that stands in for the wait states (NJEMU had MAME's flat\n"
          "   11.8 MHz); at reset and whenever the share changes while the game runs */\n"
          "static float m68k_cycles_per_usec = 11.8;\n"
          "void timer_set_m68k_clock(void)\n{\n"
          "\tm68k_cycles_per_usec = lr_m68k_clock()/1000000.0;\n}\n\n"
          "void timer_reset(void)\n{\n",
          "timer_reset")

    patch("cps2/timer.c",
          "\ttime_slice = 1000000.0 / FPS;\n\n\ttimer_set(QSOUND_INTERRUPT, TIME_IN_HZ(251), 0, qsound_interrupt);\n}\n",
          "\ttime_slice = 1000000.0 / FPS;\n\ttimer_set_m68k_clock();\t/* cps-pi */\n\n"
          "\ttimer_set(QSOUND_INTERRUPT, TIME_IN_HZ(251), 0, qsound_interrupt);\n}\n",
          "timer_reset body")
else:
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
          "\ttimer_set_m68k_clock();\t/* cps-pi: 10 or 12 MHz, less the wait states' share */\n",
          "68000 clock")

    patch("cps1/timer.c",
          "void timer_reset(void)\n{\n",
          "/* cps-pi: the 68000 clock from lr_m68k_clock() (njemu_pi.c), at reset and\n"
          "   whenever the share of the clock changes while the game runs */\n"
          "void timer_set_m68k_clock(void)\n{\n"
          "\tcpu[CPU_M68000].usec_to_cycles = lr_m68k_clock()/1000000.0;\n"
          "\tcpu[CPU_M68000].cycles_to_usec = 1000000.0/lr_m68k_clock();\n}\n\n"
          "void timer_reset(void)\n{\n",
          "timer_reset")
