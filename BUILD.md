# cps-pi - how to build (version 0.3.1)

Build host: Windows (Git Bash) or Linux. Everything lives in this directory;
nothing is installed system wide.

1. **Toolchain**: xPack aarch64-none-elf-gcc 14.2.1-1.1 (newlib) unpacked in
   `toolchain/xpack-aarch64-none-elf-gcc-14.2.1-1.1`
   (https://github.com/xpack-dev-tools/aarch64-none-elf-gcc-xpack/releases).
   On Windows also xPack Windows Build Tools (GNU make) in
   `toolchain/xpack-windows-build-tools-<version>`; `build.sh` adds both to
   the PATH.
2. **Circle** (rsta2/circle) commit 7a81e1b cloned into `circle/`, with this
   `Config.mk`:

   ```
   AARCH = 64
   RASPPI = 3
   PREFIX64 = aarch64-none-elf-
   STDLIB_SUPPORT = 1
   DEFINE += -DKERNEL_MAX_SIZE=0x1000000
   DEFINE += -DARM_ALLOW_MULTI_CORE
   ```

   then the neo-pi patches (USB pads polled every 1 ms, third-party XInput
   pads on the Xbox 360 driver):

       python patches/circle-usb-poll.py circle
       python patches/circle-xinput.py circle

   then, with the toolchain in the PATH, `./makeall` in `circle/` and `make`
   in `circle/addon/SDCard` and `circle/addon/fatfs`.
   A Pi 4 kernel needs a second copy, `circle-rpi4/`, built with `RASPPI = 4`.
3. **NJEMU** cloned into `third_party/njemu-libretro`
   (https://github.com/HonkeyKong/NJEMU-Libretro), unmodified. `build.sh`
   copies its `src` tree into the build directory and replaces
   `libretro/platform.h` with `src/libretro/platform.h`; the core is compiled
   with `-DLIBRETRO -DBUILD_CPS1 -DNO_GUI` (its frontend-driven configuration)
   and `-O3`, the rest with `-O2`.
4. **miniz** (`third_party/miniz`, from neo-pi's Geolith tree) stands in for
   zlib (`src/compat/zlib.h`) and encodes the PNG screenshots.
5. `./build.sh 3` builds `build/rpi3/kernel8.img` (`./build.sh 4` the Pi 4
   kernel, `./build.sh all` both).
6. **Raspberry Pi firmware**: the files in `dist/sd` come from neo-pi's
   release (raspberrypi/firmware commit 0641c5bf, the one Circle uses).

## Source files
- `src/kernel.cpp` - start-up, settings, game list, game loop, menus,
  benchmark, log
- `src/njemu_pi.c` - glue to the NJEMU core: runtime globals, software video
  driver (BGR555 work buffer), one frame of sound per emulated frame
  resampled to 48 kHz, inputs, the `core_*` API of `core_api.h`
- `src/shim.c` - the C library the core needs and Circle does not provide
  (printf family, sscanf, strtok, ctype table, exit/abort ...)
- `src/pi_file.c` - open/read/lseek/close and fopen/fread/fseek/ftell/fwrite
  on the SD card (FatFs), for the ROM zips and the EEPROM files
- `src/video.cpp` - double-buffered framebuffer, HVS/CPU integer scaling,
  scanlines, text in the black bands (from neo-pi)
- `src/ui.cpp` - menus drawn at CPS1 resolution (from neo-pi)
- `src/input.cpp` - USB pads and keyboard, six-button control set-up (from neo-pi)
- `src/audio.cpp` - HDMI audio with drift correction (from neo-pi, unchanged)
- `src/profile.cpp` - sampling profiler used by the benchmark (from neo-pi)
- `src/fontdata.c` - menu font ("Matinee", ZX Origins by Damien Guard)
- `src/libretro/platform.h`, `src/libretro/lifecycle.inc` - the core's
  platform header and the load/reset/frame/unload lifecycle of its libretro port
- `src/Makefile.in`, `build.sh`

## Benchmark and profile
An empty file `cpspi/BENCH` (no extension) on the card runs a benchmark on the
last played game before the list: 30 s of the game's demo, then 60 s measured
at full speed. `cpspi/BENCH.TXT` has the frame times and the slowest frames;
`cpspi/PROFILE.TXT` has where the time went, sampled every 200 us. To read it,
keep the `.elf` of the kernel that produced it and use neo-pi's
`tools/resolve_profile.py`.
