/*
 * cps-pi - glue between the NJEMU CPS1 core and the Circle kernel
 *
 * The core is built as its frontend-driven "libretro" variant (one
 * synchronous frame per call, no threads, no GUI); this file stands in for
 * src/libretro/{core,runtime,video,audio,input,ui}.c of that port:
 *   - the runtime globals and stubs the core expects,
 *   - the software video driver (every sprite/tile goes to a BGR555 work
 *     buffer, which the kernel scales to the HDMI framebuffer),
 *   - one frame of sound per emulated frame, resampled to 48 kHz,
 *   - the inputs, set by the kernel before each frame,
 *   - the core_* API of core_api.h used by kernel.cpp.
 */
#include "emumain.h"
#include "core_api.h"
#include "cpspi.h"
#include <stdlib.h>
#include <stdarg.h>

/******************************************************************************
	Runtime globals (libretro/runtime.c)
******************************************************************************/

char game_name[16], parent_name[16], game_dir[PATH_MAX];
char launchDir[PATH_MAX], screenshotDir[PATH_MAX];
bool systembuttons_available;
int option_showfps, option_speedlimit, option_autoframeskip, option_frameskip, option_vsync, option_stretch;
int option_sound_enable = 1, option_samplerate = 2, option_sound_volume = 10;
int machine_driver_type, machine_init_type, machine_input_type, machine_screen_type, machine_sound_type;
uint32_t frames_displayed;
int fatal_error, platform_cpuclock;
volatile int Loop = LOOP_BROWSER, Sleep;
void *platform_data, *power_data, *ticker_data;
uint8_t ALIGN16_DATA gulist[GULIST_SIZE];
uint64_t lr_frame_number;
unsigned lr_width = 384, lr_height = 224;
double lr_fps = FPS;

static char s_base[PATH_MAX];		/* "SD:/cpspi" */
static char s_content[PATH_MAX];	/* the game's zip */
static char s_error[256];
static int s_loaded;
static unsigned s_dips[3] = {0xff, 0xff, 0xff};

void autoframeskip_reset(void) {}
uint8_t skip_this_frame(void) { return 0; }
void update_screen(void) {}
void waitVBlank(void) {}
void video_wait_vsync(void) {}
void save_snapshot(void) {}
void show_fatal_error(void) {}
void emu_main(void) {}

void fatalerror(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s_error, sizeof(s_error), fmt, ap);
	va_end(ap);
	cpspi_log("core error: %s", s_error);
	fatal_error = 1;
	Loop = LOOP_BROWSER;
}

void lr_log(int level, const char *fmt, ...)
{
	char message[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	size_t n = strlen(message);
	while (n > 0 && (message[n-1] == '\n' || message[n-1] == '\r')) message[--n] = 0;
	if (n) cpspi_log("core: %s", message);
	if (level >= 3)
	{
		strncpy(s_error, message, sizeof(s_error) - 1);
		s_error[sizeof(s_error) - 1] = 0;
	}
}

static int32_t clock_max(void *p) { (void)p; return 333; }
static void set_clock(void *p, int32_t value) { (void)p; (void)value; }
static void set_lowest(void *p) { (void)p; }
static void *power_init(void) { return &power_data; }
static void power_free(void *p) { (void)p; }
static int32_t battery(void *p) { (void)p; return 100; }
static bool charging(void *p) { (void)p; return true; }
static power_driver_t power = {"cps-pi", power_init, power_free, battery, charging,
			       set_clock, set_lowest, clock_max};
power_driver_t *power_drivers[] = {&power, NULL};

static uint64_t ticks(void *p) { (void)p; return cpspi_ticks_us(); }
static void *ticker_init(void) { return &ticker_data; }
static void ticker_free(void *p) { (void)p; }
static ticker_driver_t ticker = {"cps-pi", ticker_init, ticker_free, ticks};
ticker_driver_t *ticker_drivers[] = {&ticker, NULL};

/******************************************************************************
	Paths and options (libretro/core.c)
******************************************************************************/

const char *lr_content_archive(void) { return s_content; }

const char *lr_system_path(const char *relative)
{
	static char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", s_base, relative);
	return path;
}

const char *lr_save_path(const char *relative)
{
	static char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", s_base, relative);
	return path;
}

void lr_options(void)
{
	option_vsync = option_speedlimit = option_autoframeskip = option_frameskip = option_showfps = 0;
	option_sound_enable = 1; option_samplerate = 2; option_sound_volume = 10;
#if EMU_SYSTEM == CPS1
	cps1_dipswitch[0] = s_dips[0] & 255;
	cps1_dipswitch[1] = s_dips[1] & 255;
	cps1_dipswitch[2] = s_dips[2] & 255;
	cps_raster_enable = 1;		/* line scroll (CPS2 has it always on) */
#endif
	cps_rotate_screen = 0;
}

/******************************************************************************
	UI stubs (libretro/ui.c): the kernel draws its own menus
******************************************************************************/

UI_PALETTE ui_palette[UI_PAL_MAX] =
{
	{ 255, 255, 255 }, { 255, 255, 255 }, { 180, 180, 180 }, { 255, 255,  64 },
	{ 255,  64,  64 }, {  48,  48,  48 }, {   0,   0, 160 }, {   0,   0,   0 },
	{  40,  40,  40 }, { 120, 120, 120 }
};

int cheat_num = 0;
gamecheat_t *gamecheat[MAX_CHEATS];

void msg_printf(const char *text, ...)
{
	char message[512];
	va_list ap;
	va_start(ap, text);
	vsnprintf(message, sizeof(message), text, ap);
	va_end(ap);
	size_t n = strlen(message);
	while (n > 0 && (message[n-1] == '\n' || message[n-1] == '\r')) message[--n] = 0;
	if (n) cpspi_log("core: %s", message);
}
void show_progress(const char *text) { if (text && *text) cpspi_log("core: %s", text); }
void update_progress(void) {}
void showmenu(void) {}
int draw_volume_status(int draw) { (void)draw; return 0; }
int draw_battery_status(int draw) { (void)draw; return 0; }
void msg_screen_clear(void) {}
void show_exit_screen(void) {}
void load_background(int number) { (void)number; }
int ui_show_popup(int draw) { (void)draw; return 0; }
void file_browser(void) { Loop = LOOP_BROWSER; }
void small_font_print(int sx, int sy, const char *s, int bg) { (void)sx; (void)sy; (void)s; (void)bg; }
void small_font_printf(int x, int y, const char *text, ...) { (void)x; (void)y; (void)text; }
void uifont_print_center(int sy, int r, int g, int b, const char *s) { (void)sy; (void)r; (void)g; (void)b; (void)s; }
void uifont_print_shadow(int sx, int sy, int r, int g, int b, const char *s) { (void)sx; (void)sy; (void)r; (void)g; (void)b; (void)s; }
void textfont_print(int sx, int sy, int r, int g, int b, const char *s, int flag) { (void)sx; (void)sy; (void)r; (void)g; (void)b; (void)s; (void)flag; }
int uifont_get_string_width(const char *s) { (void)s; return 1; }
void uifont_print_shadow_center(int sy, int r, int g, int b, const char *s) { (void)sy; (void)r; (void)g; (void)b; (void)s; }
void uifont_print(int sx, int sy, int r, int g, int b, const char *s) { (void)sx; (void)sy; (void)r; (void)g; (void)b; (void)s; }
void small_icon_shadow(int sx, int sy, int r, int g, int b, int no) { (void)sx; (void)sy; (void)r; (void)g; (void)b; (void)no; }
void small_icon(int sx, int sy, int r, int g, int b, int no) { (void)sx; (void)sy; (void)r; (void)g; (void)b; (void)no; }
void show_background(void) {}
void boxfill_alpha(int sx, int sy, int ex, int ey, int r, int g, int b, int alpha) { (void)sx; (void)sy; (void)ex; (void)ey; (void)r; (void)g; (void)b; (void)alpha; }
void ui_popup_reset(void) {}
void draw_dialog(int sx, int sy, int ex, int ey) { (void)sx; (void)sy; (void)ex; (void)ey; }
int save_png(const char *path) { (void)path; return 0; }
int load_png(const char *name, int number) { (void)name; (void)number; return 0; }
void msg_screen_init(int wallpaper, int icon, const char *title) { (void)wallpaper; (void)icon; (void)title; }
void draw_scrollbar(int sx, int sy, int ex, int ey, int disp_lines, int total_lines, int current_line) { (void)sx; (void)sy; (void)ex; (void)ey; (void)disp_lines; (void)total_lines; (void)current_line; }
void ui_popup(const char *text, ...) { (void)text; }
int help(int number) { (void)number; return 0; }
void save_gamecfg(const char *name) { (void)name; }
void ui_init(void) {}
void load_gamecfg(const char *name) { (void)name; }
void delete_files(const char *dirname, const char *pattern) { (void)dirname; (void)pattern; }

/******************************************************************************
	Video driver (libretro/video.c): CPU implementation of every operation
	used by the sprite renderer. All draws share one BGR5551 work buffer.
******************************************************************************/

typedef struct {
	uint16_t work[BUF_WIDTH * SCR_HEIGHT];
	uint16_t draw[BUF_WIDTH * SCR_HEIGHT];
	uint16_t depth[BUF_WIDTH * SCR_HEIGHT];
	uint8_t *texture[8];
	layer_texture_info_t info[8];
	unsigned count;
	int left, top, right, bottom, depth_test;
} lr_video;
void *video_data, *show_frame, *draw_frame, *work_frame;
int video_mode;
RECT full_rect = {0, 0, BUF_WIDTH, SCR_HEIGHT};

static void release(void *data)
{
	lr_video *v = data;
	if (!v) return;
	for (unsigned i = 0; i < v->count; ++i) free(v->texture[i]);
	free(v);
	show_frame = draw_frame = work_frame = NULL;
}
static void *init(layer_texture_info_t *layers, uint8_t count, clut_info_t *clut)
{
	lr_video *v = calloc(1, sizeof(*v));
	(void)clut;
	if (!v || count > 8) { free(v); return NULL; }
	v->count = count;
	for (unsigned i = 0; i < count; ++i) {
		v->info[i] = layers[i];
		v->texture[i] = calloc(layers[i].width * layers[i].height, layers[i].bytes_per_pixel);
		if (!v->texture[i]) { release(v); return NULL; }
	}
	work_frame = v->work; draw_frame = show_frame = v->draw;
	v->right = BUF_WIDTH; v->bottom = SCR_HEIGHT;
	return v;
}
static void noop(void *data) { (void)data; }
static void flip(void *data, bool sync) { (void)data; (void)sync; }
static void *addr(void *data, void *frame, int x, int y) { (void)data; return (uint16_t *)frame + y * BUF_WIDTH + x; }
static void *work(void *data) { return ((lr_video *)data)->work; }
static void *texture(void *data, uint8_t index) { lr_video *v = data; return index < v->count ? v->texture[index] : NULL; }
static void scissor(void *data, uint16_t l, uint16_t t, uint16_t r, uint16_t b)
{
	lr_video *v = data;
	v->left = l < BUF_WIDTH ? l : BUF_WIDTH;
	v->right = r < BUF_WIDTH ? r : BUF_WIDTH;
	v->top = t < SCR_HEIGHT ? t : SCR_HEIGHT;
	v->bottom = b < SCR_HEIGHT ? b : SCR_HEIGHT;
}
static void clear(void *data) { lr_video *v = data; memset(v->work, 0, sizeof(v->work)); memset(v->draw, 0, sizeof(v->draw)); }
static void clear_frame(void *data, int index)
{
	lr_video *v = data;
	memset(index == COMMON_GRAPHIC_OBJECTS_SCREEN_BITMAP ? v->work : v->draw, 0, sizeof(v->draw));
}
static void fill(void *data, void *frame, uint32_t color)
{
	(void)data;
	uint16_t c = MAKECOL15(GETR32(color), GETG32(color), GETB32(color));
	for (unsigned i = 0; i < BUF_WIDTH * SCR_HEIGHT; ++i) ((uint16_t *)frame)[i] = c;
}
static void start(void *data, uint32_t color)
{
	lr_video *v = data;
	fill(v, v->work, color);
	v->depth_test = 0;
}
static void copy_mode(void *data, void *src, void *dst, RECT *sr, RECT *dr, int mode)
{
	(void)data;
	int sw = sr->right - sr->left, sh = sr->bottom - sr->top;
	int dw = dr->right - dr->left, dh = dr->bottom - dr->top;
	if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
	for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x) {
		int dx = dr->left + x, dy = dr->top + y;
		int sx = x * sw / dw, sy = y * sh / dh;
		if (mode == 1) { sx = sw - 1 - sx; sy = sh - 1 - sy; }
		if (mode == 2) { sx = sw - 1 - y * sw / dh; sy = x * sh / dw; }
		sx += sr->left; sy += sr->top;
		if (dx >= 0 && dx < BUF_WIDTH && dy >= 0 && dy < SCR_HEIGHT && sx >= 0 && sx < BUF_WIDTH && sy >= 0 && sy < SCR_HEIGHT)
			((uint16_t *)dst)[dy * BUF_WIDTH + dx] = ((uint16_t *)src)[sy * BUF_WIDTH + sx];
	}
}
static void copy(void *d, void *s, void *t, RECT *sr, RECT *dr) { copy_mode(d,s,t,sr,dr,0); }
static void copy_flip(void *d, void *s, void *t, RECT *sr, RECT *dr) { copy_mode(d,s,t,sr,dr,1); }
static void copy_rotate(void *d, void *s, void *t, RECT *sr, RECT *dr) { copy_mode(d,s,t,sr,dr,2); }
static void transfer(void *d, RECT *s, RECT *t) { lr_video *v = d; copy(d,v->work,v->draw,s,t); }
static void draw_texture(void *d, uint32_t sf, uint32_t df, void *s, void *t, RECT *sr, RECT *dr) { (void)sf; (void)df; copy(d,s,t,sr,dr); }
static void *native(void *data, int index)
{
	lr_video *v = data;
	if (index == COMMON_GRAPHIC_OBJECTS_SCREEN_BITMAP) return v->work;
	if (index == COMMON_GRAPHIC_OBJECTS_INITIAL_TEXTURE_LAYER) return v->texture[0];
	if (index == COMMON_GRAPHIC_OBJECTS_GLOBAL_CONTEXT) return v;
	return v->draw;
}
static void upload(void *data, uint8_t i) { (void)data; (void)i; }
static void upload_clut(void *data, uint16_t *p, uint8_t b) { (void)data; (void)p; (void)b; }
/* Texel for destination pixel p of a quad spanning [p0, p0+len) with texels
 * [t0, t1): samples at the pixel centre, t0 + (2*(p-p0)+1)*(t1-t0) / (2*len),
 * truncated toward zero. Reversed texels describe flipped tiles. */
static int texel(int p, int p0, int len, int t0, int t1)
{
	if (len <= 4096 && (unsigned)(p - p0) < 4096u)
		return (t0 * 2 * len + (2 * (p - p0) + 1) * (t1 - t0)) / (2 * len);
	int64_t d = 2 * (int64_t)len;
	return (int)(((int64_t)t0 * d + (2 * (int64_t)(p - p0) + 1) * (t1 - t0)) / d);
}
static void blit(void *data, uint8_t index, void *palette, uint8_t bank, uint32_t count, void *vertices)
{
	lr_video *v = data;
	struct Vertex *vs = vertices;
	uint16_t *clut = palette;
	int ucol[BUF_WIDTH]; /* texel column per destination column, -1 if outside */
	(void)bank; /* palette already points at the selected bank and subpalette */
	if (index >= v->count) return;
	layer_texture_info_t info = v->info[index];
	const int indexed = info.bytes_per_pixel == 1;
	if (indexed && !clut) return; /* every texel would be transparent */
	const uint8_t *tex8 = v->texture[index];
	const uint16_t *tex16 = (const uint16_t *)v->texture[index];
	const int tw = (int)info.width, th = (int)info.height;
	for (unsigned i = 0; i + 1 < count; i += 2) {
		struct Vertex a = vs[i], b = vs[i+1];
		int w = b.x - a.x, h = b.y - a.y;
		if (w <= 0 || h <= 0) continue;
		int l = a.x > v->left ? a.x : v->left;
		int t = a.y > v->top ? a.y : v->top;
		int r = b.x < v->right ? b.x : v->right;
		int bot = b.y < v->bottom ? b.y : v->bottom;
		if (l >= r || t >= bot) continue;
		uint16_t z = (uint16_t)a.z;

		/* Fast path: the quad is drawn 1:1 (every CPS1 tile and object is;
		 * mirrored ones have reversed texels), so texel() reduces to
		 * t0 + (p - p0), or t0 - 1 - (p - p0) when mirrored: no per-pixel
		 * column table, no divisions, the texel pointer just steps */
		int du = (int)b.u - (int)a.u, dv = (int)b.v - (int)a.v;
		if ((du == w || du == -w) && (dv == h || dv == -h)) {
			const int ustep = du > 0 ? 1 : -1, vstep = dv > 0 ? 1 : -1;
			const int u0 = du > 0 ? (int)a.u + (l - a.x) : (int)a.u - 1 - (l - a.x);
			const int v0 = dv > 0 ? (int)a.v + (t - a.y) : (int)a.v - 1 - (t - a.y);
			const int u1 = u0 + ustep * (r - l - 1), v1 = v0 + vstep * (bot - t - 1);
			if (u0 >= 0 && u0 < tw && u1 >= 0 && u1 < tw && v0 >= 0 && v0 < th && v1 >= 0 && v1 < th) {
				const int n = r - l;
				int tv = v0;
				for (int y = t; y < bot; ++y, tv += vstep) {
					const size_t row = (size_t)tv * (size_t)tw;
					uint16_t *dst = v->work + y * BUF_WIDTH + l, *dep = v->depth + y * BUF_WIDTH + l;
					if (indexed) {
						const uint8_t *src = tex8 + row + u0;
						if (v->depth_test) {
							for (int x = 0; x < n; ++x, src += ustep) {
								uint16_t c = clut[*src];
								if (c & 0x8000) continue;
								if (z < dep[x]) continue;
								dep[x] = z;
								dst[x] = c;
							}
						} else {
							for (int x = 0; x < n; ++x, src += ustep) {
								uint16_t c = clut[*src];
								if (!(c & 0x8000)) dst[x] = c;
							}
						}
					} else {
						const uint16_t *src = tex16 + row + u0;
						if (v->depth_test) {
							for (int x = 0; x < n; ++x, src += ustep) {
								uint16_t c = *src;
								if (c & 0x8000) continue;
								if (z < dep[x]) continue;
								dep[x] = z;
								dst[x] = c;
							}
						} else {
							for (int x = 0; x < n; ++x, src += ustep) {
								uint16_t c = *src;
								if (!(c & 0x8000)) dst[x] = c;
							}
						}
					}
				}
				continue;
			}
		}

		for (int x = l; x < r; ++x) {
			int u = texel(x, a.x, w, a.u, b.u);
			ucol[x - l] = (u < 0 || (size_t)u >= info.width) ? -1 : u;
		}
		for (int y = t; y < bot; ++y) {
			int tv = texel(y, a.y, h, a.v, b.v);
			if (tv < 0 || (size_t)tv >= info.height) continue;
			size_t row = (size_t)tv * info.width;
			uint16_t *dst = v->work + y * BUF_WIDTH, *dep = v->depth + y * BUF_WIDTH;
			for (int x = l; x < r; ++x) {
				int u = ucol[x - l];
				if (u < 0) continue;
				uint16_t c = indexed ? clut[tex8[row + u]] : tex16[row + u];
				if (c & 0x8000) continue;
				if (v->depth_test) {
					if (z < dep[x]) continue;
					dep[x] = z;
				}
				dst[x] = c;
			}
		}
	}
}
static void points(void *data, uint32_t count, void *vertices)
{
	lr_video *v = data;
	struct PointVertex *p = vertices;
	for (unsigned i = 0; i < count; ++i)
		if (p[i].x >= v->left && p[i].x < v->right && p[i].y >= v->top && p[i].y < v->bottom)
			v->work[p[i].y * BUF_WIDTH + p[i].x] = p[i].color;
}
static void flush(void *data, void *addr, size_t size) { (void)data; (void)addr; (void)size; }
static void depth_on(void *data) { ((lr_video *)data)->depth_test = 1; }
static void depth_off(void *data) { ((lr_video *)data)->depth_test = 0; }
static void depth_clear(void *data) { lr_video *v = data; memset(v->depth, 0, sizeof(v->depth)); }
static void color_clear(void *data) { lr_video *v = data; memset(v->work, 0, sizeof(v->work)); }
static video_driver_t cpu_video = {
	"cps-pi", init, release, noop, flip, addr, work, texture, scissor,
	clear, clear_frame, fill, start, transfer, copy, copy_flip, copy_rotate,
	draw_texture, native, upload, upload_clut, blit, points, flush,
	depth_on, depth_off, depth_clear, color_clear
};
video_driver_t *video_drivers[] = {&cpu_video, NULL};

/******************************************************************************
	Sound (libretro/audio.c): one frame of chip output per emulated frame,
	brought to 48 kHz by linear interpolation (position carried across frames)
******************************************************************************/

static struct sound_t info;
struct sound_t *sound = &info;
unsigned lr_audio_rate = 44100;
static int s_sound_enabled;
static double s_remainder;
static int16_t s_chip[SOUND_BUFFER_SIZE];
static int16_t s_out[4096];
static unsigned s_out_frames;
static uint64_t s_res_pos;		/* 32.32, 0 = last sample of the previous frame */
static int16_t s_res_prev[2];

void lr_sound_update(int16_t *dst, unsigned frames);

void sound_thread_init(void) { memset(&info, 0, sizeof(info)); s_sound_enabled = 1; s_remainder = 0; }
void sound_thread_exit(void) { s_sound_enabled = 0; }
int sound_thread_start(void) { lr_audio_rate = sound->frequency; s_sound_enabled = 1; s_remainder = 0; return 1; }
void sound_thread_stop(void) { s_sound_enabled = 0; }
void sound_thread_set_volume(void) {}
void sound_thread_enable(int on) { s_sound_enabled = on; }
void lr_audio_reset(void) { s_remainder = 0; s_res_pos = 0; s_res_prev[0] = s_res_prev[1] = 0; s_out_frames = 0; }

/* The frame's sound in four blocks (lines 64, 128, 192 and the vblank,
   patches/njemu-sound-blocks.py): each renders a quarter of the frame's
   samples from the chips' state at that point, resamples to 48 kHz and hands
   the block to the kernel at once, which queues it and paces the emulation */
static void (*s_audio_sink)(const int16_t *samples, unsigned frames, unsigned block);
static unsigned s_audio_count;		/* frames produced since core_audio() */

static void lr_audio_block(unsigned block)
{
	s_remainder += (double)lr_audio_rate / lr_fps / 4.0;
	unsigned frames = (unsigned)s_remainder;
	s_remainder -= frames;
	if (frames * 2 > SOUND_BUFFER_SIZE) { fatalerror("Audio buffer overflow"); return; }
	sound->samples = frames;
	lr_sound_update(s_chip, frames);
	if (!s_sound_enabled || !option_sound_enable) memset(s_chip, 0, frames * 2 * sizeof(int16_t));

	/* resample frames @ lr_audio_rate -> s_out @ 48 kHz */
	const uint64_t step = ((uint64_t)lr_audio_rate << 32) / CORE_AUDIO_RATE;
	const unsigned max_out = sizeof(s_out) / sizeof(s_out[0]) / 2;
	uint64_t pos = s_res_pos;
	unsigned out = 0;
	while ((unsigned)(pos >> 32) < frames && out < max_out)
	{
		unsigned i = (unsigned)(pos >> 32);
		int f = (int)((pos >> 17) & 0x7FFF);
		int l0, r0;
		if (i == 0) { l0 = s_res_prev[0]; r0 = s_res_prev[1]; }
		else { l0 = s_chip[2*(i-1)]; r0 = s_chip[2*(i-1)+1]; }
		int l1 = s_chip[2*i], r1 = s_chip[2*i+1];
		s_out[2*out]   = (int16_t)(l0 + (((l1 - l0) * f) >> 15));
		s_out[2*out+1] = (int16_t)(r0 + (((r1 - r0) * f) >> 15));
		out++;
		pos += step;
	}
	if (frames > 0)
	{
		s_res_prev[0] = s_chip[2*(frames-1)];
		s_res_prev[1] = s_chip[2*(frames-1)+1];
		pos -= (uint64_t)frames << 32;
	}
	s_res_pos = pos;
	s_out_frames = out;
	s_audio_count += out;
	if (s_audio_sink) s_audio_sink(s_out, out, block);
}

void lr_audio_frame(void)
{
	for (unsigned b = 0; b < 4; b++) lr_audio_block(b);
}

/* timer callback at lines 64, 128 and 192 (param 0-2) */
void lr_sound_block(int param)
{
	lr_audio_block((unsigned)param);
	if (param < 2) timer_set(SOUND_BLOCK, USECS_PER_SCANLINE * 64, param + 1, lr_sound_block);
}

void core_set_audio_callback(void (*callback)(const int16_t *samples, unsigned frames, unsigned block))
{
	s_audio_sink = callback;
}

/******************************************************************************
	Inputs (libretro/input.c): the kernel sets one bitmask per player
******************************************************************************/

static uint32_t s_input[CORE_MAX_PLAYERS];
static void (*s_input_callback)(void);

void lr_input_flags(uint8_t *f, unsigned player)
{
	memset(f, 0, MAX_INPUTS);
	uint32_t v = player < CORE_MAX_PLAYERS ? s_input[player] : 0;
#if EMU_SYSTEM == CPS2
	/* CPS2 numbers its inputs differently after the start button: the
	   service coin is 15 and the test switch 16 (P2_START, the dials in between) */
	for (unsigned i = 0; i < 12; i++) f[i] = (v >> i) & 1;
	f[SERV_COIN] = (v >> 14) & 1;
	f[SERV_SWITCH] = (v >> 15) & 1;
#else
	for (unsigned i = 0; i < 16; i++) f[i] = (v >> i) & 1;
#endif
}

/* At the emulated vblank (patches/njemu-vblank-input.py): the picture of the
   frame has just been rendered and the interrupt whose handler reads the
   input ports is about to be raised. The frame's sound is produced here too,
   so that the kernel can show the picture, queue the sound, wait until just
   before the display's vsync and sample the pads then; the ports are latched
   again with that sample, a frame fresher than the latch at the start of the
   frame (and as late as the board reads them). */
void lr_vblank_input(void)
{
	lr_audio_block(3);		/* the last quarter of the frame's sound */
	if (s_input_callback) s_input_callback();
	update_inputport();
}

/* time spent rendering the picture (cps1_screenrefresh + blit_finish) */
static uint64_t s_render_t0;
static unsigned s_render_us;
void lr_render_begin(void) { s_render_t0 = cpspi_ticks_us(); }
void lr_render_end(void) { s_render_us = (unsigned)(cpspi_ticks_us() - s_render_t0); }
unsigned core_render_us(void) { return s_render_us; }

void core_set_input_callback(void (*callback)(void))
{
	s_input_callback = callback;
}

/******************************************************************************
	68000 clock: 10 MHz on the original CPS1, 12 MHz on the later boards
	(the sets MAME runs with cps1_12MHz / qsound / pang3: the CPS-Dash
	family and the QSound games). NJEMU had 10 MHz for every game; the
	build patch patches/njemu-cps1-timing.py makes timer.c ask here.
******************************************************************************/

/* parents of the 12 MHz families: a set is 12 MHz when it or its parent is listed */
static const char *const s_12mhz_families[] = {
	"sf2ce", "sf2hf", "wof", "dino", "punisher", "slammast", "mbombrd", "pang3",
	"megaman", "pnickj", "qtono2j", "cworld2j", "varth", "qad", "sfzch", "wofch", "cps1mult",
	NULL
};
/* 12 MHz revisions of games whose parent is a 10 MHz board */
static const char *const s_12mhz_sets[] = {
	"daimakair", "striderjr", "dynwarjr", "area88r", "ffightae", "cawingjr", NULL
};
/* 10 MHz boards inside the 12 MHz families (bootlegs verified by MAME) */
static const char *const s_10mhz_sets[] = {
	"sf2amf3", "sf2ceblp", "sf2cems6a", "sf2cems6b", "sf2cems6c", "sf2ceds6", NULL
};

static int in_list(const char *const *list, const char *name)
{
	for (; *list; list++)
		if (strcmp(*list, name) == 0) return 1;
	return 0;
}

static double m68k_nominal_clock(void)
{
#if EMU_SYSTEM == CPS2
	return 16000000.0;		/* every CPS2 board: a 16 MHz crystal */
#else
	if (in_list(s_10mhz_sets, game_name)) return 10000000.0;
	if (in_list(s_12mhz_sets, game_name)) return 12000000.0;
	if (in_list(s_12mhz_families, game_name)) return 12000000.0;
	if (parent_name[0] && in_list(s_12mhz_families, parent_name)) return 12000000.0;
	return 10000000.0;
#endif
}

/* The CPS-A/B stall the 68000 on its accesses to the graphics RAM and during
   their DMA; no emulator reproduces those wait states (MAME Testers 408, open
   since 2008), so the games run faster than the boards: the Street Fighter II
   games visibly so, Hyper Fighting most. Until the stalls are emulated the
   68000 gets a share of its clock: 60% by default, calibrated against a
   CPS1 board with the Street Fighter II games (55-65% depending on the
   game and the eye), in line with what PCB owners found for MAME (65% for
   Hyper Fighting, 60-65% for Captain Commando); the in-game menu and cpu= /
   cpu.<set>= in cpspi.cfg move it between 50 and 70. */
#if EMU_SYSTEM == CPS2
/* CPS2: MAME has run the 68000 at 0.7375 of its 16 MHz (11.8 MHz) since
   2008 to compensate for "RAM access waitstates etc." it does not emulate,
   and that is still too fast: Super Turbo's speed is not paced by the
   frame but by a tight loop of the 68000 (a spin counter, one game tick
   every 97 spins at speed 3), so the game runs exactly as fast as the CPU
   gets through memory: a board owner, timing rounds against his CPS2 at
   speed 3, found 56% the closest; a wide range around it */
#define CPU_PERCENT_DEFAULT	56
#define CPU_PERCENT_RANGE	20
#else
#define CPU_PERCENT_DEFAULT	60
#define CPU_PERCENT_RANGE	10
#endif
#define CPU_PERCENT_MIN		(CPU_PERCENT_DEFAULT - CPU_PERCENT_RANGE)
#define CPU_PERCENT_MAX		(CPU_PERCENT_DEFAULT + CPU_PERCENT_RANGE)
static unsigned s_cpu_override;		/* 0: the default */

unsigned core_cpu_percent(void)
{
	return s_cpu_override ? s_cpu_override : CPU_PERCENT_DEFAULT;
}

unsigned core_cpu_percent_centre(void)
{
	return CPU_PERCENT_DEFAULT;
}

unsigned core_cpu_percent_range(void)
{
	return CPU_PERCENT_RANGE;
}

double lr_m68k_clock(void)
{
	return m68k_nominal_clock() * core_cpu_percent() / 100.0;
}

void core_set_cpu_percent(unsigned percent)
{
	s_cpu_override = percent >= CPU_PERCENT_MIN && percent <= CPU_PERCENT_MAX ? percent : 0;
	if (s_loaded) timer_set_m68k_clock();
}

unsigned core_m68k_mhz(void)
{
	return (unsigned)(m68k_nominal_clock() / 1000000.0);
}

uint32_t poll_gamepad(void) { return 0; }
void pad_init(void) {}
void pad_exit(void) {}
void pad_wait_clear(void) {}
void pad_wait_press(int msec) { (void)msec; }
void pad_update(void) {}
bool pad_pressed(uint32_t code) { (void)code; return false; }
bool pad_pressed_any(void) { return false; }

/******************************************************************************
	core_* API (core_api.h)
******************************************************************************/

int core_init(const char *base_dir)
{
	strncpy(s_base, base_dir, sizeof(s_base) - 1);
	s_base[sizeof(s_base) - 1] = 0;
	snprintf(launchDir, sizeof(launchDir), "%s/", s_base);
	snprintf(screenshotDir, sizeof(screenshotDir), "%s/shots", s_base);
	snprintf(game_dir, sizeof(game_dir), "%s/roms", s_base);
	s_content[0] = 0;
	s_error[0] = 0;
	ui_text_data = ui_text_driver->init();
	if (!ui_text_data) return 0;
	sound_thread_init();
	return 1;
}

void core_unload(void)
{
	if (s_loaded || lr_initialized_machine) lr_machine_unload();
	if (video_data) { video_driver->free(video_data); video_data = NULL; }
	s_loaded = 0;
	memset(s_input, 0, sizeof(s_input));
}

int core_load(const char *name)
{
	core_unload();
	s_error[0] = 0;

	size_t n = strlen(name);
	if (n == 0 || n >= sizeof(game_name)) { snprintf(s_error, sizeof(s_error), "bad game name"); return 0; }
	for (size_t i = 0; i <= n; i++)
	{
		char c = name[i];
		game_name[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}
	parent_name[0] = 0;
	snprintf(s_content, sizeof(s_content), "%s/roms/%s.zip", s_base, game_name);

	if (zip_open(s_content) < 0)
	{
		zip_close();
		snprintf(s_error, sizeof(s_error), "invalid zip archive: %s", s_content);
		return 0;
	}
	zip_close();

	lr_options();
	lr_frame_number = 0;
	frames_displayed = 0;
	fatal_error = 0;
	video_data = video_driver->init(emu_layer_textures, emu_layer_textures_count, &emu_clut_info);
	if (!video_data) { snprintf(s_error, sizeof(s_error), "cannot allocate video buffers"); return 0; }
	if (!lr_machine_load())
	{
		if (!s_error[0]) snprintf(s_error, sizeof(s_error), "ROM set not loaded (see the log)");
		video_driver->free(video_data);
		video_data = NULL;
		return 0;
	}
	lr_options();
	s_loaded = 1;
	return 1;
}

void core_reset(void)
{
	if (!s_loaded) return;
	lr_options();
	lr_machine_reset();
}

int core_loaded(void) { return s_loaded; }

void core_frame(void)
{
	if (!s_loaded) return;
	lr_machine_frame();
	++lr_frame_number;
}

const uint16_t *core_video(void)
{
	lr_video *v = video_data;
	return v ? v->work : NULL;
}

int core_flipped(void) { return cps_flip_screen != 0; }
int core_vertical(void) { return machine_screen_type != 0; }

unsigned core_audio(const int16_t **samples)
{
	/* the last block, and the count produced since the previous call (the
	   blocks go to the kernel's callback as they are made) */
	*samples = s_out;
	unsigned n = s_audio_count;
	s_audio_count = 0;
	s_out_frames = 0;
	return n;
}

void core_set_input(unsigned player, uint32_t flags)
{
	if (player < CORE_MAX_PLAYERS) s_input[player] = flags;
}

unsigned core_players(void) { return s_loaded ? (unsigned)input_max_players : 2; }
unsigned core_buttons(void) { return s_loaded ? (unsigned)input_max_buttons : 6; }
int core_fatal(void) { return fatal_error; }
const char *core_error(void) { return s_error; }

void core_set_dips(unsigned a, unsigned b, unsigned c)
{
	s_dips[0] = a; s_dips[1] = b; s_dips[2] = c;
#if EMU_SYSTEM == CPS1
	if (s_loaded)
	{
		cps1_dipswitch[0] = a & 255;
		cps1_dipswitch[1] = b & 255;
		cps1_dipswitch[2] = c & 255;
	}
#endif
}

#if EMU_SYSTEM == CPS2
int core_qsound(void) { return 1; }		/* every CPS2 board */
#else
int core_qsound(void) { return machine_sound_type == SOUND_QSOUND; }
#endif

/* this core alone (a kernel with both cores answers from core_dispatch.c) */
#if EMU_SYSTEM == CPS2
unsigned core_systems(void) { return CORE_SYS_CPS2; }
unsigned core_system(void) { return CORE_SYS_CPS2; }
#else
unsigned core_systems(void) { return CORE_SYS_CPS1; }
unsigned core_system(void) { return CORE_SYS_CPS1; }
#endif
void core_select(unsigned system) { (void)system; }
const char *core_game_name(void) { return game_name; }
