/*
 * cps-pi - C interface between the Circle kernel (C++) and the NJEMU CPS1
 * core glue (njemu_pi.c). kernel.cpp never includes NJEMU headers.
 */
#ifndef CORE_API_H
#define CORE_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- services provided by the kernel ---- */
uint64_t cpspi_ticks_us (void);		/* monotonic microseconds */
void cpspi_delay_us (unsigned us);

/* ---- the core's picture: BGR555 work buffer, the game inside it ---- */
#define CORE_WORK_W	512		/* pitch in pixels (NJEMU BUF_WIDTH) */
#define CORE_WORK_H	272
#define CORE_GAME_X	64
#define CORE_GAME_Y	16
#define CORE_GAME_W	384
#define CORE_GAME_H	224

/* ---- inputs: one bit per NJEMU input flag (cps1/inptport.h order) ---- */
#define CORE_IN_RIGHT	(1u << 0)
#define CORE_IN_LEFT	(1u << 1)
#define CORE_IN_DOWN	(1u << 2)
#define CORE_IN_UP	(1u << 3)
#define CORE_IN_B1	(1u << 4)
#define CORE_IN_B2	(1u << 5)
#define CORE_IN_B3	(1u << 6)
#define CORE_IN_B4	(1u << 7)
#define CORE_IN_B5	(1u << 8)
#define CORE_IN_B6	(1u << 9)
#define CORE_IN_COIN	(1u << 10)
#define CORE_IN_START	(1u << 11)
#define CORE_IN_DIAL_L	(1u << 12)
#define CORE_IN_DIAL_R	(1u << 13)
#define CORE_IN_SERVICE	(1u << 14)	/* service coin */
#define CORE_IN_TEST	(1u << 15)	/* test / service switch */
#define CORE_MAX_PLAYERS 4

#define CORE_AUDIO_RATE	48000

/* base_dir: "SD:/cpspi" (rominfo.cps1, nvram/, roms/) */
int core_init (const char *base_dir);
/* loads roms/<name>.zip; 0 = failure (see core_error()) */
int core_load (const char *name);
void core_unload (void);
void core_reset (void);
int core_loaded (void);
/* one emulated frame (set the inputs first) */
void core_frame (void);
/* picture of the last frame */
const uint16_t *core_video (void);
int core_flipped (void);		/* cocktail flip active */
int core_vertical (void);		/* vertical (rotated) game */
/* interleaved stereo at CORE_AUDIO_RATE produced by the last frame */
unsigned core_audio (const int16_t **samples);
void core_set_input (unsigned player, uint32_t flags);
/* called by the core at its vblank, inside core_frame: the picture and the
   sound of the frame are complete (core_video / core_audio) and the game
   has not yet read its input ports; the kernel shows the picture, waits for
   the right moment, samples the pads and calls core_set_input; only the
   interrupt handler's few lines remain before core_frame returns */
void core_set_input_callback (void (*callback) (void));
/* microseconds the core spent rendering the picture of the last frame */
unsigned core_render_us (void);
/* the frame's sound comes in four blocks (block 0-3, lines 64, 128, 192 and
   the vblank): interleaved stereo at CORE_AUDIO_RATE, handed over as soon
   as rendered, inside core_frame; block 3 is followed by the vblank callback */
void core_set_audio_callback (void (*callback) (const int16_t *samples, unsigned frames,
						 unsigned block));
unsigned core_players (void);
unsigned core_buttons (void);
int core_fatal (void);
const char *core_error (void);
/* DIP switches (3 banks, 8 bits each, 0xff = all off) */
void core_set_dips (unsigned a, unsigned b, unsigned c);
/* sound chip: 0 = YM2151 (+OKI), 1 = QSound */
int core_qsound (void);
/* 68000 clock of the loaded game in MHz (10, or 12 on the later boards) */
unsigned core_m68k_mhz (void);
const char *core_game_name (void);

#ifdef __cplusplus
}
#endif

#endif
