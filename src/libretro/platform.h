/*
 * cps-pi: NJEMU platform header. The core is built with -DLIBRETRO, so it
 * takes the frontend-driven path (one synchronous frame per call, no threads,
 * no GUI); this file replaces src/libretro/platform.h of the libretro port.
 */
#ifndef NJEMU_LIBRETRO_PLATFORM_H
#define NJEMU_LIBRETRO_PLATFORM_H
#include <stdint.h>
#define SCR_WIDTH 512
#define SCR_HEIGHT 272
#define BUF_WIDTH 512
#define REFRESH_RATE FPS
void lr_log(int level, const char *fmt, ...);
const char *lr_content_archive(void);
const char *lr_system_path(const char *relative);
const char *lr_save_path(const char *relative);
void lr_input_flags(uint8_t *flags, unsigned player);
int lr_machine_load(void);
void lr_machine_frame(void);
void lr_machine_reset(void);
void lr_machine_unload(void);
void lr_audio_frame(void);
void lr_audio_reset(void);
void lr_options(void);
extern unsigned lr_width, lr_height;
extern uint64_t lr_frame_number;
extern unsigned lr_audio_rate;
extern double lr_fps;
extern int lr_initialized_machine;
#endif
