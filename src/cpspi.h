/*
 * cps-pi - interface shared by the C glue/shim and the C++ frontend
 */
#ifndef CPSPI_H
#define CPSPI_H

#define CPSPI_VERSION	"0.8.0"

/* the system this kernel emulates: set by the Makefile (-DCPSPI_SYSTEM=\"CPS2\"
   -DCPSPI_SYS_EXT=\"cps2\" -DCPSPI_CPS2); CPS1 unless said otherwise */
#ifndef CPSPI_SYSTEM
#define CPSPI_SYSTEM	"CPS1"
#define CPSPI_SYS_EXT	"cps1"
#define CPSPI_CPS1	1
#endif

/* Error codes = number of green LED blinks when the screen is unusable */
#define CPSPI_ERR_FB	2	/* framebuffer / HDMI */
#define CPSPI_ERR_SD	3	/* SD card or FAT file system */
#define CPSPI_ERR_DB	4	/* cpspi/rominfo.<system> missing */
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
