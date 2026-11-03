/*
 * cps-pi - HDMI output
 *
 * Console mode: framebuffer at the HDMI resolution (boot text, errors).
 * Game mode: the 384x224 CPS1 picture scaled by an integer S vertically,
 * split between the CPU (draw scale m) and the hardware scaler HVS (factor
 * k), S = m * k; the framebuffer is (display height / k) lines high and the
 * HVS enlarges it exactly k times vertically. Horizontally:
 *   - 4:3 (the arcade monitor's shape; CPS1 pixels are not square): the CPU
 *     draws the picture mx times wider and the framebuffer width is chosen so
 *     that the HVS stretches it to 4/3 of the picture height on screen;
 *   - square pixels: the same integer factor in both directions.
 * Black borders are drawn by us inside the framebuffer.
 * Filters (applied while drawing, no extra buffering, no added lag):
 *   scanlines in three strengths (dark rows at 75%, 50% or 25%).
 *
 * The source picture is the core's BGR555 work buffer (512 x 272, the game
 * at 64,16); the menus use a buffer with the same layout.
 */
#ifndef VIDEO_H
#define VIDEO_H

#include <circle/bcmframebuffer.h>
#include <circle/chargenerator.h>
#include <circle/types.h>

// CPS1 visible area inside the core's work buffer (512 x 272, pitch 512)
#define GAME_SRC_PITCH   512
#define GAME_SRC_LINES   272
#define GAME_SRC_X       64
#define GAME_SRC_Y       16
#define GAME_W           384
#define GAME_H           224

#define FILTER_NONE	0
#define FILTER_SCAN_LIGHT  1	// dark rows at 75%
#define FILTER_SCAN_MEDIUM 2	// dark rows at 50%
#define FILTER_SCAN_STRONG 3	// dark rows at 25%
#define FILTER_COUNT	4

// Picture shape: the arcade monitor's 4:3 (the 384 columns spread over 4/3
// of the picture height, blended between neighbours like a CRT, or sharp:
// nearest column) or square pixels (integer scale in both directions)
#define ASPECT_43	0
#define ASPECT_43_SHARP	1
#define ASPECT_PIXEL	2
#define ASPECT_COUNT	3

// Logical colors (R in the low byte); CVideoOut converts them if the
// framebuffer stores red in the high byte (Pi 4)
#define RGB(r, g, b)     (0xFF000000u | ((u32)(b) << 16) | ((u32)(g) << 8) | (u32)(r))

// BGR555 pixel of the core's buffers from 8 bit components
#define BGR555(r, g, b)  ((u16) ((((b) & 0xF8) << 7) | (((g) & 0xF8) << 2) | (((r) & 0xF8) >> 3)))

class CVideoOut
{
public:
	CVideoOut (void);
	~CVideoOut (void);

	boolean Initialize (void);		// console mode

	// Game mode for a filter and a picture shape (ASPECT_*); bAllowHVS = FALSE
	// forces CPU scaling
	boolean SetGameMode (unsigned nFilter, boolean bAllowHVS, unsigned nAspect);
	boolean SetConsoleMode (void);
	boolean IsGameMode (void) const		{ return m_bGameMode; }

	unsigned GetWidth (void) const		{ return m_nDispW; }	// HDMI resolution
	unsigned GetHeight (void) const		{ return m_nDispH; }
	unsigned GetScale (void) const		{ return m_nScale; }	// vertical integer scale S
	unsigned GetHVSFactor (void) const	{ return m_nK; }	// vertical k (1 = CPU only)
	unsigned GetCPUScaleX (void) const	{ return m_nMX; }	// CPU draw scales
	unsigned GetCPUScaleY (void) const	{ return m_nM; }
	unsigned GetFilter (void) const		{ return m_nFilter; }
	unsigned GetAspect (void) const		{ return m_nAspect; }
	boolean IsAspect43 (void) const		{ return m_nAspect != ASPECT_PIXEL; }
	unsigned GetFBWidth (void) const	{ return m_nWidth; }	// framebuffer before the HVS
	unsigned GetFBHeight (void) const	{ return m_nHeight; }
	unsigned GetScreenGameW (void) const	{ return m_nScreenW; }	// picture size on screen
	unsigned GetScreenGameH (void) const	{ return GAME_H * m_nScale; }

	// Text grid (character cells) of the console
	unsigned GetColumns (void) const;
	unsigned GetRows (void) const;
	void Clear (unsigned nBuffer, u32 nColor);
	void DrawText (unsigned nBuffer, unsigned nCol, unsigned nRow, const char *pText,
		       u32 nFG, u32 nBG);
	void ClearRows (unsigned nBuffer, unsigned nFirstRow, unsigned nRows, u32 nColor);

	// Game: draws the visible 384x224 area into the back buffer, on every
	// core StartDrawCores() brought up (bFlip: the picture upside down, as
	// the cocktail flip of the hardware)
	void DrawGame (const u16 *pSource, boolean bFlip = FALSE);
	// lines [nFirst, nEnd) of the game picture into the back buffer, with the
	// caller's scratch lines (GAME_W * 8 pixels each) and converted source
	// line (GAME_W + 1 pixels)
	void DrawGameLines (const u16 *pSource, unsigned nFirst, unsigned nEnd,
			    u32 *pLine, u32 *pDark, u32 *pConv, boolean bFlip);
	void DrawGameLine (const u16 *pSource, unsigned y, u32 *pLine, u32 *pDark, u32 *pConv,
			   boolean bFlip);
	// Cores 1-3 draw their share of every picture from now on; FALSE = they
	// did not start, core 0 draws alone
	boolean StartDrawCores (void);
	unsigned GetDrawCores (void) const;	// 1 or 4
	// One centred line of text in the top or bottom black band (no spans = clear)
	struct TTextSpan { const char *pText; u32 nColor; };	// colour: 0xRRGGBB
	void DrawBandText (boolean bTop, const TTextSpan *pSpans, unsigned nSpans);

	// Colors: RGB() values are converted to the framebuffer byte order
	unsigned GetPixelOrder (void) const	{ return m_nPixelOrder; }
	boolean IsRedLow (void) const		{ return m_bRLow; }

	unsigned GetBackBuffer (void) const	{ return m_nBack; }
	void ShowBuffer (unsigned nBuffer);	// show a buffer now
	boolean Flip (void);			// show back buffer, swap roles
	// Blocks until the next frame begins: the HVS line counter of the HDMI
	// channel wraps (after ProbeHVS), or the firmware's notification
	boolean WaitForVSync (void);
	// Looks for the HVS display channel whose line counter runs with the
	// output (status register: bits 0-11 the line being composed, 12-17 a
	// frame counter, 30-31 the mode, 2 running / 3 end of frame); TRUE =
	// found, WaitForVSync() polls it from now on
	boolean ProbeHVS (void);
	int GetHVSChannel (void) const		{ return m_nHVSChannel; }
	unsigned GetHVSLine (void) const	{ return ReadHVSStat (m_nHVSChannel >= 0 ? (unsigned) m_nHVSChannel : 1) & 0xFFF; }
	u32 ReadHVSStat (unsigned nChannel) const;

	// A file written by core 3 while core 0 plays (the log): the data must
	// stay untouched until the job is done; any other file access from core 0
	// must WaitForFileJob() first. FALSE = no cores (write it yourself)
	boolean PostFileJob (const char *pPath, const char *pData, size_t nSize);
	void WaitForFileJob (void);

private:
	boolean Allocate (unsigned nWidth, unsigned nHeight);
	void BuildLUT (void);
	u32 *Row (unsigned nBuffer, unsigned nY) const
	{
		return m_pBase + (nBuffer * m_nHeight + nY) * m_nPitchPx;
	}
	void DrawChar (unsigned nBuffer, unsigned nX, unsigned nY, char ch, u32 nFG, u32 nBG,
		       unsigned nTextScale, const CCharGenerator &Font);

private:
	CBcmFrameBuffer *m_pFrameBuffer;
	CCharGenerator	 m_Font;		// console font (8x16)

	unsigned m_nDispW, m_nDispH;		// HDMI mode
	unsigned m_nWidth, m_nHeight;		// current framebuffer
	unsigned m_nPitchPx;
	u32	*m_pBase;
	unsigned m_nBack;
	boolean	 m_bGameMode;

	unsigned m_nTextScale;			// console text scale (1 or 2)
	unsigned m_nScale;			// S (vertical)
	unsigned m_nK;				// HVS factor (vertical)
	unsigned m_nM;				// CPU draw scale, vertical
	unsigned m_nMX;				// CPU draw scale, horizontal (0: 4:3 column map)
	unsigned m_nAspect;			// ASPECT_*
	unsigned m_nScreenW;			// picture width on screen
	unsigned m_nFilter;
	unsigned m_nGameX, m_nGameY;		// game rectangle in the framebuffer
	unsigned m_nStatusY;
	unsigned m_nStatusScale;
	unsigned m_nTopY;

	u32 Native (u32 c) const		// RGB() (R low) -> framebuffer order
	{
		return m_bRLow ? c : (c & 0xFF00FF00u) | ((c >> 16) & 0xFFu) | ((c & 0xFFu) << 16);
	}

	u32	*m_pLUT;			// BGR555 -> framebuffer pixel (32768 entries)
	u16	*m_pColMap;			// 4:3: source column of each framebuffer picture column
	u8	*m_pColWeight;			// 4:3: weight (0-255) of the next source column (blend)
	unsigned m_nPixelOrder;			// reported by the firmware
	boolean	 m_bRLow;			// red in the low byte of a pixel
	class CDrawCores *m_pDrawCores;		// cores 1-3 drawing (0: core 0 alone)
	int	 m_nHVSChannel;			// HVS channel polled for the vsync (-1: firmware)
};

#endif
