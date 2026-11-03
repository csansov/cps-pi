/*
 * cps-pi - one kernel, two cores. build.sh compiles the NJEMU CPS1 core and
 * the CPS2 core (each with its own copy of njemu_pi.c) and prefixes every
 * symbol they define with cps1_ / cps2_ (objcopy --redefine-syms), so both
 * link into the same kernel; this file is the core_api.h the kernel sees,
 * forwarding to the selected core.
 */
#include "core_api.h"

#define DECL(S) \
	int S##_core_init (const char *base_dir); \
	int S##_core_load (const char *name); \
	void S##_core_unload (void); \
	void S##_core_reset (void); \
	int S##_core_loaded (void); \
	void S##_core_frame (void); \
	const uint16_t *S##_core_video (void); \
	int S##_core_flipped (void); \
	int S##_core_vertical (void); \
	unsigned S##_core_audio (const int16_t **samples); \
	void S##_core_set_input (unsigned player, uint32_t flags); \
	void S##_core_set_input_callback (void (*callback) (void)); \
	unsigned S##_core_render_us (void); \
	void S##_core_set_audio_callback (void (*callback) (const int16_t *, unsigned, unsigned)); \
	unsigned S##_core_players (void); \
	unsigned S##_core_buttons (void); \
	int S##_core_fatal (void); \
	const char *S##_core_error (void); \
	void S##_core_set_dips (unsigned a, unsigned b, unsigned c); \
	int S##_core_qsound (void); \
	unsigned S##_core_m68k_mhz (void); \
	unsigned S##_core_cpu_percent (void); \
	void S##_core_set_cpu_percent (unsigned percent); \
	unsigned S##_core_cpu_percent_centre (void); \
	unsigned S##_core_cpu_percent_range (void); \
	const char *S##_core_game_name (void);

DECL (cps1)
DECL (cps2)

static unsigned s_sys = CORE_SYS_CPS1;

#define ACTIVE(name, ...) (s_sys == CORE_SYS_CPS2 ? cps2_##name (__VA_ARGS__) : cps1_##name (__VA_ARGS__))
#define ACTIVE0(name) (s_sys == CORE_SYS_CPS2 ? cps2_##name () : cps1_##name ())

unsigned core_systems (void) { return CORE_SYS_CPS1 | CORE_SYS_CPS2; }
unsigned core_system (void) { return s_sys; }

void core_select (unsigned system)
{
	if (system == CORE_SYS_CPS1 || system == CORE_SYS_CPS2)
	{
		s_sys = system;
	}
}

int core_init (const char *base_dir)
{
	int a = cps1_core_init (base_dir);
	int b = cps2_core_init (base_dir);
	return a && b;
}

int core_load (const char *name)			{ return ACTIVE (core_load, name); }
void core_unload (void)					{ cps1_core_unload (); cps2_core_unload (); }
void core_reset (void)					{ ACTIVE0 (core_reset); }
int core_loaded (void)					{ return ACTIVE0 (core_loaded); }
void core_frame (void)					{ ACTIVE0 (core_frame); }
const uint16_t *core_video (void)			{ return ACTIVE0 (core_video); }
int core_flipped (void)					{ return ACTIVE0 (core_flipped); }
int core_vertical (void)				{ return ACTIVE0 (core_vertical); }
unsigned core_audio (const int16_t **samples)		{ return ACTIVE (core_audio, samples); }
void core_set_input (unsigned player, uint32_t flags)	{ cps1_core_set_input (player, flags); cps2_core_set_input (player, flags); }
void core_set_input_callback (void (*callback) (void))	{ cps1_core_set_input_callback (callback); cps2_core_set_input_callback (callback); }
unsigned core_render_us (void)				{ return ACTIVE0 (core_render_us); }
void core_set_audio_callback (void (*callback) (const int16_t *samples, unsigned frames, unsigned block))
{
	cps1_core_set_audio_callback (callback);
	cps2_core_set_audio_callback (callback);
}
unsigned core_players (void)				{ return ACTIVE0 (core_players); }
unsigned core_buttons (void)				{ return ACTIVE0 (core_buttons); }
int core_fatal (void)					{ return ACTIVE0 (core_fatal); }
const char *core_error (void)				{ return ACTIVE0 (core_error); }
void core_set_dips (unsigned a, unsigned b, unsigned c)	{ cps1_core_set_dips (a, b, c); cps2_core_set_dips (a, b, c); }
int core_qsound (void)					{ return ACTIVE0 (core_qsound); }
unsigned core_m68k_mhz (void)				{ return ACTIVE0 (core_m68k_mhz); }
unsigned core_cpu_percent (void)			{ return ACTIVE0 (core_cpu_percent); }
void core_set_cpu_percent (unsigned percent)		{ ACTIVE (core_set_cpu_percent, percent); }
unsigned core_cpu_percent_centre (void)			{ return ACTIVE0 (core_cpu_percent_centre); }
unsigned core_cpu_percent_range (void)			{ return ACTIVE0 (core_cpu_percent_range); }
const char *core_game_name (void)			{ return ACTIVE0 (core_game_name); }
