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

Status: **0.5.0**. Runs on a Raspberry Pi 3 Model B at the board's 59.64 fps
with no dropped frames (Street Fighter II' Champion Edition, 68000 at
12 MHz: emulation 6-8 ms per frame in play, worst 11 ms; the picture is
written in about 1.5 ms by the four cores). Expect rough edges.

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
build directory, drops in cps-pi's own `libretro/platform.h` and
`libretro/lifecycle.inc` (the frame's sound is produced at the vblank, with
the picture) and applies the two build-time patches in `patches/` (the
vblank callback order and the board's timing).

## Features

- Game list from `cpspi/roms/*.zip`, titles from NJEMU's database, last game
  remembered, clones shown only when their parent is present.
- The 384x224 picture shown as on the arcade monitor, 4:3 (1194x896 at
  1080p: integer 4x vertically; the 384 columns are spread over 597
  framebuffer columns by the CPU, blended between neighbours like a CRT or
  "sharp", and the hardware scaler doubles the result), or with square
  pixels (integer scale, hardware scaler); vsync paced at 60 Hz; scanline
  filters in three strengths (medium by default), fine on a Pi 3.
- The timing of the board: the frame is 262 lines of 512 pixel clocks at
  8 MHz, 59.637 Hz, and `config.txt` sets an HDMI mode at exactly that rate,
  so one emulated frame goes out per refresh with nothing sped up, slowed
  down, dropped or repeated (NJEMU used a round 60 Hz). The 68000 runs at
  12 MHz on the sets MAME lists with that clock (the CPS-Dash family:
  Street Fighter II' and later, the QSound games, Pang! 3 ...), 10 MHz on
  the rest.
- The board's input timing. The core's vblank callback hands the finished
  picture and sound of the frame to the kernel *before* raising the
  interrupt whose handler reads the input ports; the kernel flips the
  picture, waits until the display's vsync is about two milliseconds away
  (the margin adapts to what is measured), samples the pads at that moment
  and only then lets the handler run. The
  game reads controls as fresh as on the board (which reads its ports at
  its own vblank, 1.4 ms before the next frame), the emulation of the next
  frame starts right at the vsync, and nothing needs tuning: the slow part
  of the frame runs first and the wait comes after it, whatever the frame
  cost. Input to photon is the board's: the frame after the one being
  scanned, plus the game's own latency.
- The four cores write the picture to the framebuffer (a quarter each),
  about 1.5 ms instead of 4.5 on a Pi 3; the core's own sprite renderer has
  a 1:1 fast path for every tile and object.
- HDMI audio at 48 kHz, about 35 ms from the chip to the output (a queue
  that absorbs a lost display frame without a gap or a change of pitch);
  the chips run at their native 44.1 kHz (YM2151 + OKI) or 24 kHz (QSound)
  and are resampled.
- USB gamepads (six buttons, Start, Select) and keyboard, USB polled every
  1 ms, per-model control set-up saved in `cpspi/pads.cfg`.
- In-game menu (hold Select for a second, or Esc): reset, service menu (the
  TEST switch: DIP switches and board tests), game list, power off. Select+1
  takes a screenshot, Select+2 shows the status line (FPS, load, temperature),
  Select+3 the credits. A short Select press inserts a coin.
- OPTIONS from the game list (button 4): picture 4:3 or pixel, scanlines,
  controls, power off; saved in `cpspi/cpspi.cfg` (`aspect=`, `filter=`,
  `scaler=cpu`, `usbpoll=native`; by hand: `inputmargin=2.5` to read the
  controls earlier before the vsync, `multicore=off` to draw on core 0
  alone).
- EEPROM contents saved in `cpspi/nvram`, diagnostics in `cpspi/LOG.TXT`,
  written after 15 s of play and then every minute (frame, render and draw
  times, how long before the vsync the controls were read, pad report to
  game latency, audio queue), benchmark and sampling profiler with an empty
  `cpspi/BENCH` file.

Not yet: save states, DIP switch editor, vertical games drawn upright (they
run, but sideways), Pi 4 kernel.

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
whatever the file names. Current MAME and FBNeo sets differ from the
database for some games (re-dumped or re-arranged ROMs): run
`python tools/check_roms.py <folder with the zips>` on the PC before copying
them, it names every missing ROM and the set a zip's contents do match.
No games are included.

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
