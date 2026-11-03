#!/bin/bash
# cps-pi build: ./build.sh [3|4|all] [all|cps1|cps2]   (host: Git Bash on Windows or Linux)
#
# "all" (the default) builds the kernel that carries both cores:
#   build/rpi<P>-cps1/   the frontend + the NJEMU CPS1 core (also a kernel on its own)
#   build/rpi<P>-cps2/   the same with the CPS2 core
#   build/rpi<P>/        kernel8.img: the frontend + both cores, each core's
#                        symbols prefixed cps1_ / cps2_ (objcopy) and joined by
#                        src/core_dispatch.c
# "cps1" or "cps2" builds that single-core kernel only.
#
# Needs in this directory:
#   toolchain/xpack-aarch64-none-elf-gcc-14.2.1-1.1   (and, on Windows,
#   toolchain/xpack-windows-build-tools-*/bin for GNU make)
#   circle/            rsta2/circle 7a81e1b, built with Config.mk RASPPI=3
#   circle-rpi4/       the same for RASPPI=4 (optional, target 4)
#   third_party/njemu-libretro   the NJEMU CPS1/CPS2 cores
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
# GNU make on Windows wants D:/... paths, not /d/...
ROOTW="$(cygpath -m "$ROOT" 2>/dev/null || echo "$ROOT")"
NJSRC="$ROOT/third_party/njemu-libretro/src"
TARGETS="${1:-3}"
[ "$TARGETS" = "all" ] && TARGETS="3 4"
WANT="${2:-all}"
case "$WANT" in
	all)  SYSTEMS="cps1 cps2"; UNIFIED=1 ;;
	cps1|cps2) SYSTEMS="$WANT"; UNIFIED=0 ;;
	*) echo "usage: $0 [3|4|all] [all|cps1|cps2]"; exit 1 ;;
esac

TC="$ROOT/toolchain/xpack-aarch64-none-elf-gcc-14.2.1-1.1/bin"
export PATH="$TC:$PATH"
for d in "$ROOT"/toolchain/xpack-windows-build-tools-*/bin; do
	[ -d "$d" ] && export PATH="$d:$PATH"
done

gen_makefile() {	# <build dir> <circle dir> <sys> <SYS>
	sed "s|@CIRCLEHOME@|$2|; s|@SRC@|$ROOTW/src|; s|@MINIZ@|$ROOTW/third_party/miniz|; s|@SYS@|$3|; s|@SYSU@|$4|" \
		"$ROOT/src/Makefile.in" > "$1/Makefile"
}

BUILT=""
for P in $TARGETS; do
	CIRCLE="$ROOTW/circle"
	[ "$P" = "4" ] && CIRCLE="$ROOTW/circle-rpi4"

	# ---- one core each ----
	for S in $SYSTEMS; do
		SU=$(echo "$S" | tr a-z A-Z)
		B="$ROOT/build/rpi$P-$S"
		rm -rf "$B"; mkdir -p "$B"
		cp "$ROOT"/src/*.c "$ROOT"/src/*.cpp "$B"/
		rm -f "$B/core_dispatch.c"
		cp "$ROOT"/third_party/miniz/miniz.c "$B"/
		# the core: a private copy, with our platform header and frame lifecycle in
		# place of the libretro port's (the sound is produced at the vblank)
		cp -r "$NJSRC" "$B/njemu"
		cp "$ROOT/src/libretro/platform.h" "$B/njemu/libretro/platform.h"
		cp "$ROOT/src/libretro/lifecycle.inc" "$B/njemu/libretro/lifecycle.inc"
		# controls sampled at the emulated vblank, when the game reads them
		python "$ROOT/patches/njemu-vblank-input.py" "$B/njemu/$S/driver.c"
		# the real frame period (59.637 Hz) and the 68000 clock from the glue
		python "$ROOT/patches/njemu-cps1-timing.py" "$B/njemu" "$S"
		# the frame's sound rendered in four blocks, handed over as they are made
		python "$ROOT/patches/njemu-sound-blocks.py" "$B/njemu" "$S"
		gen_makefile "$B" "$CIRCLE" "$S" "$SU"
		make -C "$B" -j8
		BUILT="$BUILT $B"
	done

	# ---- both cores in one kernel ----
	if [ "$UNIFIED" = 1 ]; then
		U="$ROOT/build/rpi$P"
		rm -rf "$U"; mkdir -p "$U/cores"
		for S in $SYSTEMS; do
			B="$ROOT/build/rpi$P-$S"
			OBJS="$(find "$B/njemu" -name '*.o') $B/njemu_pi.o"
			# every symbol the core defines gets the prefix; references to the
			# kernel (cpspi_*, the C library) are undefined there and stay as they are
			aarch64-none-elf-nm -g --defined-only $OBJS \
				| awk -v s="$S" '$2 ~ /^[TtDdBbRrCVWG]$/ && $3 != "" {print $3, s "_" $3}' \
				| sort -u > "$U/cores/$S.syms"
			for o in $OBJS; do
				rel="${o#$B/}"
				aarch64-none-elf-objcopy --redefine-syms="$U/cores/$S.syms" "$o" "$U/cores/$S-$(echo "$rel" | tr '/' '_')"
			done
			echo "$S: $(wc -l < "$U/cores/$S.syms") symbols prefixed"
		done
		cp "$ROOT"/src/*.c "$ROOT"/src/*.cpp "$U"/
		rm -f "$U/njemu_pi.c"
		cp "$ROOT"/third_party/miniz/miniz.c "$U"/
		gen_makefile "$U" "$CIRCLE" "all" "ALL"
		make -C "$U" -j8
		BUILT="$BUILT $U"
	fi
done

# sanity check: the whole kernel (code + data + bss) must fit in KERNEL_MAX_SIZE
for B in $BUILT; do
	ELF=$(ls "$B"/*.elf)
	END=$(aarch64-none-elf-nm "$ELF" | awk '$3=="__bss_end"||$3=="_end"{print $1}' | sort | tail -1)
	LIMIT=$((0x80000 + 0x1000000))
	if [ $((16#$END)) -ge $LIMIT ]; then echo "ERROR: $(basename "$B") too large (ends at 0x$END)"; exit 1; fi
	echo "$(basename "$B"): kernel ends at 0x$END (limit $(printf 0x%x $LIMIT)) OK"
done
