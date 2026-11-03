/*
 * cps-pi - HDMI output (see video.h); derived from neo-pi's video.cpp
 */
#include "fontdata.h"
#include "video.h"
#include <circle/bcmpropertytags.h>
#include <circle/font.h>
#include <circle/util.h>
#include <circle/synchronize.h>
#include <circle/multicore.h>
#include <circle/memory.h>
#include <circle/timer.h>
#include <circle/bcm2835.h>
#include <circle/memio.h>

#define PROPTAG_RELEASE_BUFFER	0x00048001
#define PROPTAG_GET_PIXEL_ORDER	0x00040006	// 0 = BGR, 1 = RGB

#define MAX_LINE	(GAME_W * 8)		// longest scaled line we support

// ---- drawing on all four cores ---------------------------------------------
// Writing the picture into the framebuffer (non-cacheable memory) is bound by
// each core's write path, about 4.5 ms for the 4:3 picture on a Pi 3; four
// cores writing their own quarter take about a third of that. The job (source
// picture, flip) is published by bumping a generation counter and signalled
// with SEV; cores 1-3 wait in WFE in between and report with their own
// counters. The source picture and the tables are normal cacheable memory,
// which the cores keep coherent among themselves; the framebuffer needs no
// cache maintenance, only a barrier before the report.

#define DRAW_CORES	4
#define LINES_PER_CORE	(GAME_H / DRAW_CORES)

// per-core scratch: one scaled line, the same darkened, the converted source line
static u32 s_Line[DRAW_CORES][MAX_LINE];
static u32 s_Dark[DRAW_CORES][MAX_LINE];
static u32 s_Conv[DRAW_CORES][GAME_W + 1];

class CDrawCores : public CMultiCoreSupport
{
public:
	CDrawCores (CVideoOut *pVideo)
	:	CMultiCoreSupport (CMemorySystem::Get ()),
		m_pVideo (pVideo),
		m_nJob (0),
		m_pSource (0),
		m_bFlip (FALSE),
		m_bBroken (FALSE)
	{
		for (unsigned c = 0; c < DRAW_CORES; c++)
		{
			m_nDone[c] = 0;
			m_bAlive[c] = FALSE;
		}
	}

	// starts cores 1-3 and waits until they are in their loop
	boolean Start (void)
	{
		if (!Initialize ())
		{
			return FALSE;
		}
		unsigned nWait = CTimer::GetClockTicks ();
		while (CTimer::GetClockTicks () - nWait < 500000)
		{
			if (m_bAlive[1] && m_bAlive[2] && m_bAlive[3])
			{
				return TRUE;
			}
		}
		return FALSE;
	}

	boolean IsWorking (void) const	{ return !m_bBroken; }

	// the whole picture: core 0 takes the first share and waits for the others
	void Draw (const u16 *pSource, boolean bFlip)
	{
		m_pSource = pSource;
		m_bFlip = bFlip;
		DataMemBarrier ();
		unsigned nJob = ++m_nJob;
		DataSyncBarrier ();
		asm volatile ("sev");

		m_pVideo->DrawGameLines (pSource, 0, LINES_PER_CORE, s_Line[0], s_Dark[0], s_Conv[0], bFlip);

		unsigned nStart = CTimer::GetClockTicks ();
		for (unsigned c = 1; c < DRAW_CORES; c++)
		{
			while (m_nDone[c] != nJob)
			{
				if (CTimer::GetClockTicks () - nStart > 50000)
				{
					// a core did not answer: its share here, and core 0 alone from now on
					m_pVideo->DrawGameLines (pSource, c * LINES_PER_CORE, (c + 1) * LINES_PER_CORE,
								 s_Line[0], s_Dark[0], s_Conv[0], bFlip);
					m_bBroken = TRUE;
					break;
				}
			}
		}
		DataMemBarrier ();
	}

	void Run (unsigned nCore) override
	{
		if (nCore == 0 || nCore >= DRAW_CORES)
		{
			return;
		}
		m_bAlive[nCore] = TRUE;
		DataSyncBarrier ();
		unsigned nSeen = 0;
		while (1)
		{
			while (m_nJob == nSeen)
			{
				asm volatile ("wfe" ::: "memory");
			}
			nSeen = m_nJob;
			DataMemBarrier ();
			m_pVideo->DrawGameLines (m_pSource, nCore * LINES_PER_CORE, (nCore + 1) * LINES_PER_CORE,
						 s_Line[nCore], s_Dark[nCore], s_Conv[nCore], m_bFlip);
			DataSyncBarrier ();		// every store to the framebuffer done
			m_nDone[nCore] = nSeen;
			DataSyncBarrier ();
			asm volatile ("sev");
		}
	}

private:
	CVideoOut *m_pVideo;
	volatile unsigned m_nJob;
	volatile unsigned m_nDone[DRAW_CORES];
	volatile boolean m_bAlive[DRAW_CORES];
	const u16 *volatile m_pSource;
	volatile boolean m_bFlip;
	volatile boolean m_bBroken;
};

CVideoOut::CVideoOut (void)
:	m_pFrameBuffer (0),
	m_nDispW (0),
	m_nDispH (0),
	m_nWidth (0),
	m_nHeight (0),
	m_nPitchPx (0),
	m_pBase (0),
	m_nBack (1),
	m_bGameMode (FALSE),
	m_nTextScale (1),
	m_nScale (1),
	m_nK (1),
	m_nM (1),
	m_nMX (1),
	m_nAspect (ASPECT_43),
	m_nScreenW (GAME_W),
	m_nFilter (FILTER_NONE),
	m_nGameX (0),
	m_nGameY (0),
	m_nStatusY (0),
	m_nStatusScale (1),
	m_nTopY (0),
	m_pLUT (0),
	m_pColMap (0),
	m_pColWeight (0),
	m_nPixelOrder (1),
	m_bRLow (TRUE),
	m_pDrawCores (0)
{
}

CVideoOut::~CVideoOut (void)
{
	delete m_pFrameBuffer;
	delete [] m_pLUT;
	delete [] m_pColMap;
	delete [] m_pColWeight;
}

boolean CVideoOut::StartDrawCores (void)
{
	if (m_pDrawCores != 0)
	{
		return m_pDrawCores->IsWorking ();
	}
	CDrawCores *pCores = new CDrawCores (this);
	if (pCores == 0)
	{
		return FALSE;
	}
	if (!pCores->Start ())
	{
		// the cores that did start, if any, sleep in WFE for good
		return FALSE;
	}
	m_pDrawCores = pCores;
	return TRUE;
}

unsigned CVideoOut::GetDrawCores (void) const
{
	return m_pDrawCores != 0 && m_pDrawCores->IsWorking () ? DRAW_CORES : 1;
}

// BGR555 (R in bits 0-4, G 5-9, B 10-14) -> framebuffer pixel
void CVideoOut::BuildLUT (void)
{
	for (unsigned c = 0; c < 32768; c++)
	{
		unsigned r5 = c & 0x1F, g5 = (c >> 5) & 0x1F, b5 = (c >> 10) & 0x1F;
		unsigned r = (r5 << 3) | (r5 >> 2);
		unsigned g = (g5 << 3) | (g5 >> 2);
		unsigned b = (b5 << 3) | (b5 >> 2);
		m_pLUT[c] = Native (RGB (r, g, b));
	}
}

boolean CVideoOut::Allocate (unsigned nWidth, unsigned nHeight)
{
	if (m_pFrameBuffer != 0)
	{
		CBcmPropertyTags Tags;
		TPropertyTagSimple Release;
		Tags.GetTag (PROPTAG_RELEASE_BUFFER, &Release, sizeof Release);	// may be unsupported
		delete m_pFrameBuffer;
	}

	m_pFrameBuffer = new CBcmFrameBuffer (nWidth, nHeight, 32, nWidth, nHeight * 2, 0, TRUE);
	if (   m_pFrameBuffer == 0
	    || !m_pFrameBuffer->Initialize ())
	{
		return FALSE;
	}

	m_nWidth   = m_pFrameBuffer->GetWidth ();
	m_nHeight  = m_pFrameBuffer->GetHeight ();
	m_nPitchPx = m_pFrameBuffer->GetPitch () / 4;
	m_pBase    = (u32 *) (uintptr) m_pFrameBuffer->GetBuffer ();
	if (   m_pBase == 0
	    || m_nWidth != nWidth || m_nHeight != nHeight
	    || m_pFrameBuffer->GetVirtHeight () < 2 * m_nHeight)
	{
		return FALSE;
	}

	// Byte order of the pixels: QEMU reports RGB (1, red in the low byte),
	// the Pi 4 firmware (framebuffer_swap=1 by default) reports BGR (0, red
	// in the high byte)
	CBcmPropertyTags Tags;
	TPropertyTagSimple Order;
	if (Tags.GetTag (PROPTAG_GET_PIXEL_ORDER, &Order, sizeof Order))
	{
		m_nPixelOrder = Order.nValue;
	}
	m_bRLow = m_nPixelOrder != 0;
	BuildLUT ();

	Clear (0, RGB (0, 0, 0));
	Clear (1, RGB (0, 0, 0));
	ShowBuffer (0);
	return TRUE;
}

boolean CVideoOut::Initialize (void)
{
	// current HDMI mode (chosen by the firmware from config.txt / EDID)
	CBcmPropertyTags Tags;
	TPropertyTagDisplayDimensions Dim;
	if (   Tags.GetTag (PROPTAG_GET_DISPLAY_DIMENSIONS, &Dim, sizeof Dim)
	    && Dim.nWidth >= 640 && Dim.nWidth <= 4096
	    && Dim.nHeight >= 480 && Dim.nHeight <= 2160)
	{
		m_nDispW = Dim.nWidth;
		m_nDispH = Dim.nHeight;
	}
	else
	{
		m_nDispW = 640;
		m_nDispH = 480;
	}

	m_pLUT = new u32[32768];
	m_pColMap = new u16[MAX_LINE];
	m_pColWeight = new u8[MAX_LINE];
	if (m_pLUT == 0 || m_pColMap == 0 || m_pColWeight == 0)
	{
		return FALSE;
	}

	return SetConsoleMode ();
}

boolean CVideoOut::SetConsoleMode (void)
{
	m_bGameMode = FALSE;
	m_nTextScale = m_nDispH >= 900 ? 2 : 1;
	return Allocate (m_nDispW, m_nDispH);
}

boolean CVideoOut::SetGameMode (unsigned nFilter, boolean bAllowHVS, unsigned nAspect)
{
	m_nFilter = nFilter < FILTER_COUNT ? nFilter : FILTER_NONE;
	m_nAspect = nAspect < ASPECT_COUNT ? nAspect : ASPECT_43;
	const boolean bAspect43 = m_nAspect != ASPECT_PIXEL;

	// vertical integer scale S, and the picture's width on screen: 4/3 of
	// its height (the arcade monitor) or square pixels
	unsigned S = m_nDispH / GAME_H;
	if (S < 1) S = 1;
	if (S > 8) S = 8;
	unsigned nScreenW = bAspect43 ? (GAME_H * S * 4 + 1) / 3 : GAME_W * S;
	while (S > 1 && nScreenW > m_nDispW)
	{
		S--;
		nScreenW = bAspect43 ? (GAME_H * S * 4 + 1) / 3 : GAME_W * S;
	}
	m_nScale = S;

	// HVS factor k: largest divisor of S that divides the display exactly
	// (the firmware keeps the framebuffer's proportions, so it must have the
	// display's); with a filter the CPU must draw at least 2x (m >= 2); for
	// 4:3 the picture in the framebuffer must keep its 384 columns, so that
	// the CPU spreads them over nScreenW / k columns (597 at 1080p, k = 2)
	// instead of writing the whole 1195 x 896 picture (10 ms on a Pi 3)
	m_nK = 1;
	if (bAllowHVS)
	{
		for (unsigned k = S; k >= 2; k--)
		{
			if (   S % k == 0 && m_nDispH % k == 0 && m_nDispW % k == 0
			    && (m_nFilter == FILTER_NONE || S / k >= 2)
			    && (!bAspect43 || nScreenW / k >= GAME_W))
			{
				m_nK = k;
				break;
			}
		}
	}
	m_nM = S / m_nK;
	m_nMX = bAspect43 ? 0 : m_nM;		// 0: column map instead of a factor

	unsigned nFbW = m_nDispW / m_nK, nFbH = m_nDispH / m_nK;
	if (!Allocate (nFbW, nFbH))
	{
		// fall back to the full resolution, CPU only
		m_nK = 1;
		m_nM = S;
		m_nMX = bAspect43 ? 0 : S;
		if (!Allocate (m_nDispW, m_nDispH))
		{
			return FALSE;
		}
	}

	// 4:3: the picture is nFbGameW framebuffer columns wide; each takes the
	// source column under its centre (sharp) or a blend of the two nearest
	// (8 bit weights), like the beam of a CRT crossing the pixel clock
	unsigned nFbGameW = bAspect43 ? nScreenW / m_nK : GAME_W * m_nMX;
	if (bAspect43)
	{
		if (nFbGameW > MAX_LINE) nFbGameW = MAX_LINE;
		for (unsigned x = 0; x < nFbGameW; x++)
		{
			if (m_nAspect == ASPECT_43_SHARP)
			{
				unsigned nSrc = ((2 * x + 1) * GAME_W) / (2 * nFbGameW);
				m_pColMap[x] = (u16) (nSrc < GAME_W ? nSrc : GAME_W - 1);
				m_pColWeight[x] = 0;
			}
			else
			{
				// source position of the column centre, in 1/256 columns
				int nPos = (int) (((2 * x + 1) * GAME_W * 256) / (2 * nFbGameW)) - 128;
				if (nPos < 0) nPos = 0;
				unsigned nSrc = (unsigned) nPos >> 8, nW = (unsigned) nPos & 255;
				if (nSrc >= GAME_W - 1)
				{
					nSrc = GAME_W - 1;
					nW = 0;
				}
				m_pColMap[x] = (u16) nSrc;
				m_pColWeight[x] = (u8) nW;
			}
		}
		nScreenW = nFbGameW * m_nK;
	}
	m_nScreenW = nScreenW;

	unsigned nGameW = nFbGameW, nGameH = GAME_H * m_nM;
	m_nGameX = ((m_nWidth - nGameW) / 2) & ~3u;	// 16 byte aligned rows for the copies
	m_nGameY = (m_nHeight - nGameH) / 2;

	// text in the black bands: 8x8 menu font, 16 px high on screen where possible
	m_nStatusScale = m_nK >= 2 ? 1 : 2;
	unsigned nTextH = 8 * m_nStatusScale + m_nStatusScale;		// + shadow
	unsigned nBottom = m_nHeight - (m_nGameY + nGameH);
	m_nStatusY = m_nGameY + nGameH + (nBottom > nTextH ? (nBottom - nTextH) / 2 : 0);
	m_nTopY = m_nGameY > nTextH ? (m_nGameY - nTextH) / 2 : 0;

	m_bGameMode = TRUE;
	return TRUE;
}

unsigned CVideoOut::GetColumns (void) const
{
	return m_nWidth / (m_Font.GetCharWidth () * m_nTextScale);
}

unsigned CVideoOut::GetRows (void) const
{
	return m_nHeight / (m_Font.GetCharHeight () * m_nTextScale);
}

void CVideoOut::Clear (unsigned nBuffer, u32 nColor)
{
	nColor = Native (nColor);
	for (unsigned y = 0; y < m_nHeight; y++)
	{
		u32 *p = Row (nBuffer, y);
		for (unsigned x = 0; x < m_nWidth; x++)
		{
			p[x] = nColor;
		}
	}
}

void CVideoOut::ClearRows (unsigned nBuffer, unsigned nFirstRow, unsigned nRows, u32 nColor)
{
	nColor = Native (nColor);
	unsigned nCharH = m_Font.GetCharHeight () * m_nTextScale;
	unsigned y0 = nFirstRow * nCharH;
	unsigned y1 = (nFirstRow + nRows) * nCharH;
	if (y1 > m_nHeight)
	{
		y1 = m_nHeight;
	}
	for (unsigned y = y0; y < y1; y++)
	{
		u32 *p = Row (nBuffer, y);
		for (unsigned x = 0; x < m_nWidth; x++)
		{
			p[x] = nColor;
		}
	}
}

void CVideoOut::DrawChar (unsigned nBuffer, unsigned nX, unsigned nY, char ch, u32 nFG, u32 nBG,
			  unsigned nTextScale, const CCharGenerator &Font)
{
	nFG = Native (nFG);
	nBG = Native (nBG);
	unsigned nCharW = Font.GetCharWidth ();
	unsigned nCharH = Font.GetCharHeight ();
	if (   nX + nCharW * nTextScale > m_nWidth
	    || nY + nCharH * nTextScale > m_nHeight)
	{
		return;
	}

	for (unsigned cy = 0; cy < nCharH; cy++)
	{
		CCharGenerator::TPixelLine Line = Font.GetPixelLine (ch, cy);
		for (unsigned sy = 0; sy < nTextScale; sy++)
		{
			u32 *p = Row (nBuffer, nY + cy * nTextScale + sy) + nX;
			for (unsigned cx = 0; cx < nCharW; cx++)
			{
				u32 nColor = Font.GetPixel (cx, Line) ? nFG : nBG;
				for (unsigned sx = 0; sx < nTextScale; sx++)
				{
					*p++ = nColor;
				}
			}
		}
	}
}

void CVideoOut::DrawText (unsigned nBuffer, unsigned nCol, unsigned nRow, const char *pText,
			  u32 nFG, u32 nBG)
{
	unsigned nCharW = m_Font.GetCharWidth () * m_nTextScale;
	unsigned nCharH = m_Font.GetCharHeight () * m_nTextScale;
	for (unsigned i = 0; pText[i] != '\0' && nCol + i < GetColumns (); i++)
	{
		DrawChar (nBuffer, (nCol + i) * nCharW, nRow * nCharH, pText[i], nFG, nBG,
			  m_nTextScale, m_Font);
	}
}

void CVideoOut::DrawGame (const u16 *pSource, boolean bFlip)
{
	if (m_pDrawCores != 0 && m_pDrawCores->IsWorking ())
	{
		m_pDrawCores->Draw (pSource, bFlip);
	}
	else
	{
		DrawGameLines (pSource, 0, GAME_H, s_Line[0], s_Dark[0], s_Conv[0], bFlip);
	}
}

void CVideoOut::DrawGameLines (const u16 *pSource, unsigned nFirst, unsigned nEnd,
			       u32 *pLine, u32 *pDark, u32 *pConv, boolean bFlip)
{
	for (unsigned y = nFirst; y < nEnd; y++)
	{
		DrawGameLine (pSource, y, pLine, pDark, pConv, bFlip);
	}
}

void CVideoOut::DrawGameLine (const u16 *pSource, unsigned y, u32 *pLine, u32 *pDark, u32 *pConv,
			      boolean bFlip)
{
	const unsigned M = m_nM, MX = m_nMX;
	const unsigned nGameW = MX ? GAME_W * MX : m_nScreenW / m_nK;

	// scanlines: bottom rows of each group darker (2 of 4 at 4x, 1 of 2 or 3)
	const unsigned nDarkRows = m_nFilter != FILTER_NONE ? (M >= 4 ? M / 2 : (M >= 2 ? 1 : 0)) : 0;

	// expand one source line horizontally into cached memory, converting
	// the core's BGR555 to the framebuffer pixel through the table
	const unsigned nSrcY = bFlip ? GAME_H - 1 - y : y;
	const u16 *pSrc = pSource + (GAME_SRC_Y + nSrcY) * GAME_SRC_PITCH + GAME_SRC_X;
	const u32 *pLUT = m_pLUT;
	u32 *pDst = pLine;
	if (MX == 0)
	{
		// 4:3: the 384 source pixels are converted once into a small cached
		// line (the 128 KB colour table is what misses the cache), then each
		// framebuffer column is a blend of source columns i and i + 1 with
		// weight w / 256 (w = 0 reproduces column i exactly: no branch); the
		// two 16 bit lanes trick blends the four bytes with two multiplies
		if (bFlip)
		{
			for (unsigned x = 0; x < GAME_W; x++)
			{
				pConv[x] = pLUT[pSrc[GAME_W - 1 - x] & 0x7FFF];
			}
		}
		else
		{
			for (unsigned x = 0; x < GAME_W; x++)
			{
				pConv[x] = pLUT[pSrc[x] & 0x7FFF];
			}
		}
		pConv[GAME_W] = pConv[GAME_W - 1];
		const u16 *pMap = m_pColMap;
		const u8 *pW = m_pColWeight;
		for (unsigned x = 0; x < nGameW; x++)
		{
			unsigned i = pMap[x], w = pW[x], iw = 256 - w;
			u32 a = pConv[i], b = pConv[i + 1];
			u32 rb = ((a & 0x00FF00FFu) * iw + (b & 0x00FF00FFu) * w) >> 8;
			u32 ga = (((a >> 8) & 0x00FF00FFu) * iw + ((b >> 8) & 0x00FF00FFu) * w) >> 8;
			pDst[x] = (rb & 0x00FF00FFu) | ((ga & 0x00FF00FFu) << 8);
		}
	}
	else if (bFlip)
	{
		for (unsigned x = 0; x < GAME_W; x++)
		{
			u32 c = pLUT[pSrc[GAME_W - 1 - x] & 0x7FFF];
			for (unsigned s = 0; s < MX; s++)
			{
				*pDst++ = c;
			}
		}
	}
	else if (MX == 1)
	{
		for (unsigned x = 0; x < GAME_W; x++)
		{
			*pDst++ = pLUT[pSrc[x] & 0x7FFF];
		}
	}
	else
	{
		for (unsigned x = 0; x < GAME_W; x++)
		{
			u32 c = pLUT[pSrc[x] & 0x7FFF];
			for (unsigned s = 0; s < MX; s++)
			{
				*pDst++ = c;
			}
		}
	}

	unsigned nDstY = m_nGameY + y * M;

	if (nDarkRows)
	{
		// shifts and masks only (75% = 50% + 25%)
		const u32 *pL = pLine;
		u32 *pD = pDark;
		switch (m_nFilter)
		{
		case FILTER_SCAN_LIGHT:
			for (unsigned x = 0; x < nGameW; x++)
			{
				u32 c = pL[x];
				pD[x] = 0xFF000000u | (((c >> 1) & 0x007F7F7Fu) + ((c >> 2) & 0x003F3F3Fu));
			}
			break;
		case FILTER_SCAN_STRONG:
			for (unsigned x = 0; x < nGameW; x++)
			{
				pD[x] = 0xFF000000u | ((pL[x] >> 2) & 0x003F3F3Fu);
			}
			break;
		default:
			for (unsigned x = 0; x < nGameW; x++)
			{
				pD[x] = 0xFF000000u | ((pL[x] >> 1) & 0x007F7F7Fu);
			}
			break;
		}
	}
	for (unsigned s = 0; s < M; s++)
	{
		const u32 *pSrcLine = s >= M - nDarkRows ? pDark : pLine;
		memcpy (Row (m_nBack, nDstY + s) + m_nGameX, pSrcLine, nGameW * 4);
	}
}

// One centred line in the top or bottom black band (nSpans = 0 clears it)
void CVideoOut::DrawBandText (boolean bTop, const TTextSpan *pSpans, unsigned nSpans)
{
	const unsigned S = m_nStatusScale;
	const unsigned nY = bTop ? m_nTopY : m_nStatusY;
	const unsigned nH = 9 * S;					// glyph + shadow

	for (unsigned y = nY; y < nY + nH && y < m_nHeight; y++)
	{
		u32 *p = Row (m_nBack, y);
		if (y >= m_nGameY && y < m_nHeight - m_nGameY)
		{
			// never inside the picture: clear only the side bands
			continue;
		}
		for (unsigned x = 0; x < m_nWidth; x++)
		{
			p[x] = 0xFF000000u;
		}
	}

	unsigned nChars = 0;
	for (unsigned i = 0; i < nSpans; i++)
	{
		nChars += strlen (pSpans[i].pText);
	}
	unsigned nW = nChars * 8 * S;
	if (nSpans == 0 || nW > m_nWidth)
	{
		return;
	}

	unsigned x = (m_nWidth - nW) / 2;
	for (unsigned i = 0; i < nSpans; i++)
	{
		u32 c0 = pSpans[i].nColor;			// 0xRRGGBB, like the UI colours
		u32 nColor = Native (0xFF000000u | ((c0 >> 16) & 0xFF) | (c0 & 0xFF00) | ((c0 & 0xFF) << 16));
		for (const char *s = pSpans[i].pText; *s; s++, x += 8 * S)
		{
			unsigned ch = (unsigned char) *s;
			if (ch < 32 || ch > 127)
			{
				continue;
			}
			if (ch >= 'a' && ch <= 'z')
			{
				ch -= 32;		// capitals only, like the menus
			}
			for (int pass = 0; pass < 2; pass++)		// shadow, then glyph
			{
				u32 c = pass == 0 ? 0xFF000000u : nColor;
				unsigned o = pass == 0 ? S : 0;
				for (unsigned r = 0; r < 8; r++)
				{
					unsigned char nRow = g_MenuFont[ch - 32][r];
					for (unsigned k = 0; k < 8; k++)
					{
						if (!(nRow & (0x80 >> k)))
						{
							continue;
						}
						for (unsigned sy = 0; sy < S; sy++)
						{
							unsigned yy = nY + r * S + sy + o;
							if (yy >= m_nHeight) continue;
							u32 *p = Row (m_nBack, yy) + x + k * S + o;
							for (unsigned sx = 0; sx < S; sx++)
							{
								p[sx] = c;
							}
						}
					}
				}
			}
		}
	}
}

void CVideoOut::ShowBuffer (unsigned nBuffer)
{
	DataSyncBarrier ();
	m_pFrameBuffer->SetVirtualOffset (0, nBuffer * m_nHeight);
	m_nBack = nBuffer ^ 1;
}

boolean CVideoOut::Flip (void)
{
	// the framebuffer is Normal non-cacheable memory (patches/circle-fb-normal-nc.py):
	// every store must have left the write buffer before the GPU is told
	DataSyncBarrier ();
	boolean bOK = m_pFrameBuffer->SetVirtualOffset (0, m_nBack * m_nHeight);
	m_nBack ^= 1;
	return bOK;
}

boolean CVideoOut::WaitForVSync (void)
{
	return m_pFrameBuffer->WaitForVerticalSync ();
}

u32 CVideoOut::ReadHVSStat (unsigned nChannel) const
{
	// HVS at bus 0x7E400000; SCALER_DISPSTAT0/1/2 at 0x48, 0x58, 0x68
	return read32 (ARM_IO_BASE + 0x400000 + 0x48 + (nChannel < 3 ? nChannel : 0) * 0x10);
}
