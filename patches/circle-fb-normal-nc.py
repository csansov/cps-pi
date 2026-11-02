#!/usr/bin/env python3
"""cps-pi: Circle patch, the GPU memory (where the framebuffer lives) mapped
as Normal non-cacheable memory instead of Device-nGnRE.

Circle maps everything above the ARM memory size as Device memory. For the
peripherals that is right, but the framebuffer is ordinary SDRAM owned by
the GPU, and Device memory forbids the CPU to merge its stores: every
16 byte store is its own bus transaction and a Pi 3 writes the picture at
about 430 MB/s (10 ms for a 1195x896 picture). As Normal non-cacheable
memory the store buffer merges the writes into full lines; the GPU still
reads the real memory (nothing is cached), and the ordering with the page
flip is secured by the data synchronisation barrier in CVideoOut::Flip.

    python patches/circle-fb-normal-nc.py circle
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
        print("circle-fb-normal-nc: %s already patched" % path)
        return
    if s.count(old_nl) != 1:
        sys.exit("circle-fb-normal-nc: %s not found in %s" % (where, path))
    s = s.replace(old_nl, new_nl)
    with open(p, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write(s)
    print("circle-fb-normal-nc: patched %s" % path)


# a fourth memory attribute: Normal, non-cacheable
patch("include/circle/translationtable64.h",
      "#define ATTRINDX_COHERENT\t2\n",
      "#define ATTRINDX_COHERENT\t2\n#define ATTRINDX_NORMAL_NC\t3\t// cps-pi: GPU memory (framebuffer)\n",
      "ATTRINDX_COHERENT")

patch("lib/memory64.cpp",
      "\t                | 0x00 << ATTRINDX_COHERENT*8;\t// Device-nGnRnE\n",
      "\t                | 0x00 << ATTRINDX_COHERENT*8\t// Device-nGnRnE\n"
      "\t                | 0x44 << ATTRINDX_NORMAL_NC*8;\t// cps-pi: Normal non-cacheable\n",
      "MAIR_EL1 setup")

# the GPU memory below the peripherals gets the new attribute, the
# peripherals stay Device memory
patch("lib/translationtable64.cpp",
      "#if RASPPI >= 4\n"
      "\t\t\tif (   (   nBaseAddress >= m_nMemSize\n"
      "\t\t\t        && nBaseAddress < MEM_HIGHMEM_START)\n"
      "\t\t\t    || nBaseAddress > MEM_HIGHMEM_END)\n"
      "#else\n"
      "\t\t\tif (nBaseAddress >= m_nMemSize)\n"
      "#endif\n"
      "\t\t\t{\n"
      "\t\t\t\tpDesc->AttrIndx = ATTRINDX_DEVICE;\n"
      "\t\t\t\tpDesc->SH\t= ATTRIB_SH_OUTER_SHAREABLE;\n"
      "\t\t\t}\n",
      "#if RASPPI >= 4\n"
      "\t\t\tif (   nBaseAddress >= m_nMemSize\n"
      "\t\t\t    && nBaseAddress < MEM_HIGHMEM_START)\n"
      "\t\t\t{\n"
      "\t\t\t\t// cps-pi: GPU memory (framebuffer), see patches/circle-fb-normal-nc.py\n"
      "\t\t\t\tpDesc->AttrIndx = ATTRINDX_NORMAL_NC;\n"
      "\t\t\t\tpDesc->SH\t= ATTRIB_SH_OUTER_SHAREABLE;\n"
      "\t\t\t}\n"
      "\t\t\telse if (nBaseAddress > MEM_HIGHMEM_END)\n"
      "#else\n"
      "\t\t\tif (   nBaseAddress >= m_nMemSize\n"
      "\t\t\t    && nBaseAddress < ARM_IO_BASE)\n"
      "\t\t\t{\n"
      "\t\t\t\t// cps-pi: GPU memory (framebuffer), see patches/circle-fb-normal-nc.py\n"
      "\t\t\t\tpDesc->AttrIndx = ATTRINDX_NORMAL_NC;\n"
      "\t\t\t\tpDesc->SH\t= ATTRIB_SH_OUTER_SHAREABLE;\n"
      "\t\t\t}\n"
      "\t\t\telse if (nBaseAddress >= ARM_IO_BASE)\n"
      "#endif\n"
      "\t\t\t{\n"
      "\t\t\t\tpDesc->AttrIndx = ATTRINDX_DEVICE;\n"
      "\t\t\t\tpDesc->SH\t= ATTRIB_SH_OUTER_SHAREABLE;\n"
      "\t\t\t}\n",
      "device memory mapping")

# ARM_IO_BASE comes from bcm2835.h
p = os.path.join(root, "lib/translationtable64.cpp")
with open(p, encoding="utf-8", errors="surrogateescape", newline="") as f:
    s = f.read()
if "#include <circle/bcm2835.h>" not in s:
    nl = "\r\n" if "\r\n" in s else "\n"
    marker = "#include <circle/translationtable64.h>" + nl
    if marker not in s:
        sys.exit("circle-fb-normal-nc: include marker not found")
    s = s.replace(marker, marker + "#include <circle/bcm2835.h>" + nl, 1)
    with open(p, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write(s)
    print("circle-fb-normal-nc: added bcm2835.h include")
