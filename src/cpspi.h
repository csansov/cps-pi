/*
 * cps-pi - interface shared by the C glue/shim and the C++ frontend
 */
#ifndef CPSPI_H
#define CPSPI_H

#define CPSPI_VERSION	"0.6.2"

/* Error codes = number of green LED blinks when the screen is unusable */
#define CPSPI_ERR_FB	2	/* framebuffer / HDMI */
#define CPSPI_ERR_SD	3	/* SD card or FAT file system */
#define CPSPI_ERR_DB	4	/* cpspi/rominfo.cps1 missing */
#define CPSPI_ERR_GAME	5	/* no playable game in cpspi/roms */
#define CPSPI_ERR_CORE	6	/* emulator core error */
#define CPSPI_ERR_MEM	7	/* out of memory */

#ifdef __cplusplus
extern "C" {
#endif

void cpspi_log (const char *fmt, ...);
void cpspi_fatal (int code, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
