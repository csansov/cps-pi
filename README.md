# cps-pi

A bare-metal Capcom CPS1 arcade emulator for the Raspberry Pi 3 and 4.
Power on, and a few seconds later the game list is on the screen: no Linux,
no desktop, nothing to configure.

**cps-pi is a derivative of [neo-pi](https://github.com/alessioscand/neo-pi)**,
Alessio Scanderebech's bare-metal Neo Geo emulator, and keeps its whole
philosophy: one system, done well, with the lowest latency the hardware
allows. Where neo-pi runs the Geolith Neo Geo core, cps-pi runs the CPS1 core
of [NJEMU](https://github.com/HonkeyKong/NJEMU-Libretro) (NJ's CPS1PSP, as
maintained by fjtrujy and HonkeyKong). The frontend, the boot sequence, the
menus, the pad handling and the video and audio paths are neo-pi's, adapted to
the CPS1 picture and controls.

Status: **0.1.0, first iteration**. It builds and links; it has not yet been
run on real hardware. Expect rough edges.

## Why

The CPS1 board (Street Fighter II, Final Fight, Ghouls'n Ghosts, Strider,
Captain Commando, The Punisher, Cadillacs and Dinosaurs ...) is, with the
Neo Geo, the other great 2D arcade platform of the early nineties, and it is
not served by any bare-metal emulator. Linux distributions for the Pi boot
slowly, add input and display latency through their stacks, and need care to
shut down. neo-pi proved that a Pi 3 can run a 16-bit arcade system from a
kernel that owns the whole machine, with vsync-locked video, 1 ms USB
polling and HDMI audio. cps-pi brings that approach to the CPS1.

## How it is put together

| Part | Origin | Role |
| --- | --- | --- |
| Frontend (`src/kernel.cpp`, `video.cpp`, `ui.cpp`, `input.cpp`, `audio.cpp`, `profile.cpp`) | neo-pi, adapted | boot, SD card, game list, menus, USB pads, integer scaling with the HVS hardware scaler, HDMI audio with drift correction, log, benchmark |
| Emulation (`third_party/njemu-libretro/src`) | NJEMU, unmodified | 68000 (C68K), Z80 (CZ80), CPS-A/CPS-B video, YM2151 + OKI6295 and QSound sound, Kabuki decryption, ROM database for 137 sets |
| Glue (`src/njemu_pi.c`) | new | the core's platform layer: software sprite renderer into a BGR555 buffer, one frame of sound per emulated frame resampled to 48 kHz, inputs, paths |
| C library (`src/shim.c`, `src/pi_file.c`) | new | printf family, sscanf, strtok, ctype; open/read/lseek and fopen/fread/fseek on FatFs |
| Bare-metal environment | [Circle](https://github.com/rsta2/circle) by Rene Stange | CPU, interrupts, framebuffer, USB, SD card, HDMI audio, FatFs |
| zip and PNG | [miniz](https://github.com/richgel999/miniz) | reads the ROM zips (as zlib for NJEMU's unzip), writes screenshots |

NJEMU is built in its frontend-driven configuration (the one its libretro
port uses): one synchronous frame per call, no threads, no GUI, software
rendering. The core source is not modified; `build.sh` copies it into the
build directory and drops in cps-pi's own `libretro/platform.h`.

## Features

- Game list from `cpspi/roms/*.zip`, titles from NJEMU's database, last game
  remembered, clones shown only when their parent is present.
- 384x224 picture at an integer scale (4x at 1080p, done by the hardware
  scaler), vsync paced at 60 Hz, scanline filters in three strengths.
- HDMI audio at 48 kHz; the chips run at their native 44.1 kHz (YM2151 + OKI)
  or 24 kHz (QSound) and are resampled.
- USB gamepads (six buttons, Start, Select) and keyboard, USB polled every
  1 ms, per-model control set-up saved in `cpspi/pads.cfg`.
- In-game menu (hold Select for a second, or Esc): reset, service menu (the
  TEST switch: DIP switches and board tests), game list, power off. Select+1
  takes a screenshot, Select+2 shows the status line (FPS, load, temperature),
  Select+3 the credits. A short Select press inserts a coin.
- EEPROM contents saved in `cpspi/nvram`, diagnostics in `cpspi/LOG.TXT`,
  benchmark and sampling profiler with an empty `cpspi/BENCH` file.

Not yet: save states, DIP switch editor, vertical games drawn upright (they
run, but sideways), the multicore and beam-racing refinements of neo-pi,
Pi 4 kernel.

## SD card

Format a card as FAT32 and copy the contents of `dist/sd` (from a release, or
after building) to its root: the Raspberry Pi firmware, `config.txt`,
`cmdline.txt`, `kernel8.img` and the `cpspi` folder with `rominfo.cps1` and
`zipname.cps1`. Put the games in `cpspi/roms`.

`config.txt` selects 1080p at 60 Hz; for 720p change `hdmi_mode=16` to
`hdmi_mode=4`. The `[pi3]` section has a commented overclock (`arm_freq=1300`,
`over_voltage=2`): enable it only with a heatsink.

## ROM sets

NJEMU identifies every ROM by CRC, in the sets listed in `cpspi/rominfo.cps1`
(MAME 0.152 era). A clone needs its parent zip in the same folder. Only the
CRCs matter, so a newer set whose ROM contents are the same loads too,
whatever the file names. No games are included.

## Controls

Pads with a known layout (Xbox and compatible): X Y RB = buttons 1 2 3
(punches), A B RT = buttons 4 5 6 (kicks), Start, Back = coin. Generic HID
pads and arcade encoders: buttons 1-6, button 9 = Select, 10 = Start. The
first time a pad model is seen, cps-pi asks for its eight buttons and saves
the answer. Keyboard: arrows, A S D and Z X C, Enter, 5 or Space, Esc.

## Building

Host: Windows (Git Bash) or Linux. Tools: the xPack
[aarch64-none-elf-gcc](https://github.com/xpack-dev-tools/aarch64-none-elf-gcc-xpack)
14.2.1 toolchain (plus xPack Windows Build Tools for GNU make on Windows),
Circle at commit 7a81e1b with neo-pi's two patches (USB polling, XInput pads),
Python 3 for the patches, and the NJEMU source. `./build.sh 3` produces
`build/rpi3/kernel8.img`. `BUILD.md` has the step-by-step instructions.

## Credits

- [neo-pi](https://github.com/alessioscand/neo-pi) by Alessio Scanderebech:
  the base of this project.
- [NJEMU](https://github.com/HonkeyKong/NJEMU-Libretro): NJ (original PSP
  emulator), fjtrujy (multi-platform driver architecture),
  HonkeyKong (libretro port with the software renderer that cps-pi uses).
  C68K and CZ80 by Stephane Dallaire; sound chip emulation from MAME.
- [Circle](https://github.com/rsta2/circle) by Rene Stange.
- [miniz](https://github.com/richgel999/miniz) by Rich Geldreich.
- Menu font "Matinee" (ZX Origins) by Damien Guard.

## Licence

GPLv3, see `LICENSE`. neo-pi, NJEMU and Circle are GPLv3; miniz is MIT; the
Raspberry Pi firmware in `dist/sd` is distributed under `LICENCE.broadcom`.
CPS, Capcom and the game names are trademarks of their owners. No games or
other copyrighted ROM data are included.
