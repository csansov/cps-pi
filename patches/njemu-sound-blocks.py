#!/usr/bin/env python3
"""cps-pi: patch applied by build.sh to the build copy of NJEMU, for the
sound in blocks.

NJEMU renders the frame's sound in one block at the end of the frame, from
the chips' state at that moment, so a register written early in the frame
is heard up to a frame late and the kernel's audio queue must hold a whole
frame. The patch adds a timer slot (SOUND_BLOCK) that fires at lines 64,
128 and 192 of the 262-line frame; lr_sound_block() (njemu_pi.c) renders a
quarter of the frame's sound each time, the vblank renders the last
quarter, and the kernel paces the emulation so that the blocks reach the
HDMI output a few milliseconds after they are produced.

    python patches/njemu-sound-blocks.py build/rpi3/njemu
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
        print("njemu-sound-blocks: %s already patched" % path)
        return
    if s.count(old_nl) != 1:
        sys.exit("njemu-sound-blocks: %s not found in %s" % (where, path))
    s = s.replace(old_nl, new_nl)
    with open(p, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write(s)
    print("njemu-sound-blocks: patched %s" % path)


patch("cps1/timer.h",
      "#define VBLANK_INTERRUPT\t4\n#define MAX_TIMER\t\t\t5\n",
      "#define VBLANK_INTERRUPT\t4\n#define SOUND_BLOCK\t\t\t5\t/* cps-pi: sound rendered in blocks */\n"
      "#define MAX_TIMER\t\t\t6\n",
      "timer slots")

patch("cps1/timer.c",
      "\ttimer_set_vblank_interrupt();\n\n\twhile (timer_left > 0)\n",
      "\ttimer_set_vblank_interrupt();\n"
      "\ttimer_set(SOUND_BLOCK, USECS_PER_SCANLINE * 64, 0, lr_sound_block);\t/* cps-pi */\n\n"
      "\twhile (timer_left > 0)\n",
      "frame start")
