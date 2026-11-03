# cps-pi

A bare-metal Capcom CPS1 and CPS2 arcade emulator for the Raspberry Pi 3.
Power on, and a few seconds later the game list is on the screen: no Linux,
no desktop, nothing to configure. Street Fighter II, Final Fight, Ghouls'n
Ghosts, Strider and the rest of the CPS1 catalogue; Super Street Fighter II
Turbo / X and the CPS2 catalogue, in the same list.

**cps-pi is a derivative of [neo-pi](https://github.com/alessioscand/neo-pi)**,
Alessio Scanderebech's bare-metal Neo Geo emulator, and keeps its idea: one
machine done well, with the lowest latency the hardware allows. Where neo-pi
runs the Geolith Neo Geo core, cps-pi runs the CPS1 and CPS2 cores of
[NJEMU](https://github.com/HonkeyKong/NJEMU-Libretro) (NJ's CPS1PSP and
CPS2PSP, as maintained by fjtrujy and HonkeyKong). The boot, the menus, the
pads, the video and the audio paths come from neo-pi, adapted.

Status: **0.10.0**. Tested on a Raspberry Pi 3 Model B at 1080p, and against
real CPS1 and CPS2 boards for game speed. Expect rough edges.

## What you get

- **The board's timing.** The picture refreshes at the CPS frame rate,
  59.637 Hz, and the HDMI mode runs at exactly that rate, so nothing is sped
  up, slowed down, dropped or repeated. The 68000 runs at 10 or 12 MHz as on
  each CPS1 board, 16 MHz on CPS2.
- **The board's game speed.** The CPS chips make the 68000 wait when it
  touches the graphics memory, and no emulator reproduces that, so emulated
  CPS games run faster than the boards (a known, open MAME issue since
  2008). cps-pi gives the 68000 a share of its clock to stand in for those
  waits, calibrated against real boards, and lets you adjust it while
  playing (see CPU SPEED below).
- **The board's input timing.** The controls are read at the emulated
  vblank, the moment the game's own interrupt handler reads them, about one
  millisecond before the display draws the next frame. What you press is
  on screen the frame after, as on the board, and there is no frame delay
  to tune: the display's own line counter tells the kernel where the beam
  is.
- **The picture as on the arcade monitor.** 4:3, with an integer vertical
  scale and the columns blended like a CRT beam (or sharp), or square pixels
  if you prefer; scanlines in three strengths. Your own bezel around it.
- **Sound close to the chips.** About 18 ms from the sound chips to the
  HDMI output, with no change of pitch and no gaps.
- **USB pads and keyboards**, polled every millisecond, set up once per pad
  model.
- **A log** on the card with everything the kernel measured, if you ever
  want to know.

## Setting up the card

1. Format a card as FAT32 and copy the contents of the release zip (or of
   `dist/sd` after building) to its root: the Raspberry Pi firmware,
   `config.txt`, `cmdline.txt`, `kernel8.img` and the `cpspi` folder.
2. Put your games in `cpspi/roms`: MAME-style zips (see ROM sets below).
   CPS1 and CPS2 games go in the same folder.
3. Plug in a pad, power on. The first time a pad model is seen, cps-pi asks
   which of its buttons are the six buttons, Start and Select, and remembers.

`config.txt` sets 1080p at the board's 59.637 Hz. If your display shows
nothing, put `#` in front of the four `hdmi_*` lines of that mode and remove
it from `hdmi_group=1` and `hdmi_mode=16`: standard 60 Hz, everything else
the same. The `[pi3]` section has a commented overclock: enable it only with
a heatsink.

## The menus

**Game list.** Up and down choose, button 1 starts, button 4 opens OPTIONS.
Under the selected game: its zip name and whether it is CPS1 or CPS2. The
last game played is remembered.

**OPTIONS** (from the list):

| Item | What it does |
| --- | --- |
| PICTURE | `4:3`: the arcade monitor's shape, columns blended like a CRT. `4:3 SHARP`: the same, nearest column. `PIXEL`: square pixels, integer scale. |
| SCANLINES | Off, light, medium or strong. Fine on a Pi 3. |
| OVERLAY | Shows or hides your bezel (`cpspi/overlay.png`, see below). `NONE` if there is no file. |
| CONTROLS | Set up a pad's buttons again. |
| POWER OFF | Shuts the Pi down cleanly. |

**In the game.** Hold Select for a second (or press Esc) for the menu:

| Item | What it does |
| --- | --- |
| RESET | Resets the board. |
| CPU SPEED | The 68000's share of its clock, as a number around 0: 0 is the calibrated board, negative slower, positive faster. Left and right move 1, buttons 1 and 3 move 5. Takes effect at once and is kept for that game. |
| SERVICE MENU | Presses the board's TEST switch: the game's own service menu, DIP switches (CPS1) or configuration (CPS2), tests. |
| GAME LIST | Back to the list. |
| POWER OFF | Shuts down. |

Also while playing: a short press of Select inserts a coin; Select + 1
takes a screenshot (`cpspi/shots`); Select + 2 shows a status line (FPS,
load, temperature); Select + 3 the credits.

## CPU SPEED, and why it exists

On a CPS board the graphics chips make the 68000 wait every time it reads
or writes the graphics memory and during their DMA. No emulator reproduces
those waits (MAME Testers 408, open since 2008), so the games run faster
than the boards. In most CPS1 games the effect is lag frames the board has
and the emulator does not; in Street Fighter II it is visible, Hyper
Fighting most. In Super Street Fighter II Turbo / X it is the whole game
speed: that game paces itself with a tight CPU loop instead of the frame,
so it runs exactly as fast as the CPU gets through memory, and MAME's usual
74% is still too fast.

cps-pi stands in for the waits by giving the 68000 a share of its clock:

- CPS1: 60% (SF2 World Warrior, Champion Edition and Hyper Fighting timed
  against a real board), adjustable from -10 to +10 (50% to 70%).
- CPS2: 56% (Super Street Fighter II X at speed 3 timed against a real
  board), adjustable from -20 to +20 (36% to 76%).

To calibrate a game against a board you own, time the in-game round timer
from 99 to 0 in the same situation on both, and move CPU SPEED until the
times match (for Super Turbo the speed is proportional to the number, so
one measurement gives it). The value is kept per game in `cpspi/cpspi.cfg`
(`cpu.<set>=NN`, in percent; `cpu=NN` applies to every game). Music tempo
is not affected: it comes from the sound chip's own timers.

## Your own bezel

Put a PNG the size of the display (1920x1080; 8 bit RGB or RGBA, not
interlaced) at `cpspi/overlay.png` and it is shown around the game picture.
At 1080p the picture occupies 1194x896 pixels with its top left corner at
(360, 92); whatever the PNG has there is never seen. No bezel is included:
make one with the artwork you like. OVERLAY in the menu, or `overlay=off`
in `cpspi/cpspi.cfg`, leaves black.

## ROM sets

NJEMU identifies every ROM by its CRC, in the sets listed in
`cpspi/rominfo.cps1` and `cpspi/rominfo.cps2` (MAME 0.152 era). The zip
must carry the set name of that era: for example the Japan 940223 Super
Street Fighter II X that MAME now calls ssf2xjr1 is `ssf2xj.zip` here. A
clone needs its parent zip in the same folder, unless it is a merged set
with the parent's ROMs inside. Only the CRCs matter, so a newer set whose
ROM contents are the same loads too, whatever the file names; some current
MAME and FBNeo sets differ (re-dumped or re-arranged ROMs). Run
`python tools/check_roms.py <folder with the zips> [rominfo file]` on the
PC before copying them: it names every missing ROM and the set a zip's
contents do match. No games are included.

## Controls

Pads with a known layout (Xbox and compatible): X Y RB = buttons 1 2 3
(punches), A B RT = buttons 4 5 6 (kicks), Start, Back = coin. Generic HID
pads and arcade encoders: buttons 1-6, button 9 = Select, 10 = Start. The
first time a pad model is seen, cps-pi asks for its eight buttons and saves
the answer in `cpspi/pads.cfg`. Keyboard: arrows, A S D and Z X C, Enter,
5 or Space, Esc.

## cpspi.cfg

Written by the menus; these keys can also be set by hand:

| Key | Meaning |
| --- | --- |
| `aspect=4:3` / `4:3sharp` / `square` | the PICTURE setting |
| `filter=NONE` / `SCANLINES_LIGHT` / `SCANLINES` / `SCANLINES_STRONG` | the SCANLINES setting |
| `overlay=off` | no bezel |
| `cpu=NN`, `cpu.<set>=NN` | the 68000's share of its clock, in percent, for every game or for one set |
| `inputmargin=2.5` | read the controls this many milliseconds before the frame instead of 1 (if the log shows missed frames) |
| `multicore=off` | draw the picture on core 0 alone and write nothing in the background |
| `scaler=cpu` | no hardware scaler |
| `usbpoll=native` | poll the pads at the interval they ask for instead of 1 ms |

## How it works, briefly

- One kernel, two cores: NJEMU's CPS1 and CPS2 cores are compiled
  separately, every symbol of each is prefixed and `src/core_dispatch.c`
  forwards the core interface to the one the chosen game needs.
- NJEMU runs in its frontend-driven configuration (one synchronous frame
  per call, software rendering), unmodified: `build.sh` copies its source,
  drops in cps-pi's `libretro/platform.h` and `lifecycle.inc` and applies
  three build-time patches (the vblank callback order, the board's timing
  and 68000 clock, the sound in four blocks per frame).
- The core renders the frame and raises the vblank interrupt; between the
  two, the kernel shows the picture, waits until the display is a
  millisecond from its next frame (the HVS line counter), reads the pads
  and only then lets the game's interrupt handler run.
- The sound is produced in four blocks per frame and the emulation is paced
  through the real frame so each block reaches the HDMI output shortly
  after it is made; the audio queue adapts to the measured gaps.
- The four cores write the picture to the framebuffer, a quarter each; core
  3 also writes the log in the background. The framebuffer is mapped as
  normal non-cacheable memory (a Circle patch), ten times faster to write
  than Circle's default.
- The frontend (kernel, video, UI, input, audio) is neo-pi's, on
  [Circle](https://github.com/rsta2/circle); the zips and screenshots go
  through [miniz](https://github.com/richgel999/miniz); a small C library
  shim covers what NJEMU needs.

`cpspi/LOG.TXT` holds the diagnostics: frame, render and draw times, how
long before the frame the controls were read, pad report to game latency,
sound block pacing, audio queue, every minute. An empty `cpspi/BENCH` file
runs a benchmark and a sampling profiler at boot.

Not yet: save states, a DIP switch editor, vertical games drawn upright
(they run, but sideways), the QSound DSP emulation (NJEMU's sample-based
QSound is used), a Pi 4 kernel.

## Building

Host: Windows (Git Bash) or Linux. Tools: the xPack
[aarch64-none-elf-gcc](https://github.com/xpack-dev-tools/aarch64-none-elf-gcc-xpack)
14.2.1 toolchain (plus xPack Windows Build Tools for GNU make on Windows),
Circle at commit 7a81e1b with the three patches in `patches/`, Python 3,
and the NJEMU source. `./build.sh 3` produces `build/rpi3/kernel8.img` with
both cores (`./build.sh 3 cps1` or `cps2` a single-core kernel). `BUILD.md`
has the step-by-step instructions.

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
- The speed calibration owes to the owners of real boards who timed them.

## Licence

GPLv3, see `LICENSE`. neo-pi, NJEMU and Circle are GPLv3; miniz is MIT; the
Raspberry Pi firmware in `dist/sd` is distributed under `LICENCE.broadcom`.
CPS, Capcom and the game names are trademarks of their owners. No games or
other copyrighted ROM data are included.
