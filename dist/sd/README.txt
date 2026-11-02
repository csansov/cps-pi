neo-pi 0.7.0-rc1 - bare metal Neo Geo emulator for Raspberry Pi 4 / 400 and Pi 3
==============================================================================

SET-UP
  1. A micro SD card formatted FAT32 (with a normal partition table).
  2. Copy EVERYTHING in this folder to the root of the card.
  3. Put neogeo.zip (MAME/FBNeo BIOS set) and your games in .neo format
     (made with TerraOnion's NeoBuilder) in the neopi folder.
  4. Connect a pad and the TV (on a Pi 4: HDMI 0, next to the power
     socket), insert the card and switch on.

START-UP
  - The screen stays black for a moment, then the game list appears.
  - SINGLE-SLOT MODE: with only one game in the neopi folder, the list is
    skipped and the game starts straight away. GAME LIST in the in-game
    menu still shows the list.
  - Game list: up/down to choose, left/right to jump by 8, A or Start to
    play, D for the OPTIONS menu.
  - An empty file called neopi/BENCH (no extension) runs a benchmark on the
    selected game first: 30 seconds of the game's demo, then 60 seconds
    measured (about a minute and a half, do not touch anything). Results in
    neopi/BENCH.TXT and neopi/PROFILE.TXT. Delete BENCH to start normally.

SETTING UP THE CONTROLS
  The first time a pad model is connected, the CONTROLS screen opens by
  itself: press the button you want as A, then B, C, D, START and
  SELECT/COIN. After the last one the set-up is saved (SAVED). Directions
  are not set up: the d-pad, hat and left stick always work. A button
  already used is ignored. On a keyboard, Esc cancels. The set-up applies to
  every pad of the same model and is stored in neopi/pads.cfg.
  To redo it: OPTIONS (D in the list) > CONTROLS.

CONTROLS IN GAME
  d-pad or left stick     joystick
  A B C D                 Neo Geo buttons
  Start                   start
  Select (short press)    insert a coin (AES: Select button)
  Select held 1 second    in-game menu (the game pauses)
  Select + A              screenshot (neopi/shots/<game>_NNN.png)
  Select + B              lazy finger on/off (turbo: holding A, B, C or D
                          fires repeatedly; for each player, off at every
                          game start)
  Select + C              status line on/off
  Select + D              credits (B closes them)
  The second pad connected is player 2.

IN-GAME MENU (hold Select for 1 second; B resumes the game)
  SAVE STATE     saves the game (one save per game)
  LOAD STATE     loads it (available once something has been saved)
  RESET          restarts the game
  BIOS SETTINGS  (MVS only) presses the TEST button: the BIOS's own menu
                 appears (game and cabinet settings); its EXIT item
                 restarts the game with the new settings
  GAME LIST      back to the game list
  POWER OFF      3 second countdown (B cancels), saves everything and
                 switches picture and sound off: then the power can be
                 unplugged

OPTIONS MENU (D in the list; A or left/right changes, B goes back)
  SYSTEM         MVS (arcade) or AES (home console), from the next game
  REGION         EUROPE / USA / JAPAN / ASIA (BIOS region: language,
                 censorship), from the next game
  SCANLINES      OFF / LIGHT / MEDIUM / STRONG
  CONTROLS       sets up a pad (with several pads: press a button on the
                 one to set up)
  POWER OFF      as above
  Settings are stored in neopi/neopi.cfg.

STATUS LINE (Select + C), below the picture
  60 FPS   LOAD 62%   39 C   DLY 6.5   1080P 4X CPU+HVS
  LOAD is how much of each 1/60 s frame is used (green, yellow from 75%,
  red above 95%). 4X HVS = scaled by the Pi's hardware scaler,
  CPU+HVS = the CPU draws the scanlines at 2x and the scaler doubles it,
  CPU = all by the CPU. DLY is the frame delay in ms; with beam racing,
  RACE is the time in ms from reading the pads to the top of the picture
  being sent to the TV (see INPUT LAG).

INPUT LAG
  Each frame starts with the vertical blank, when games read the pads, and
  then draws the picture: the frame shown at the next vsync already answers
  the pads (up to 0.6.4 the answer came one frame later).
  Frame delay: the frame starts a few ms after the vsync, so the pads are
  read later. It is automatic: it starts at 0 and grows slowly while the
  game's frames stay light (up to 6 ms), and drops at once if they get
  heavier. LOG.TXT shows it every minute.
  Beam racing: the picture is not prepared in advance and shown
  at the next vsync, it is drawn line by line just ahead of the HDMI
  output, as a real Neo Geo draws it on a CRT: the pads are read about
  2-4 ms before the top of the picture is sent to the TV. At start-up
  neo-pi measures the Pi's video scaler for about one second (LOG.TXT:
  "raster" and "Beam racing" lines). The margins adapt to each game; if
  lines arrive late too often, that game goes back to the normal way
  (double buffer + frame delay) and LOG.TXT says so. The Pi 3 has less
  time to spare than the Pi 4, so heavier games may go back to the normal
  way more often there.
  Testing options in neopi/neopi.cfg (one per line):
    beamracing=off      normal way (double buffer + frame delay)
    framedelay=off      no frame delay
    framedelay=5        fixed delay in ms (1-12; too much = stutter)
    framephase=classic  the frame starts as up to 0.6.4 (one frame more)

PICTURE
  Always integer scaling (square pixels, never stretched) with black
  borders. 1080p: 1216x896 picture. 720p: 912x672 picture.
  Testing options in neopi/neopi.cfg (one per line):
    scaler=cpu       do not use the hardware scaler (comparison only)
    multicore=off    everything on one CPU core, as up to 0.6.3
    usbpoll=native   poll pads at the rate they ask for (usually every
                     8-10 ms) instead of every 1 ms
    resampler=speex  the previous, heavier audio resampler (comparison)

FILES ON THE CARD (neopi folder)
  LOG.TXT      diagnostics: model, HDMI, pads, audio, per-minute timings.
               Updated when the menu opens, when the game changes and at
               power off.
  BENCH.TXT    benchmark results
  PROFILE.TXT  benchmark: where the time goes (for the developer)
  shots/       screenshots (Select + A)
  saves/       save states (.st1 MVS, .sa1 AES), MVS memory (.nv), memory
               cards
  LAST.TXT     last game played (the list starts there)
  neopi.cfg    OPTIONS settings
  pads.cfg     controls for each pad model
  Always switch off with POWER OFF: it saves the memory, save data and
  LOG.TXT. For help, LOG.TXT and BENCH.TXT are all that is needed.

IF SOMETHING GOES WRONG
  The green LED blinks a number of times, repeatedly:
  2 = HDMI   3 = SD card   4 = BIOS   5 = no .neo game   7 = memory
  No picture at all: put a # in front of hdmi_group and hdmi_mode in
  config.txt.

neo-pi by Alessio Scanderebech. Licence: GPLv3 (see LICENSES.txt).
NEO GEO is a trademark of SNK. BIOS and games are not included.
