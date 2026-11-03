#!/bin/bash
# cps-pi build: ./build.sh [3|4|all]   (host: Git Bash on Windows or Linux)
#
# Needs in this directory:
#   toolchain/xpack-aarch64-none-elf-gcc-14.2.1-1.1   (and, on Windows,
#   toolchain/xpack-windows-build-tools-*/bin for GNU make)
#   circle/            rsta2/circle 7a81e1b, built with Config.mk RASPPI=3
#   circle-rpi4/       the same for RASPPI=4 (optional, target 4)
#   third_party/njemu-libretro   the NJEMU CPS1 core
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
# GNU make on Windows wants D:/... paths, not /d/...
ROOTW="$(cygpath -m "$ROOT" 2>/dev/null || echo "$ROOT")"
NJSRC="$ROOT/third_party/njemu-libretro/src"
TARGETS="${1:-3}"
[ "$TARGETS" = "all" ] && TARGETS="3 4"

TC="$ROOT/toolchain/xpack-aarch64-none-elf-gcc-14.2.1-1.1/bin"
export PATH="$TC:$PATH"
for d in "$ROOT"/toolchain/xpack-windows-build-tools-*/bin; do
	[ -d "$d" ] && export PATH="$d:$PATH"
done

for P in $TARGETS; do
	B="$ROOT/build/rpi$P"
	rm -rf "$B"; mkdir -p "$B"
	# our sources, flat
	cp "$ROOT"/src/*.c "$ROOT"/src/*.cpp "$B"/
	cp "$ROOT"/third_party/miniz/miniz.c "$B"/
	# the core: a private copy, with our platform header and frame lifecycle in
	# place of the libretro port's (the sound is produced at the vblank)
	cp -r "$NJSRC" "$B/njemu"
	cp "$ROOT/src/libretro/platform.h" "$B/njemu/libretro/platform.h"
	cp "$ROOT/src/libretro/lifecycle.inc" "$B/njemu/libretro/lifecycle.inc"
	# controls sampled at the emulated vblank, when the game reads them
	python "$ROOT/patches/njemu-vblank-input.py" "$B/njemu/cps1/driver.c"
	# the real frame period (59.637 Hz) and the 68000 at 12 MHz where the board had it
	python "$ROOT/patches/njemu-cps1-timing.py" "$B/njemu"
	# the frame's sound rendered in four blocks, handed over as they are made
	python "$ROOT/patches/njemu-sound-blocks.py" "$B/njemu"
	CIRCLE="$ROOTW/circle"
	[ "$P" = "4" ] && CIRCLE="$ROOTW/circle-rpi4"
	sed "s|@CIRCLEHOME@|$CIRCLE|; s|@SRC@|$ROOTW/src|; s|@MINIZ@|$ROOTW/third_party/miniz|" \
		"$ROOT/src/Makefile.in" > "$B/Makefile"
	make -C "$B" -j8
done

# sanity check: the whole kernel (code + data + bss) must fit in KERNEL_MAX_SIZE
for P in $TARGETS; do
	ELF=$(ls "$ROOT"/build/rpi$P/*.elf)
	END=$(aarch64-none-elf-nm "$ELF" | awk '$3=="__bss_end"||$3=="_end"{print $1}' | sort | tail -1)
	LIMIT=$((0x80000 + 0x1000000))
	if [ $((16#$END)) -ge $LIMIT ]; then echo "ERROR: kernel rpi$P too large (ends at 0x$END)"; exit 1; fi
	echo "rpi$P: kernel ends at 0x$END (limit $(printf 0x%x $LIMIT)) OK"
done
