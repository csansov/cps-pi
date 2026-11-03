/*
 * cps-pi - PNG encoding of a screenshot and decoding of the overlay (miniz)
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
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

/* ---- decoding: 8 bit RGB or RGBA, not interlaced (what any editor writes) ---- */

static uint32_t be32 (const uint8_t *p)
{
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static int iabs (int v)
{
	return v < 0 ? -v : v;
}

static int paeth (int a, int b, int c)
{
	int p = a + b - c;
	int pa = iabs (p - a), pb = iabs (p - b), pc = iabs (p - c);
	return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

/* RGB (3 bytes per pixel, malloc'd; free with cpspi_png_free_rgb) or 0 */
uint8_t *cpspi_png_decode (const void *pData, size_t nSize, unsigned *pWidth, unsigned *pHeight)
{
	static const uint8_t Signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
	const uint8_t *p = pData;
	if (nSize < 8 + 25 || memcmp (p, Signature, 8) != 0)
	{
		return 0;
	}

	/* the chunks: IHDR, the total size of the IDAT data, IEND */
	unsigned w = 0, h = 0, bpp = 0;
	size_t nIDAT = 0, pos = 8;
	while (pos + 12 <= nSize)
	{
		uint32_t len = be32 (p + pos);
		const uint8_t *type = p + pos + 4, *data = p + pos + 8;
		if (pos + 12 + len > nSize)
		{
			return 0;
		}
		if (memcmp (type, "IHDR", 4) == 0)
		{
			if (len < 13) return 0;
			w = be32 (data);
			h = be32 (data + 4);
			unsigned depth = data[8], ct = data[9], il = data[12];
			if (depth != 8 || (ct != 2 && ct != 6) || il != 0) return 0;
			bpp = ct == 6 ? 4 : 3;
		}
		else if (memcmp (type, "IDAT", 4) == 0)
		{
			nIDAT += len;
		}
		else if (memcmp (type, "IEND", 4) == 0)
		{
			break;
		}
		pos += 12 + len;
	}
	if (w == 0 || h == 0 || bpp == 0 || nIDAT == 0 || w > 4096 || h > 4096)
	{
		return 0;
	}

	uint8_t *z = malloc (nIDAT);
	if (z == 0) return 0;
	size_t zpos = 0;
	for (pos = 8; pos + 12 <= nSize; )
	{
		uint32_t len = be32 (p + pos);
		const uint8_t *type = p + pos + 4;
		if (memcmp (type, "IDAT", 4) == 0)
		{
			memcpy (z + zpos, p + pos + 8, len);
			zpos += len;
		}
		else if (memcmp (type, "IEND", 4) == 0)
		{
			break;
		}
		pos += 12 + len;
	}

	size_t stride = 1 + (size_t) w * bpp;
	mz_ulong nRaw = (mz_ulong) (stride * h);
	uint8_t *raw = malloc (nRaw);
	uint8_t *rgb = malloc ((size_t) w * h * 3);
	uint8_t *zero = malloc ((size_t) w * bpp);
	if (raw == 0 || rgb == 0 || zero == 0
	    || mz_uncompress (raw, &nRaw, z, (mz_ulong) nIDAT) != MZ_OK || nRaw != stride * h)
	{
		free (z); free (raw); free (rgb); free (zero);
		return 0;
	}
	free (z);
	memset (zero, 0, (size_t) w * bpp);

	/* the filters, undone in place row by row */
	const uint8_t *prev = zero;
	for (unsigned y = 0; y < h; y++)
	{
		uint8_t *cur = raw + y * stride + 1;
		unsigned filter = raw[y * stride];
		unsigned n = w * bpp;
		for (unsigned i = 0; i < n; i++)
		{
			int a = i >= bpp ? cur[i - bpp] : 0;
			int b = prev[i];
			int c = i >= bpp ? prev[i - bpp] : 0;
			switch (filter)
			{
			case 0: break;
			case 1: cur[i] = (uint8_t) (cur[i] + a); break;
			case 2: cur[i] = (uint8_t) (cur[i] + b); break;
			case 3: cur[i] = (uint8_t) (cur[i] + ((a + b) >> 1)); break;
			case 4: cur[i] = (uint8_t) (cur[i] + paeth (a, b, c)); break;
			default:
				free (raw); free (rgb); free (zero);
				return 0;
			}
		}
		uint8_t *out = rgb + (size_t) y * w * 3;
		for (unsigned x = 0; x < w; x++)
		{
			out[x * 3] = cur[x * bpp];
			out[x * 3 + 1] = cur[x * bpp + 1];
			out[x * 3 + 2] = cur[x * bpp + 2];
		}
		prev = cur;
	}
	free (raw);
	free (zero);
	*pWidth = w;
	*pHeight = h;
	return rgb;
}

void cpspi_png_free_rgb (void *p)
{
	free (p);
}
