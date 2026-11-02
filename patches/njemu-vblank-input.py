#!/usr/bin/env python3
"""cps-pi: patch applied by build.sh to the build copy of NJEMU's
cps1/driver.c. The core latches the controls at the start of the emulated
frame, but the game reads them in its vblank interrupt handler, at the end:
a frame of lag for nothing. The patch calls lr_vblank_input() (njemu_pi.c)
right before the vblank interrupt is raised, so the kernel can sample the
pads at that moment, as the real board reads its input ports.

    python patches/njemu-vblank-input.py build/rpi3/njemu/cps1/driver.c
"""
import sys

path = sys.argv[1]
with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
    s = f.read()

nl = "\r\n" if "\r\n" in s else "\n"		# a git checkout on Windows may use CRLF
old = "TIMER_CALLBACK( cps1_vblank_interrupt )" + nl + "{" + nl + "\tm68000_set_irq_line(2, HOLD_LINE);" + nl
new = ("TIMER_CALLBACK( cps1_vblank_interrupt )" + nl + "{" + nl
       + "\tlr_vblank_input();\t/* cps-pi: fresh controls for the handler */" + nl
       + "\tm68000_set_irq_line(2, HOLD_LINE);" + nl)
if new in s:
    print("njemu-vblank-input: already applied")
    sys.exit(0)
if s.count(old) != 1:
    sys.exit("njemu-vblank-input: cps1_vblank_interrupt not found in %s" % path)
s = s.replace(old, new)
with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
    f.write(s)
print("njemu-vblank-input: patched %s" % path)
