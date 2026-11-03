#!/usr/bin/env python3
"""cps-pi: patch applied by build.sh to the build copy of NJEMU's
cps1/driver.c, for the vblank of the real board.

The core latches the controls at the start of the emulated frame, but the
game reads them in its vblank interrupt handler, at the end: a frame of lag
for nothing. The patch reorders the vblank callback so that the picture of
the frame is rendered first (timed by lr_render_begin/end), then
lr_vblank_input() (njemu_pi.c) hands the finished picture and sound to the
kernel, which shows them, waits until just before the display's vsync and
samples the pads at that moment, as the board reads its ports at its vblank;
only then is the interrupt raised for the handler. Nothing changes for the
emulation (no CPU runs between those calls); only when the host does its
part.

    python patches/njemu-vblank-input.py build/rpi3/njemu/cps1/driver.c
"""
import sys

path = sys.argv[1]
with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
    s = f.read()

nl = "\r\n" if "\r\n" in s else "\n"		# a git checkout on Windows may use CRLF
old = ("TIMER_CALLBACK( cps1_vblank_interrupt )\n{\n"
       "\tm68000_set_irq_line(2, HOLD_LINE);\n"
       "\tif (!skip_this_frame())\n\t{\n\t\tcps1_screenrefresh();\n\t\tblit_finish();\n\t}\n"
       "\tcps1_objram_latch();\n}\n").replace("\n", nl)
new = ("TIMER_CALLBACK( cps1_vblank_interrupt )\n{\n"
       "\t/* cps-pi: the picture first, then the kernel shows it and reads the\n"
       "\t   controls as late as the vsync allows (lr_vblank_input), then the\n"
       "\t   interrupt whose handler reads them (patches/njemu-vblank-input.py) */\n"
       "\tlr_render_begin();\n"
       "\tif (!skip_this_frame())\n\t{\n\t\tcps1_screenrefresh();\n\t\tblit_finish();\n\t}\n"
       "\tcps1_objram_latch();\n"
       "\tlr_render_end();\n"
       "\tlr_vblank_input();\n"
       "\tm68000_set_irq_line(2, HOLD_LINE);\n}\n").replace("\n", nl)
if new in s:
    print("njemu-vblank-input: already applied")
    sys.exit(0)
if s.count(old) != 1:
    sys.exit("njemu-vblank-input: cps1_vblank_interrupt not found in %s" % path)
s = s.replace(old, new)
with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
    f.write(s)
print("njemu-vblank-input: patched %s" % path)
