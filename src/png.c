/*
 * cps-pi - PNG encoding of a screenshot (miniz)
 */
#include <stddef.h>
#include "miniz.h"

/* PNG of an RGB image (3 bytes per pixel); free the result with cpspi_png_free */
void *cpspi_png_encode (const void *pRGB, int nWidth, int nHeight, size_t *pSize)
{
	return tdefl_write_image_to_png_file_in_memory_ex (pRGB, nWidth, nHeight, 3, pSize, 6, MZ_FALSE);
}

void cpspi_png_free (void *p)
{
	mz_free (p);
}
