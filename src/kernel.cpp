/*
 * cps-pi - kernel: game list, play with USB pads and HDMI audio, in-game
 * menu, automatic diagnostics in LOG.TXT. Derived from neo-pi's kernel.cpp.
 */
#include "kernel.h"
#include "fontdata.h"
#include "cpspi.h"
#include "core_api.h"
#include "profile.h"
#include <circle/machineinfo.h>
#include <circle/memory.h>
#include <circle/new.h>
#include <circle/util.h>
#include <circle/alloc.h>
#include <circle/bcmwatchdog.h>
#include <circle/usb/usb.h>
#include <stdarg.h>

extern "C" {
int snprintf (char *pBuffer, size_t nSize, const char *pFormat, ...);
int vsnprintf (char *pBuffer, size_t nSize, const char *pFormat, va_list Args);
void *cpspi_png_encode (const void *pRGB, int nWidth, int nHeight, size_t *pSize);
void cpspi_png_free (void *p);
uint8_t *cpspi_png_decode (const void *pData, size_t nSize, unsigned *pWidth, unsigned *pHeight);
void cpspi_png_free_rgb (void *p);
}

#define CPSPI_DIR	"SD:/cpspi"
#define ROMS_DIR	CPSPI_DIR "/roms"
#define NVRAM_DIR	CPSPI_DIR "/nvram"
#define SHOT_DIR	CPSPI_DIR "/shots"
#define DB_PATH		CPSPI_DIR "/rominfo.cps1"
#define NAMES_PATH	CPSPI_DIR "/zipname.cps1"
#define LOG_PATH	CPSPI_DIR "/LOG.TXT"
#define BENCH_PATH	CPSPI_DIR "/BENCH.TXT"
#define BENCH_FLAG	CPSPI_DIR "/BENCH"
#define PROFILE_PATH	CPSPI_DIR "/PROFILE.TXT"
#define LAST_PATH	CPSPI_DIR "/LAST.TXT"
#define CFG_PATH	CPSPI_DIR "/cpspi.cfg"
#define PADS_PATH	CPSPI_DIR "/pads.cfg"
#define OVERLAY_PATH	CPSPI_DIR "/overlay.png"

#ifndef BENCH_WARMUP
#define BENCH_WARMUP	1800		// 30 s of play before measuring
#endif
#ifndef BENCH_FRAMES
#define BENCH_FRAMES	3600		// then 60 s measured
#endif
#define BENCH_PROFILE_US 200		// profiler: one sample every 200 us
#define BENCH_SLOW_US	16000		// frames from 16 ms on count as slow
#define REPORT_FRAMES	3600		// one statistics line per minute of play
// the CPS1 frame: 262 lines of 512 pixel clocks at 8 MHz = 16768 us, 59.637 Hz;
// the HDMI mode in config.txt runs at that rate and the core emulates it
#define FRAME_US	16768
#define CPS1_FPS100	5964		// 59.64 fps

#define COLOR_TEXT	RGB (220, 220, 220)
#define COLOR_TITLE	RGB (255, 200, 0)
#define COLOR_OK	RGB (80, 230, 80)
#define COLOR_ERR	RGB (255, 70, 70)
#define COLOR_DIM	RGB (140, 140, 140)

CKernel *CKernel::s_pThis = 0;

enum { GmReset, GmSpeed, GmService, GmList, GmPower, GmCount };

// ---- C interface used by the shim and the core glue ------------------------

extern "C" void cpspi_log (const char *pFormat, ...)
{
	char Buffer[200];
	va_list Args;
	va_start (Args, pFormat);
	vsnprintf (Buffer, sizeof Buffer, pFormat, Args);
	va_end (Args);
	if (CKernel::Get ())
	{
		CKernel::Get ()->Log (Buffer, COLOR_TEXT);
	}
}

extern "C" void cpspi_fatal (int nCode, const char *pFormat, ...)
{
	char Buffer[200];
	va_list Args;
	va_start (Args, pFormat);
	vsnprintf (Buffer, sizeof Buffer, pFormat, Args);
	va_end (Args);
	CKernel::Get ()->Fatal (nCode, Buffer);
}

extern "C" uint64_t cpspi_ticks_us (void)
{
	return CTimer::GetClockTicks64 ();
}

extern "C" void cpspi_delay_us (unsigned nMicros)
{
	if (nMicros >= 1000)
	{
		CTimer::SimpleMsDelay (nMicros / 1000);
	}
	else
	{
		CTimer::SimpleusDelay (nMicros);
	}
}

// ---- the vblank: picture and sound out, controls in --------------------------
// The core calls CoreVBlank at its emulated vblank, inside core_frame: the
// picture and the sound of the frame are complete and the game's interrupt
// handler, which reads the input ports, has not run yet. The kernel queues
// the sound, draws the picture and flips it (the display takes it at the
// next vsync), then waits until the vsync is s_nInputMargin away and samples
// the pads (kept current by the USB interrupts) at that moment: the game
// reads controls as fresh as on the board, which reads its ports at its own
// vblank, and only the handler's few lines run before the next frame begins.
// There is no frame delay to tune: the slow part of the frame (the 68000
// and the picture) runs right after the previous vsync, the wait comes
// after it, and the controls go in at the end whatever the frame cost.
// The game loop decides per frame which pads are ignored (buttons still held
// from a menu) and the extra bits of the frame (coin pulse, TEST switch).

static CInput *s_pLateInput;
static volatile boolean s_bLateEnabled;		// RunGame only (not the benchmark)
static volatile boolean s_LateIgnore[CORE_MAX_PLAYERS];
static volatile u32 s_LateExtra[CORE_MAX_PLAYERS];
static boolean s_bLateChange;		// lag measurement: a pad report changed the
static unsigned s_nLateChangeTicks;	// controls sampled at this vblank (its time)
static unsigned s_nLastVSync;		// when the display's last vsync was seen
static unsigned s_nInputMargin = 2000;	// controls sampled this long before the vsync (us)
static unsigned s_nMarginBoost;		// added after vsyncs missed with the frame ready
static unsigned s_tEmuEnd, s_tFlip, s_tSample;	// this frame: picture rendered, flipped, pads sampled
static boolean s_bPresented;		// this frame went through CoreVBlank
static unsigned s_nLateHooks;		// frames whose picture was not ready by the sample time
static unsigned s_nMissUnexplained;	// vsyncs missed although the frame was ready in time
static u32 s_HVSAtVSync[3], s_HVSAtSample[3];	// HVS channel status (diagnostic)
static unsigned s_nReportLimit;		// frames per statistics window (short first window)
// The sound comes in four blocks per frame (lines 64, 128, 192, vblank).
// Each block's hook queues it and then holds the emulation until the block's
// deadline, so that the blocks are spread through the real frame and the
// audio queue can stay a few milliseconds deep; the deadlines come from the
// slowest block, picture and draw of the last second, with room to spare.
static unsigned s_BlockDeadline[3];	// us after the vsync; 0 = no pacing yet
static unsigned s_tBlockStart;		// when the current block's emulation began
static unsigned s_nBlockWorst;		// slowest of blocks 0-2 this second (us)
static unsigned s_nLastBlockWorst;	// slowest block 3 with the picture this second (us)
static unsigned s_nDrawWorst;		// slowest draw and flip this second (us)
static unsigned s_tLastAudio;		// when the last block was queued
static unsigned s_nGapMax, s_nGapMaxWin;	// longest wait between blocks (second, window)
static unsigned s_nBlocksLate;		// blocks queued after their deadline (window)
static unsigned s_nWaitUs;		// this frame's pacing waits (not emulation time)
static unsigned s_nCarried;		// frames that began late and were carried on (window)

// pad bits -> core flags; while Select is held only the joystick goes through
static u32 GameFlags (u32 n)
{
	u32 nGame = (n & BTN_SELECT) ? (n & 0x0F) : n;
	u32 f = 0;
	if (nGame & BTN_UP)	f |= CORE_IN_UP;
	if (nGame & BTN_DOWN)	f |= CORE_IN_DOWN;
	if (nGame & BTN_LEFT)	f |= CORE_IN_LEFT;
	if (nGame & BTN_RIGHT)	f |= CORE_IN_RIGHT;
	if (nGame & BTN_1)	f |= CORE_IN_B1;
	if (nGame & BTN_2)	f |= CORE_IN_B2;
	if (nGame & BTN_3)	f |= CORE_IN_B3;
	if (nGame & BTN_4)	f |= CORE_IN_B4;
	if (nGame & BTN_5)	f |= CORE_IN_B5;
	if (nGame & BTN_6)	f |= CORE_IN_B6;
	if (nGame & BTN_START)	f |= CORE_IN_START;
	return f;
}

void CKernel::CoreVBlank (void)
{
	if (!s_bLateEnabled || s_pLateInput == 0)
	{
		return;
	}
	s_tEmuEnd = CTimer::GetClockTicks ();
	unsigned nLastBlock = s_tEmuEnd - s_tBlockStart;	// block 3, picture included
	if (nLastBlock > s_nLastBlockWorst) s_nLastBlockWorst = nLastBlock;

	// the picture, out (the sound went block by block, the bands were
	// drawn before the frame)
	m_Video.DrawGame (core_video (), core_flipped ());
	m_Video.Flip ();
	s_tFlip = CTimer::GetClockTicks ();
	if (s_tFlip - s_tEmuEnd > s_nDrawWorst) s_nDrawWorst = s_tFlip - s_tEmuEnd;

	// the controls, as late as the vsync allows
	unsigned nTarget = s_nLastVSync + FRAME_US - s_nInputMargin;
	if ((int) (s_tFlip - nTarget) >= 0)
	{
		s_nLateHooks++;		// the frame cost more than the period minus the margin
	}
	while ((int) (CTimer::GetClockTicks () - nTarget) < 0)
	{
	}
	unsigned nTicks;
	if (s_pLateInput->TakeChange (&nTicks))
	{
		s_nLateChangeTicks = nTicks;
		s_bLateChange = TRUE;
	}
	for (unsigned p = 0; p < CORE_MAX_PLAYERS; p++)
	{
		if (s_LateIgnore[p])
		{
			core_set_input (p, 0);
		}
		else
		{
			core_set_input (p, GameFlags (s_pLateInput->Get (p)) | s_LateExtra[p]);
		}
	}
	s_tSample = CTimer::GetClockTicks ();
	for (unsigned c = 0; c < 3; c++)
	{
		s_HVSAtSample[c] = m_Video.ReadHVSStat (c);
	}
	s_bPresented = TRUE;
}

static void CoreVBlankHook (void)
{
	CKernel::Get ()->CoreVBlank ();
}

void CKernel::CoreAudioBlock (const int16_t *pSamples, unsigned nFrames, unsigned nBlock)
{
	if (!s_bLateEnabled)
	{
		return;
	}
	unsigned now = CTimer::GetClockTicks ();
	if (s_tLastAudio != 0)
	{
		unsigned nGap = now - s_tLastAudio;
		if (nGap > s_nGapMax) s_nGapMax = nGap;
		if (nGap > s_nGapMaxWin) s_nGapMaxWin = nGap;
	}
	s_tLastAudio = now;
	m_Audio.WriteFrame (pSamples, nFrames);
	if (nBlock >= 3)
	{
		return;			// CoreVBlank follows: the picture, then the controls
	}
	unsigned nBlockTime = now - s_tBlockStart;
	if (nBlockTime > s_nBlockWorst) s_nBlockWorst = nBlockTime;

	// hold the emulation until this block's deadline, so that the next
	// block's sound is made when the queue needs it, not long before
	unsigned nDeadline = s_BlockDeadline[nBlock];
	if (nDeadline)
	{
		unsigned nTarget = s_nLastVSync + nDeadline;
		if ((int) (CTimer::GetClockTicks () - nTarget) >= 0)
		{
			s_nBlocksLate++;
		}
		while ((int) (CTimer::GetClockTicks () - nTarget) < 0)
		{
		}
	}
	s_tBlockStart = CTimer::GetClockTicks ();
	s_nWaitUs += s_tBlockStart - now;
}

static void CoreAudioHook (const int16_t *pSamples, unsigned nFrames, unsigned nBlock)
{
	CKernel::Get ()->CoreAudioBlock (pSamples, nFrames, nBlock);
}

// ---- helpers --------------------------------------------------------------

static const char *AspectName (unsigned nAspect);

static void FormatMs (char *pOut, size_t nSize, unsigned nMicros)
{
	snprintf (pOut, nSize, "%u.%02u", nMicros / 1000, (nMicros % 1000) / 10);
}

struct TStats
{
	unsigned nAvg, nP99, nMax, nMin;
};

static void ComputeStats (const unsigned *pValues, unsigned nCount, TStats *pStats)
{
	static unsigned Sorted[BENCH_FRAMES];
	u64 nSum = 0;
	for (unsigned i = 0; i < nCount; i++)
	{
		Sorted[i] = pValues[i];
		nSum += pValues[i];
	}
	for (unsigned i = 1; i < nCount; i++)
	{
		unsigned v = Sorted[i];
		int j = (int) i - 1;
		while (j >= 0 && Sorted[j] > v)
		{
			Sorted[j+1] = Sorted[j];
			j--;
		}
		Sorted[j+1] = v;
	}
	pStats->nAvg = (unsigned) (nSum / nCount);
	pStats->nMin = Sorted[0];
	pStats->nMax = Sorted[nCount-1];
	pStats->nP99 = Sorted[(nCount * 99) / 100];
}

// Histogram in 0.1 ms bins (0..50 ms) for per-minute reports
struct THisto
{
	enum { Bins = 500 };
	unsigned Bin[Bins];
	unsigned nCount, nMax;
	u64 nSum;

	unsigned nMin;
	void Reset (void) { memset (Bin, 0, sizeof Bin); nCount = nMax = 0; nMin = ~0u; nSum = 0; }
	void Add (unsigned us)
	{
		if (us < nMin) nMin = us;
		unsigned b = us / 100;
		Bin[b < Bins ? b : Bins - 1]++;
		nCount++;
		nSum += us;
		if (us > nMax) nMax = us;
	}
	unsigned Avg (void) const { return nCount ? (unsigned) (nSum / nCount) : 0; }
	unsigned P99 (void) const
	{
		unsigned nTarget = nCount - nCount / 100, n = 0;
		for (unsigned b = 0; b < Bins; b++)
		{
			n += Bin[b];
			if (n >= nTarget) return b * 100 + 100;
		}
		return nMax;
	}
};

static THisto s_HistEmu, s_HistWork, s_HistPeriod;
static THisto s_HistRender;		// the core's own rendering of the picture (inside emu)
static THisto s_HistDraw;		// picture + sound to the framebuffer and flip
static THisto s_HistLag;		// pad report -> the vblank sample that used it
static THisto s_HistSlack;		// frame ready (flip) -> vsync
static THisto s_HistMargin;		// controls sampled -> vsync
static THisto s_HistPost;		// controls sampled -> core_frame returned
static THisto s_HistWait;		// pacing waits per frame (inside emu, taken out of it)
static unsigned s_nOverBudget, s_nMissedVSync, s_nWindowFrames;
static unsigned s_nWindowStart;

static boolean HasZipExtension (const char *pName)
{
	size_t n = strlen (pName);
	if (n < 4)
	{
		return FALSE;
	}
	const char *e = pName + n - 4;
	return    e[0] == '.'
	       && (e[1] == 'z' || e[1] == 'Z')
	       && (e[2] == 'i' || e[2] == 'I')
	       && (e[3] == 'p' || e[3] == 'P');
}

int CLogSink::Write (const void *pBuffer, size_t nCount)
{
	static char Line[160];
	static unsigned nPos = 0;
	const char *p = (const char *) pBuffer;
	for (size_t i = 0; i < nCount; i++)
	{
		char c = p[i];
		if (c == '\x1b')
		{
			while (i + 1 < nCount && !((p[i+1] >= 'a' && p[i+1] <= 'z') || (p[i+1] >= 'A' && p[i+1] <= 'Z')))
			{
				i++;
			}
			i++;
			continue;
		}
		if (c == '\n' || nPos >= sizeof Line - 1)
		{
			Line[nPos] = '\0';
			if (nPos > 0 && CKernel::Get ())
			{
				CKernel::Get ()->Log (Line, COLOR_DIM);
			}
			nPos = 0;
			continue;
		}
		if (c != '\r')
		{
			Line[nPos++] = c;
		}
	}
	return (int) nCount;
}

// ---- kernel ---------------------------------------------------------------

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_CPUThrottle (CPUSpeedMaximum),
	m_USBHCI (&m_Interrupt, &m_Timer, TRUE),
	m_EMMC (&m_Interrupt, &m_Timer, &m_ActLED),
	m_pInput (0),
	m_pUI (0),
	m_bVideoOK (FALSE),
	m_bSDOK (FALSE),
	m_bQuiet (TRUE),		// black screen while booting; text only on errors
	m_nRepeat (0),
	m_nLines (0),
	m_pGames (0),
	m_nGames (0),
	m_nLoaded (-1),
	m_pTitles (0),
	m_bVSync (TRUE),
	m_bStatusOn (FALSE),
	m_nClearStatus (0),
	m_nShots (0),
	m_nFilter (FILTER_SCAN_MEDIUM),
	m_bHVS (TRUE),
	m_nAspect (ASPECT_43),
	m_nInputMarginMin (0),
	m_bMultiCore (TRUE),
	m_bOverlay (TRUE),
	m_pOverlayRGB (0),
	m_nCpuPercent (0),
	m_nCpuSets (0)
{
	s_pThis = this;
	m_LastRaw[0] = '\0';
}

CKernel::~CKernel (void)
{
	s_pThis = 0;
}

boolean CKernel::Initialize (void)
{
	m_bVideoOK = m_Video.Initialize ();

	m_Serial.Initialize (115200);

	if (!m_Logger.Initialize (&m_LogSink))
	{
		return FALSE;
	}
	if (!m_Interrupt.Initialize ())
	{
		return FALSE;
	}
	if (!m_Timer.Initialize ())
	{
		return FALSE;
	}

	return TRUE;
}

// ---- console / log --------------------------------------------------------

void CKernel::Log (const char *pLine, u32 nColor)
{
	if (m_nLines > 0 && strcmp (pLine, m_LastRaw) == 0)
	{
		m_nRepeat++;
		unsigned n = Slot (m_nLines - 1);
		snprintf (m_Lines[n], MaxLineLen, "%s (x%u)", m_LastRaw, m_nRepeat);
		if (!m_bQuiet && m_bVideoOK && !m_Video.IsGameMode () && m_nLines <= m_Video.GetRows ())
		{
			m_Video.DrawText (0, 0, m_nLines - 1, m_Lines[n], nColor, RGB (0, 0, 0));
		}
		return;
	}
	strncpy (m_LastRaw, pLine, MaxLineLen - 1);
	m_LastRaw[MaxLineLen - 1] = '\0';
	m_nRepeat = 1;

	unsigned n = Slot (m_nLines);
	strncpy (m_Lines[n], pLine, MaxLineLen - 1);
	m_Lines[n][MaxLineLen - 1] = '\0';
	m_LineColor[n] = nColor;
	m_nLines++;

	if (m_bQuiet)
	{
		return;
	}

	m_Serial.Write (pLine, strlen (pLine));
	m_Serial.Write ("\r\n", 2);

	if (!m_bVideoOK || m_Video.IsGameMode ())	// never write text over the game screen
	{
		return;
	}

	unsigned nRows = m_Video.GetRows ();
	if (m_nLines <= nRows)
	{
		m_Video.DrawText (0, 0, m_nLines - 1, m_Lines[n], nColor, RGB (0, 0, 0));
	}
	else
	{
		RedrawConsole ();
	}
}

void CKernel::RedrawConsole (void)
{
	unsigned nRows = m_Video.GetRows ();
	unsigned nFirst = m_nLines > nRows ? m_nLines - nRows : 0;
	m_Video.ClearRows (0, 0, nRows, RGB (0, 0, 0));
	for (unsigned i = nFirst; i < m_nLines; i++)
	{
		unsigned n = Slot (i);
		m_Video.DrawText (0, 0, i - nFirst, m_Lines[n], m_LineColor[n], RGB (0, 0, 0));
	}
}

void CKernel::WriteTextFile (const char *pPath, const char *pText)
{
	if (!m_bSDOK)
	{
		return;
	}
	m_Video.WaitForFileJob ();		// core 3 may be writing the log
	FIL File;
	if (f_open (&File, pPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		return;
	}
	UINT nWritten;
	f_write (&File, pText, strlen (pText), &nWritten);
	f_close (&File);
}

void CKernel::SaveLog (boolean bBackground)
{
	static char Text[MaxLines * (MaxLineLen + 2) + 80];
	m_Video.WaitForFileJob ();		// the buffer may still be being written
	// the head, a note on what was not kept, then the tail
	const unsigned nRing = MaxLines - HeadLines;
	unsigned nTailFirst = m_nLines > HeadLines + nRing ? m_nLines - nRing : HeadLines;
	size_t nPos = 0;
	for (unsigned i = 0; i < m_nLines; i++)
	{
		if (i == HeadLines && nTailFirst > HeadLines)
		{
			nPos += snprintf (Text + nPos, 80, "(... %u lines not kept ...)\r\n", nTailFirst - HeadLines);
			i = nTailFirst;
		}
		const char *pLine = m_Lines[Slot (i)];
		size_t nLen = strlen (pLine);
		memcpy (Text + nPos, pLine, nLen);
		nPos += nLen;
		Text[nPos++] = '\r';
		Text[nPos++] = '\n';
	}
	Text[nPos] = '\0';
	if (bBackground && m_bSDOK && m_Video.PostFileJob (LOG_PATH, Text, nPos))
	{
		return;				// core 3 writes it while the game goes on
	}
	WriteTextFile (LOG_PATH, Text);
}

void CKernel::Fatal (int nCode, const char *pMessage)
{
	if (m_bQuiet || m_Video.IsGameMode ())
	{
		m_bQuiet = FALSE;
		if (m_bVideoOK && m_Video.SetConsoleMode ())
		{
			RedrawConsole ();
		}
	}

	char Line[160];
	snprintf (Line, sizeof Line, "ERROR: %s", pMessage);
	Log (Line, COLOR_ERR);

	switch (nCode)
	{
	case CPSPI_ERR_SD:
		Log ("  Check that the card is FAT32 and that the files are in its root.", COLOR_TEXT);
		break;
	case CPSPI_ERR_DB:
		Log ("  Copy rominfo.cps1 and zipname.cps1 into the cpspi folder.", COLOR_TEXT);
		break;
	case CPSPI_ERR_GAME:
		Log ("  Copy at least one supported CPS1 zip (MAME set) into cpspi/roms.", COLOR_TEXT);
		break;
	case CPSPI_ERR_MEM:
		Log ("  Not enough memory.", COLOR_TEXT);
		break;
	default:
		break;
	}
	snprintf (Line, sizeof Line, "The green LED blinks %d times: error code %d.", nCode, nCode);
	Log (Line, COLOR_TEXT);

	SaveLog ();

	while (1)
	{
		m_ActLED.Blink (nCode, 250, 350);
		CTimer::SimpleMsDelay (1500);
	}
}

// ---- files ----------------------------------------------------------------

// a whole text file, zero terminated (free with delete [])
char *CKernel::LoadText (const char *pPath, size_t *pSize)
{
	FIL File;
	if (f_open (&File, pPath, FA_READ) != FR_OK)
	{
		return 0;
	}
	size_t nSize = f_size (&File);
	char *pBuffer = new char[nSize + 1];
	if (pBuffer == 0)
	{
		f_close (&File);
		return 0;
	}
	UINT nRead = 0;
	f_read (&File, pBuffer, nSize, &nRead);
	f_close (&File);
	pBuffer[nRead] = '\0';
	*pSize = nRead;
	return pBuffer;
}

// "name,Title" lines of zipname.cps1
const char *CKernel::TitleOf (const char *pName) const
{
	static char Title[64];
	if (m_pTitles == 0)
	{
		return 0;
	}
	size_t nLen = strlen (pName);
	const char *p = m_pTitles;
	while (*p)
	{
		const char *pEnd = p;
		while (*pEnd && *pEnd != '\n') pEnd++;
		if (   (size_t) (pEnd - p) > nLen + 1
		    && strncasecmp (p, pName, nLen) == 0 && p[nLen] == ',')
		{
			const char *t = p + nLen + 1;
			unsigned n = 0;
			while (t < pEnd && *t != '\r' && n < sizeof Title - 1)
			{
				char c = *t++;
				if (c >= 'a' && c <= 'z') c -= 32;
				if (c < 32 || c > 126) c = ' ';
				Title[n++] = c;
			}
			while (n > 0 && Title[n-1] == ' ') n--;
			Title[n] = '\0';
			return n ? Title : 0;
		}
		p = *pEnd ? pEnd + 1 : pEnd;
	}
	return 0;
}

// The zips in cpspi/roms that the core's database knows (FILENAME( name,
// parent, ... ) lines of rominfo.cps1); a clone needs its parent zip too.
void CKernel::ScanGames (void)
{
	m_pGames = new TGameEntry[MaxGames];
	m_nGames = 0;

	size_t nSize = 0;
	char *pDB = LoadText (DB_PATH, &nSize);
	if (pDB == 0)
	{
		Fatal (CPSPI_ERR_DB, "cpspi/rominfo.cps1 not found or unreadable");
	}
	m_pTitles = LoadText (NAMES_PATH, &nSize);
	if (m_pTitles == 0)
	{
		Log ("  zipname.cps1 not found: the list shows the file names", COLOR_ERR);
	}

	// supported sets and their parents
	struct TSet { char Name[16]; char Parent[16]; };
	static TSet Sets[512];
	unsigned nSets = 0;
	for (const char *p = pDB; *p && nSets < 512; )
	{
		const char *pEnd = p;
		while (*pEnd && *pEnd != '\n') pEnd++;
		if (strncmp (p, "FILENAME(", 9) == 0)
		{
			const char *q = p + 9;
			char *pOut[2] = {Sets[nSets].Name, Sets[nSets].Parent};
			boolean bOK = TRUE;
			for (int f = 0; f < 2 && bOK; f++)
			{
				while (q < pEnd && *q == ' ') q++;
				unsigned n = 0;
				while (q < pEnd && *q != ',' && *q != ' ' && *q != ')' && n < 15)
				{
					pOut[f][n++] = *q++;
				}
				pOut[f][n] = '\0';
				while (q < pEnd && *q != ',') q++;
				if (q < pEnd) q++;
				if (n == 0) bOK = FALSE;
			}
			if (bOK) nSets++;
		}
		p = *pEnd ? pEnd + 1 : pEnd;
	}
	delete [] pDB;
	char Line[160];
	snprintf (Line, sizeof Line, "Database: %u CPS1 sets", nSets);
	Log (Line, COLOR_TEXT);

	DIR Dir;
	FILINFO Info;
	if (f_opendir (&Dir, ROMS_DIR) != FR_OK)
	{
		return;
	}
	while (   m_nGames < MaxGames
	       && f_readdir (&Dir, &Info) == FR_OK
	       && Info.fname[0] != '\0')
	{
		if ((Info.fattrib & AM_DIR) || !HasZipExtension (Info.fname)
		    || strlen (Info.fname) - 4 >= sizeof m_pGames[0].File)
		{
			continue;
		}
		char Name[16];
		size_t n = strlen (Info.fname) - 4;
		for (size_t i = 0; i < n; i++)
		{
			char c = Info.fname[i];
			Name[i] = (c >= 'A' && c <= 'Z') ? (char) (c + 32) : c;
		}
		Name[n] = '\0';

		const TSet *pSet = 0;
		for (unsigned i = 0; i < nSets; i++)
		{
			if (strcmp (Sets[i].Name, Name) == 0)
			{
				pSet = &Sets[i];
				break;
			}
		}
		if (pSet == 0)
		{
			snprintf (Line, sizeof Line, "  %s: not a supported CPS1 set, skipped", Info.fname);
			Log (Line, COLOR_ERR);
			continue;
		}
		if (strcmp (pSet->Parent, "cps1") != 0)
		{
			char Path[128];
			FILINFO PInfo;
			snprintf (Path, sizeof Path, "%s/%s.zip", ROMS_DIR, pSet->Parent);
			if (f_stat (Path, &PInfo) != FR_OK)
			{
				snprintf (Line, sizeof Line, "  %s: clone of %s, %s.zip missing, skipped",
					  Info.fname, pSet->Parent, pSet->Parent);
				Log (Line, COLOR_ERR);
				continue;
			}
		}

		TGameEntry &G = m_pGames[m_nGames];
		strcpy (G.File, Name);
		const char *pTitle = TitleOf (Name);
		if (pTitle)
		{
			strncpy (G.Name, pTitle, sizeof G.Name - 1);
			G.Name[sizeof G.Name - 1] = '\0';
		}
		else
		{
			unsigned k = 0;
			for (; Name[k] && k < sizeof G.Name - 1; k++)
			{
				char c = Name[k];
				G.Name[k] = (c >= 'a' && c <= 'z') ? (char) (c - 32) : c;
			}
			G.Name[k] = '\0';
		}
		m_nGames++;
	}
	f_closedir (&Dir);

	// sort by title
	for (unsigned i = 1; i < m_nGames; i++)
	{
		TGameEntry t = m_pGames[i];
		int j = (int) i - 1;
		while (j >= 0 && strcmp (m_pGames[j].Name, t.Name) > 0)
		{
			m_pGames[j+1] = m_pGames[j];
			j--;
		}
		m_pGames[j+1] = t;
	}
}

// ---- core -----------------------------------------------------------------

void CKernel::UnloadGame (void)
{
	if (m_nLoaded < 0)
	{
		return;
	}
	core_unload ();			// writes the EEPROM contents to cpspi/nvram
	m_nLoaded = -1;
}

boolean CKernel::LoadGame (unsigned nIndex)
{
	UnloadGame ();

	const TGameEntry &G = m_pGames[nIndex];
	char Line[180];
	snprintf (Line, sizeof Line, "Game: %s (%s.zip)", G.Name, G.File);
	Log (Line, COLOR_TITLE);

	if (m_bQuiet && m_pUI)
	{
		m_pUI->DrawLoading (G.Name, 0);
		ShowUIFrame (FALSE);
	}

	unsigned nStart = CTimer::GetClockTicks ();
	core_set_cpu_percent (CpuPercentFor (G.File));	// 0: the core's default for the game
	if (!core_load (G.File))
	{
		snprintf (Line, sizeof Line, "  load failed: %s", core_error ());
		Log (Line, COLOR_ERR);
		return FALSE;
	}
	unsigned nMs = (CTimer::GetClockTicks () - nStart) / 1000;
	unsigned nPercent = core_cpu_percent ();
	unsigned nEff = core_m68k_mhz () * 100 * nPercent / 100;	// in 1/100 MHz
	snprintf (Line, sizeof Line, "  loaded in %u ms: %u players, %u buttons, 68000 at %u MHz run at %u%% (%u.%02u MHz, the wait states), %s%s", nMs,
		  core_players (), core_buttons (), core_m68k_mhz (), nPercent, nEff / 100, nEff % 100,
		  core_qsound () ? "QSound" : "YM2151 + OKI", core_vertical () ? ", vertical screen" : "");
	Log (Line, COLOR_DIM);
	m_nLoaded = (int) nIndex;
	m_nShots = 0;					// screenshot numbers: next free one

	WriteTextFile (LAST_PATH, G.File);
	return TRUE;
}

// ---- video helpers ----------------------------------------------------------

void CKernel::ShowUIFrame (boolean bWait, const char *pTopMessage)
{
	m_Video.DrawGame (m_pUI->GetBuffer ());
	if (pTopMessage && pTopMessage[0])
	{
		// in the black band above the picture, like the in-game messages
		const CVideoOut::TTextSpan Span = {pTopMessage, UI_ERR};
		m_Video.DrawBandText (TRUE, &Span, 1);
	}
	else
	{
		m_Video.DrawBandText (TRUE, 0, 0);	// no status line or message over menus
	}
	m_Video.DrawBandText (FALSE, 0, 0);
	m_Video.Flip ();
	if (!bWait)
	{
		return;
	}
	if (m_bVSync)
	{
		m_Video.WaitForVSync ();
	}
	else
	{
		CTimer::SimpleusDelay (FRAME_US);
	}
}

// Select+1: the original CPS1 picture (384x224, no filter) as
// cpspi/shots/<game>_NNN.png; returns the file name for the message, or 0
const char *CKernel::SaveScreenshot (void)
{
	static char Name[80];
	if (m_nLoaded < 0)
	{
		return 0;
	}
	const char *pBase = m_pGames[m_nLoaded].File;
	char Path[160];

	// next free number (the search starts after the last one used)
	FILINFO Info;
	for (;;)
	{
		if (++m_nShots > 999)
		{
			return 0;
		}
		snprintf (Name, sizeof Name, "%s_%03u.png", pBase, m_nShots);
		snprintf (Path, sizeof Path, "%s/%s", SHOT_DIR, Name);
		if (f_stat (Path, &Info) != FR_OK)
		{
			break;
		}
	}

	static u8 RGB[GAME_W * GAME_H * 3];
	const u16 *pSrc = core_video ();
	boolean bFlip = core_flipped ();
	u8 *d = RGB;
	for (unsigned y = 0; y < GAME_H; y++)
	{
		const u16 *s = pSrc + (GAME_SRC_Y + (bFlip ? GAME_H - 1 - y : y)) * GAME_SRC_PITCH + GAME_SRC_X;
		for (unsigned x = 0; x < GAME_W; x++)
		{
			u16 p = s[bFlip ? GAME_W - 1 - x : x];
			unsigned r5 = p & 0x1F, g5 = (p >> 5) & 0x1F, b5 = (p >> 10) & 0x1F;
			*d++ = (u8) ((r5 << 3) | (r5 >> 2));
			*d++ = (u8) ((g5 << 3) | (g5 >> 2));
			*d++ = (u8) ((b5 << 3) | (b5 >> 2));
		}
	}
	size_t nSize = 0;
	void *pPng = cpspi_png_encode (RGB, GAME_W, GAME_H, &nSize);
	if (pPng == 0)
	{
		return 0;
	}
	FIL File;
	boolean bOK = FALSE;
	m_Video.WaitForFileJob ();
	if (f_open (&File, Path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK)
	{
		UINT nWritten = 0;
		bOK = f_write (&File, pPng, nSize, &nWritten) == FR_OK && nWritten == nSize;
		f_close (&File);
	}
	cpspi_png_free (pPng);

	char Line[200];
	snprintf (Line, sizeof Line, "  screenshot %s: %s", Name, bOK ? "saved" : "FAILED");
	Log (Line, bOK ? COLOR_TEXT : COLOR_ERR);
	return bOK ? Name : 0;
}

// ---- benchmark -------------------------------------------------------------

// An empty cpspi/BENCH file: 30 s of the game's demo, then 60 s measured at
// full speed, with the sampling profiler; results in BENCH.TXT / PROFILE.TXT
void CKernel::Benchmark (void)
{
	static unsigned Emu[BENCH_FRAMES], Draw[BENCH_FRAMES], Total[BENCH_FRAMES], Render[BENCH_FRAMES];
	u64 nSamples = 0;

	boolean bQuiet = m_bQuiet;
	ApplyVideoMode ();		// measure the real drawing path (filter, HVS)
	m_bQuiet = TRUE;

	auto Progress = [this] (const char *pPhase, unsigned nFrame, unsigned nFrames)
	{
		char Text[64];
		snprintf (Text, sizeof Text, "BENCHMARK  %s %u/%u S", pPhase,
			  nFrame / 60, nFrames / 60);
		const CVideoOut::TTextSpan Span = {Text, UI_WHITE};
		m_Video.DrawBandText (TRUE, &Span, 1);
		m_Video.Flip ();
	};

	Log ("Benchmark: warm-up", COLOR_TEXT);
	SaveLog ();

	s_bLateEnabled = FALSE;
	for (unsigned p = 0; p < CORE_MAX_PLAYERS; p++) core_set_input (p, 0);
	for (unsigned i = 0; i < BENCH_WARMUP; i++)
	{
		core_frame ();
		m_Video.DrawGame (core_video (), core_flipped ());
		if (i % 60 == 0)
		{
			Progress ("WARM-UP", i, BENCH_WARMUP);
		}
	}

	Log ("Benchmark: measuring", COLOR_TEXT);
	SaveLog ();

	CProfiler *pProfiler = new CProfiler (&m_Interrupt);
	boolean bProfile = pProfiler && pProfiler->Start (BENCH_PROFILE_US);

	enum { Worst = 10 };
	unsigned WorstIdx[Worst];
	unsigned nWorst = 0;
	unsigned nSlow = 0, nOver = 0;
	u32 nAllHash = 2166136261U;

	for (unsigned i = 0; i < BENCH_FRAMES; i++)
	{
		if (bProfile) pProfiler->BeginFrame ();
		unsigned t0 = CTimer::GetClockTicks ();
		core_frame ();
		unsigned t1 = CTimer::GetClockTicks ();
		m_Video.DrawGame (core_video (), core_flipped ());
		unsigned t2 = CTimer::GetClockTicks ();
		boolean bSlow = t2 - t0 >= BENCH_SLOW_US;
		if (bProfile) pProfiler->EndFrame (bSlow);

		const u16 *pSrc = core_video ();
		for (unsigned y = 0; y < GAME_H; y++)
		{
			const u16 *p = &pSrc[(y + GAME_SRC_Y) * GAME_SRC_PITCH + GAME_SRC_X];
			for (unsigned x = 0; x < GAME_W; x++)
			{
				nAllHash = (nAllHash ^ p[x]) * 16777619U;
			}
		}
		if (i % 60 == 0)
		{
			Progress ("MEASURING", i, BENCH_FRAMES);
		}

		Emu[i] = t1 - t0;
		Draw[i] = t2 - t1;
		Total[i] = t2 - t0;
		Render[i] = core_render_us ();
		const int16_t *pAudio;
		nSamples += core_audio (&pAudio);
		if (bSlow) nSlow++;
		if (Total[i] > FRAME_US) nOver++;

		unsigned k;
		if (nWorst < Worst)
		{
			k = nWorst++;
		}
		else if (Total[i] > Total[WorstIdx[Worst - 1]])
		{
			k = Worst - 1;
		}
		else
		{
			continue;
		}
		while (k > 0 && Total[WorstIdx[k-1]] < Total[i])
		{
			WorstIdx[k] = WorstIdx[k-1];
			k--;
		}
		WorstIdx[k] = i;
	}
	if (bProfile) pProfiler->Stop ();

	m_bQuiet = bQuiet;
	m_Video.SetConsoleMode ();
	RedrawConsole ();

	TStats E, D, T, R;
	ComputeStats (Emu, BENCH_FRAMES, &E);
	ComputeStats (Draw, BENCH_FRAMES, &D);
	ComputeStats (Total, BENCH_FRAMES, &T);
	ComputeStats (Render, BENCH_FRAMES, &R);

	char a[16], b[16], c[16], d[16];
	char Report[4096];
	size_t nPos = 0;
	char Line[160];

	CMachineInfo *pInfo = CMachineInfo::Get ();
	unsigned nSpeed = T.nAvg ? (unsigned) ((u64) FRAME_US * 100 / T.nAvg) : 0;

	#define REPORT(color) do { Log (Line, color); \
		nPos += snprintf (Report + nPos, sizeof Report - nPos, "%s\r\n", Line); } while (0)

	snprintf (Line, sizeof Line, "cps-pi %s - %s, %u MB, CPU %u MHz, %u C",
		  CPSPI_VERSION, pInfo->GetMachineName (), pInfo->GetRAMSize (),
		  m_CPUThrottle.GetClockRate () / 1000000, m_CPUThrottle.GetTemperature ());
	REPORT (COLOR_TEXT);
	snprintf (Line, sizeof Line, "Game: %s (measured after %u s of play, for %u s)",
		  m_pGames[m_nLoaded].File, BENCH_WARMUP / 60, BENCH_FRAMES / 60);
	REPORT (COLOR_TEXT);
	char Tag[8];
	VideoModeTag (Tag, sizeof Tag);
	snprintf (Line, sizeof Line, "Screen: mode %s, %ux%u, scale %ux (HVS %ux, CPU %ux), filter %s",
		  Tag, m_Video.GetWidth (), m_Video.GetHeight (), m_Video.GetScale (),
		  m_Video.GetHVSFactor (), m_Video.GetScale () / m_Video.GetHVSFactor (),
		  FilterName (m_nFilter));
	REPORT (COLOR_TEXT);
	snprintf (Line, sizeof Line, "Frame times (ms)            avg       p99     max     min");
	REPORT (COLOR_TITLE);
	FormatMs (a, sizeof a, E.nAvg); FormatMs (b, sizeof b, E.nP99);
	FormatMs (c, sizeof c, E.nMax); FormatMs (d, sizeof d, E.nMin);
	snprintf (Line, sizeof Line, "  emulation + audio       %7s %7s %7s %7s", a, b, c, d);
	REPORT (COLOR_TEXT);
	FormatMs (a, sizeof a, R.nAvg); FormatMs (b, sizeof b, R.nP99);
	FormatMs (c, sizeof c, R.nMax); FormatMs (d, sizeof d, R.nMin);
	snprintf (Line, sizeof Line, "    of which the picture  %7s %7s %7s %7s", a, b, c, d);
	REPORT (COLOR_TEXT);
	FormatMs (a, sizeof a, D.nAvg); FormatMs (b, sizeof b, D.nP99);
	FormatMs (c, sizeof c, D.nMax); FormatMs (d, sizeof d, D.nMin);
	snprintf (Line, sizeof Line, "  drawing to HDMI (x%u)   %7s %7s %7s %7s", m_Video.GetDrawCores (), a, b, c, d);
	REPORT (COLOR_TEXT);
	FormatMs (a, sizeof a, T.nAvg); FormatMs (b, sizeof b, T.nP99);
	FormatMs (c, sizeof c, T.nMax); FormatMs (d, sizeof d, T.nMin);
	snprintf (Line, sizeof Line, "  total                   %7s %7s %7s %7s", a, b, c, d);
	REPORT (COLOR_TEXT);
	snprintf (Line, sizeof Line, "Top speed: %u%% of a real CPS1 (100%% = 59.64 fps)", nSpeed);
	REPORT (nSpeed >= 150 ? COLOR_OK : COLOR_ERR);
	snprintf (Line, sizeof Line, "Frames over 16.7 ms (too slow for full speed): %u of %u (%u.%u%%)",
		  nOver, BENCH_FRAMES, nOver * 100 / BENCH_FRAMES, (nOver * 1000 / BENCH_FRAMES) % 10);
	REPORT (nOver ? COLOR_ERR : COLOR_OK);
	snprintf (Line, sizeof Line, "Slowest frames (second.frame: emulation + drawing ms):");
	REPORT (COLOR_TITLE);
	for (unsigned w = 0; w < nWorst; w += 2)
	{
		size_t n = 0;
		Line[0] = 0;
		for (unsigned j = w; j < w + 2 && j < nWorst; j++)
		{
			unsigned f = WorstIdx[j];
			FormatMs (a, sizeof a, Emu[f]); FormatMs (b, sizeof b, Draw[f]);
			n += snprintf (Line + n, sizeof Line - n, "  %3u.%02u: %s + %s",
				       f / 60, f % 60, a, b);
		}
		REPORT (COLOR_TEXT);
	}
	snprintf (Line, sizeof Line, "Audio: %u stereo samples per frame at %u Hz",
		  (unsigned) (nSamples / BENCH_FRAMES), CORE_AUDIO_RATE);
	REPORT (COLOR_DIM);
	snprintf (Line, sizeof Line, "Check: all %u measured frames = %08X", BENCH_FRAMES, nAllHash);
	REPORT (COLOR_DIM);
	if (bProfile)
	{
		snprintf (Line, sizeof Line, "Profile: %u samples (%u in frames over %u ms) -> PROFILE.TXT",
			  pProfiler->GetTotal (), pProfiler->GetSlow (), BENCH_SLOW_US / 1000);
	}
	else
	{
		snprintf (Line, sizeof Line, "Profile: not available");
	}
	REPORT (COLOR_DIM);

	WriteTextFile (BENCH_PATH, Report);
	if (bProfile)
	{
		const size_t nSize = 1024 * 1024;
		char *pText = new char[nSize];
		if (pText)
		{
			pProfiler->Format (pText, nSize);
			WriteTextFile (PROFILE_PATH, pText);
			delete [] pText;
		}
	}
	delete pProfiler;
	Log ("Results saved to cpspi/BENCH.TXT and cpspi/PROFILE.TXT", COLOR_OK);
	SaveLog ();

	core_reset ();		// play from a clean boot
}

// ---- statistics -------------------------------------------------------------

void CKernel::ReportWindow (boolean bFinal)
{
	if (s_nWindowFrames < 60)
	{
		return;
	}
	unsigned nElapsed = CTimer::GetClockTicks () - s_nWindowStart;
	unsigned nFPS100 = (unsigned) ((u64) s_nWindowFrames * 100 * 1000000 / nElapsed);
	char ea[16], ep[16], em[16], ra[16], wa[16], wp[16], da[16], pm[16];
	FormatMs (ea, sizeof ea, s_HistEmu.Avg ());
	FormatMs (ep, sizeof ep, s_HistEmu.P99 ());
	FormatMs (em, sizeof em, s_HistEmu.nMax);
	FormatMs (ra, sizeof ra, s_HistRender.Avg ());
	FormatMs (wa, sizeof wa, s_HistWork.Avg ());
	FormatMs (wp, sizeof wp, s_HistWork.P99 ());
	FormatMs (da, sizeof da, s_HistDraw.Avg ());
	FormatMs (pm, sizeof pm, s_HistPeriod.nMax);
	unsigned qmin, qmax, und;
	int adj;
	m_Audio.GetStats (&qmin, &qmax, &und, &adj);

	char Tag[8];
	VideoModeTag (Tag, sizeof Tag);
	char Line[MaxLineLen];
	snprintf (Line, sizeof Line,
		  "[%s] %u frames, FPS %u.%02u | emu %s/%s/%s ms (picture %s) | emu+draw %s/%s ms (>16ms: %u) | draw x%u %s ms",
		  bFinal ? "partial" : "1 min", s_nWindowFrames, nFPS100 / 100, nFPS100 % 100,
		  ea, ep, em, ra, wa, wp, s_nOverBudget, m_Video.GetDrawCores (), da);
	Log (Line, COLOR_TEXT);
	snprintf (Line, sizeof Line,
		  "   video %s x%u %s %s | max period %s ms, missed vsyncs %u | audio %u-%u, underruns %u, adj %d | %u MHz %u C",
		  Tag, m_Video.GetScale (), m_Video.IsAspect43 () ? "4:3" : "SQ", FilterShort (m_Video.GetFilter ()),
		  pm, s_nMissedVSync, qmin, qmax, und, adj,
		  m_CPUThrottle.GetClockRate () / 1000000, m_CPUThrottle.GetTemperature ());
	Log (Line, COLOR_TEXT);

	if (s_HistMargin.nCount > 0)
	{
		char ma[16], mn[16], mx[16], px[16], sa[16], sm[16];
		FormatMs (ma, sizeof ma, s_HistMargin.Avg ());
		FormatMs (mn, sizeof mn, s_HistMargin.nMin);
		FormatMs (mx, sizeof mx, s_HistMargin.nMax);
		FormatMs (px, sizeof px, s_HistPost.nMax);
		FormatMs (sa, sizeof sa, s_HistSlack.Avg ());
		FormatMs (sm, sizeof sm, s_HistSlack.nMin == ~0u ? 0 : s_HistSlack.nMin);
		snprintf (Line, sizeof Line,
			  "   controls %s/%s/%s ms before vsync (margin %u.%u, after %s) | "
			  "picture %s/%s ms before | late %u, carried %u, missed ready %u",
			  ma, mn, mx, s_nInputMargin / 1000, (s_nInputMargin % 1000) / 100, px, sa, sm,
			  s_nLateHooks, s_nCarried, s_nMissUnexplained);
		Log (Line, COLOR_TEXT);
		char ga[16], d1[16], d2[16], d3[16], wa[16];
		FormatMs (ga, sizeof ga, s_nGapMaxWin);
		FormatMs (d1, sizeof d1, s_BlockDeadline[0]);
		FormatMs (d2, sizeof d2, s_BlockDeadline[1]);
		FormatMs (d3, sizeof d3, s_BlockDeadline[2]);
		FormatMs (wa, sizeof wa, s_HistWait.Avg ());
		snprintf (Line, sizeof Line,
			  "   sound: gap %s ms max, deadlines %s/%s/%s, late %u, queue %u | waits %s ms avg | "
			  "HVS ch %d, line %u at controls",
			  ga, d1, d2, d3, s_nBlocksLate, m_Audio.GetTarget (), wa, m_Video.GetHVSChannel (),
			  s_HVSAtSample[1] & 0xFFF);
		Log (Line, COLOR_TEXT);
	}
	if (s_HistLag.nCount > 0)
	{
		char la[16], lm[16], lx[16];
		FormatMs (la, sizeof la, s_HistLag.Avg ());
		FormatMs (lm, sizeof lm, s_HistLag.nMin);
		FormatMs (lx, sizeof lx, s_HistLag.nMax);
		snprintf (Line, sizeof Line,
			  "   lag: %u inputs, pad report -> read by the game %s/%s/%s ms avg/min/max "
			  "(+ one frame to the screen, as on the board)",
			  s_HistLag.nCount, la, lm, lx);
		Log (Line, COLOR_TEXT);
	}

	s_HistEmu.Reset ();
	s_HistRender.Reset ();
	s_HistWork.Reset ();
	s_HistDraw.Reset ();
	s_HistPeriod.Reset ();
	s_HistLag.Reset ();
	s_HistSlack.Reset ();
	s_HistMargin.Reset ();
	s_HistPost.Reset ();
	s_HistWait.Reset ();
	s_nOverBudget = s_nMissedVSync = s_nWindowFrames = 0;
	s_nLateHooks = s_nMissUnexplained = s_nBlocksLate = s_nGapMaxWin = s_nCarried = 0;
	s_nWindowStart = CTimer::GetClockTicks ();
}

// ---- configuration, system menu, power off ---------------------------------

const char *CKernel::FilterShort (unsigned nFilter)
{
	switch (nFilter)
	{
	case FILTER_SCAN_LIGHT:	 return "SCAN-L";
	case FILTER_SCAN_MEDIUM: return "SCAN-M";
	case FILTER_SCAN_STRONG: return "SCAN-S";
	default:		 return "NOFLT";
	}
}

const char *CKernel::FilterName (unsigned nFilter)
{
	switch (nFilter)
	{
	case FILTER_SCAN_LIGHT:	 return "SCANLINES_LIGHT";
	case FILTER_SCAN_MEDIUM: return "SCANLINES";
	case FILTER_SCAN_STRONG: return "SCANLINES_STRONG";
	default:		 return "NONE";
	}
}

// "HVS4", "HVS2" or "CPU": how the picture is being enlarged right now
void CKernel::VideoModeTag (char *pOut, size_t nSize)
{
	if (m_Video.GetHVSFactor () > 1)
	{
		snprintf (pOut, nSize, "HVS%u", m_Video.GetHVSFactor ());
	}
	else
	{
		snprintf (pOut, nSize, "CPU");
	}
}

void CKernel::ApplyVideoMode (void)
{
	if (!m_Video.SetGameMode (m_nFilter, m_bHVS, m_nAspect))
	{
		Fatal (CPSPI_ERR_FB, "cannot set up the game framebuffer");
	}
	char Tag[8];
	VideoModeTag (Tag, sizeof Tag);
	char Line[MaxLineLen];
	snprintf (Line, sizeof Line, "Video: mode %s, HDMI %ux%u, framebuffer %ux%u, picture %ux%u (%s), vertical %ux (HVS %ux, CPU %ux), filter %s%s",
		  Tag, m_Video.GetWidth (), m_Video.GetHeight (),
		  m_Video.GetFBWidth (), m_Video.GetFBHeight (),
		  m_Video.GetScreenGameW (), m_Video.GetScreenGameH (), AspectName (m_Video.GetAspect ()),
		  m_Video.GetScale (), m_Video.GetHVSFactor (), m_Video.GetCPUScaleY (),
		  FilterName (m_Video.GetFilter ()), m_bHVS ? "" : " (scaler=cpu)");
	Log (Line, COLOR_TEXT);
	snprintf (Line, sizeof Line, "Video: pixel order %s (%u), red in the %s byte",
		  m_Video.GetPixelOrder () ? "RGB" : "BGR", m_Video.GetPixelOrder (),
		  m_Video.IsRedLow () ? "low" : "high");
	Log (Line, COLOR_TEXT);
}

// cpspi.cfg value and menu label of a picture shape
static const char *AspectName (unsigned nAspect)
{
	switch (nAspect)
	{
	case ASPECT_43_SHARP:	return "4:3sharp";
	case ASPECT_PIXEL:	return "square";
	default:		return "4:3";
	}
}

void CKernel::LoadConfig (void)
{
	FIL File;
	char Text[256];
	UINT nRead = 0;
	if (f_open (&File, CFG_PATH, FA_READ) != FR_OK)
	{
		return;
	}
	f_read (&File, Text, sizeof Text - 1, &nRead);
	f_close (&File);
	Text[nRead] = '\0';

	if (strstr (Text, "filter=NONE"))			m_nFilter = FILTER_NONE;
	else if (strstr (Text, "filter=SCANLINES_LIGHT"))	m_nFilter = FILTER_SCAN_LIGHT;
	else if (strstr (Text, "filter=SCANLINES_STRONG"))	m_nFilter = FILTER_SCAN_STRONG;
	else if (strstr (Text, "filter=SCANLINES"))		m_nFilter = FILTER_SCAN_MEDIUM;
	if (strstr (Text, "scaler=cpu"))	m_bHVS = FALSE;
	// aspect=4:3 (default, the arcade monitor, columns blended), 4:3sharp
	// (nearest column) or square (integer scale in both directions)
	if (strstr (Text, "aspect=square"))		m_nAspect = ASPECT_PIXEL;
	else if (strstr (Text, "aspect=4:3sharp"))	m_nAspect = ASPECT_43_SHARP;
	else if (strstr (Text, "aspect=4:3"))		m_nAspect = ASPECT_43;
	// inputmargin=N (milliseconds, one decimal allowed, e.g. 2.5): the
	// controls are read at least this long before the vsync (unless set:
	// 1.0 with the HVS line counter, 2.0 with the firmware's notification;
	// it also grows by itself after vsyncs missed with the frame ready)
	const char *pIM = strstr (Text, "inputmargin=");
	if (pIM)
	{
		pIM += 12;
		unsigned nUs = (unsigned) atoi (pIM) * 1000;
		while (*pIM >= '0' && *pIM <= '9') pIM++;
		if (*pIM == '.' && pIM[1] >= '0' && pIM[1] <= '9') nUs += (pIM[1] - '0') * 100;
		if (nUs >= 300 && nUs <= 8000) m_nInputMarginMin = nUs;
	}
	// multicore=off: core 0 draws the picture alone (cores 1-3 not started)
	if (strstr (Text, "multicore=off"))	m_bMultiCore = FALSE;
	// overlay=off: black around the picture even if cpspi/overlay.png exists
	if (strstr (Text, "overlay=off"))	m_bOverlay = FALSE;
	// cpu=NN: the 68000 runs at NN% of its clock in every game (the wait
	// states of the board, which are not emulated); cpu.<set>=NN per set
	for (const char *p = Text; (p = strstr (p, "cpu")) != 0; p++)
	{
		if (p != Text && p[-1] != '\n')
		{
			continue;
		}
		if (p[3] == '=')
		{
			unsigned n = (unsigned) atoi (p + 4);
			if (n >= 30 && n <= 150) m_nCpuPercent = n;
		}
		else if (p[3] == '.')
		{
			char Set[16];
			unsigned i = 0;
			const char *q = p + 4;
			while (*q && *q != '=' && *q != '\r' && *q != '\n' && i < sizeof Set - 1)
			{
				Set[i++] = *q++;
			}
			Set[i] = '\0';
			if (*q == '=')
			{
				unsigned n = (unsigned) atoi (q + 1);
				if (n >= 30 && n <= 150) SetCpuPercentFor (Set, n);
			}
		}
	}
	// USB pads are polled every 1 ms (like usbhid.jspoll=1 on Linux);
	// usbpoll=native keeps the interval requested by the device
	if (strstr (Text, "usbpoll=native"))	g_nUSBInterruptPollMs = 0;

	char Line[140];
	char Margin[16] = "auto";
	if (m_nInputMarginMin)
	{
		snprintf (Margin, sizeof Margin, "%u.%u ms", m_nInputMarginMin / 1000, (m_nInputMarginMin % 1000) / 100);
	}
	snprintf (Line, sizeof Line, "Settings: aspect %s, filter %s, input margin %s, USB polling %s%s",
		  AspectName (m_nAspect), FilterName (m_nFilter), Margin,
		  g_nUSBInterruptPollMs ? "1 ms" : "native", m_bMultiCore ? "" : ", multicore off");
	Log (Line, COLOR_TEXT);
}

void CKernel::SaveConfig (void)
{
	char Text[240 + MaxCpuSets * 40];
	snprintf (Text, sizeof Text, "aspect=%s\r\nfilter=%s\r\n%s", AspectName (m_nAspect),
		  FilterName (m_nFilter), m_bHVS ? "" : "scaler=cpu\r\n");
	if (m_nInputMarginMin != 0)
	{
		char IM[32];
		snprintf (IM, sizeof IM, "inputmargin=%u.%u\r\n", m_nInputMarginMin / 1000,
			  (m_nInputMarginMin % 1000) / 100);
		strcat (Text, IM);
	}
	if (!m_bMultiCore)
	{
		strcat (Text, "multicore=off\r\n");
	}
	if (!m_bOverlay)
	{
		strcat (Text, "overlay=off\r\n");
	}
	if (g_nUSBInterruptPollMs == 0)
	{
		strcat (Text, "usbpoll=native\r\n");
	}
	if (m_nCpuPercent)
	{
		char Cpu[24];
		snprintf (Cpu, sizeof Cpu, "cpu=%u\r\n", m_nCpuPercent);
		strcat (Text, Cpu);
	}
	for (unsigned i = 0; i < m_nCpuSets; i++)
	{
		char Cpu[40];
		snprintf (Cpu, sizeof Cpu, "cpu.%s=%u\r\n", m_CpuSets[i].Set, m_CpuSets[i].nPercent);
		if (strlen (Text) + strlen (Cpu) < sizeof Text - 1)
		{
			strcat (Text, Cpu);
		}
	}
	WriteTextFile (CFG_PATH, Text);
}

unsigned CKernel::CpuPercentFor (const char *pSet) const
{
	for (unsigned i = 0; i < m_nCpuSets; i++)
	{
		if (strcmp (m_CpuSets[i].Set, pSet) == 0)
		{
			return m_CpuSets[i].nPercent;
		}
	}
	return m_nCpuPercent;		// 0: the core's default for the game
}

void CKernel::SetCpuPercentFor (const char *pSet, unsigned nPercent)
{
	for (unsigned i = 0; i < m_nCpuSets; i++)
	{
		if (strcmp (m_CpuSets[i].Set, pSet) == 0)
		{
			m_CpuSets[i].nPercent = nPercent;
			return;
		}
	}
	if (m_nCpuSets < MaxCpuSets)
	{
		strncpy (m_CpuSets[m_nCpuSets].Set, pSet, sizeof m_CpuSets[0].Set - 1);
		m_CpuSets[m_nCpuSets].Set[sizeof m_CpuSets[0].Set - 1] = '\0';
		m_CpuSets[m_nCpuSets].nPercent = nPercent;
		m_nCpuSets++;
	}
}

// cpspi/overlay.png: a picture the size of the display shown around the game
void CKernel::LoadOverlay (void)
{
	size_t nSize = 0;
	char *pFile = LoadText (OVERLAY_PATH, &nSize);
	if (pFile == 0)
	{
		Log ("Overlay: no cpspi/overlay.png, black around the picture", COLOR_DIM);
		return;
	}
	unsigned w = 0, h = 0;
	m_pOverlayRGB = cpspi_png_decode (pFile, nSize, &w, &h);
	delete [] pFile;
	char Line[MaxLineLen];
	if (m_pOverlayRGB == 0)
	{
		Log ("Overlay: cpspi/overlay.png not usable (needs an 8 bit RGB or RGBA PNG, not interlaced)", COLOR_ERR);
		return;
	}
	m_Video.SetOverlay (m_pOverlayRGB, w, h);
	m_Video.EnableOverlay (m_bOverlay);
	snprintf (Line, sizeof Line, "Overlay: cpspi/overlay.png %ux%u%s%s", w, h,
		  (w != m_Video.GetWidth () || h != m_Video.GetHeight ()) ? ", scaled to the display" : "",
		  m_bOverlay ? "" : " (off in cpspi.cfg)");
	Log (Line, COLOR_TEXT);
}

void CKernel::RunSystemMenu (void)
{
	enum { SmAspect, SmFilter, SmOverlay, SmControls, SmPower, SmCount };

	unsigned nSel = 0;
	u32 nPrev = ~0u;
	unsigned nOldFilter = m_nFilter, nOldAspect = m_nAspect;
	boolean bOldOverlay = m_bOverlay;
	const char *pMsg = "";
	unsigned nFrame = 0;

	CMachineInfo *pInfo = CMachineInfo::Get ();
	char Footer[48];
	snprintf (Footer, sizeof Footer, "CPS-PI " CPSPI_VERSION "  PI %u  %u C", pInfo->GetModelMajor (),
		  m_CPUThrottle.GetTemperature ());

	while (1)
	{
		if (nFrame++ % 15 == 0)
		{
			m_pInput->UpdateDevices ();
		}
		if (nFrame % 120 == 0)
		{
			snprintf (Footer, sizeof Footer, "CPS-PI " CPSPI_VERSION "  PI %u  %u C", pInfo->GetModelMajor (),
				  m_CPUThrottle.GetTemperature ());
		}

		u32 n = m_pInput->GetAny ();
		u32 nNew = nPrev == ~0u ? 0 : (n & ~nPrev);
		nPrev = n;

		if (nNew & BTN_UP)	{ nSel = (nSel + SmCount - 1) % SmCount; pMsg = ""; }
		if (nNew & BTN_DOWN)	{ nSel = (nSel + 1) % SmCount; pMsg = ""; }
		int nDir = (nNew & (BTN_1 | BTN_RIGHT)) ? 1 : ((nNew & BTN_LEFT) ? -1 : 0);

		if (nDir != 0)
		{
			switch (nSel)
			{
			case SmAspect:
				m_nAspect = (m_nAspect + ASPECT_COUNT + (unsigned) nDir) % ASPECT_COUNT;
				ApplyVideoMode ();
				break;

			case SmFilter:
				m_nFilter = (m_nFilter + FILTER_COUNT + (unsigned) nDir) % FILTER_COUNT;
				ApplyVideoMode ();
				break;

			case SmOverlay:
				if (m_Video.HasOverlay ())
				{
					m_bOverlay = !m_bOverlay;
					m_Video.EnableOverlay (m_bOverlay);
				}
				else
				{
					pMsg = "NO cpspi/overlay.png ON THE CARD";
				}
				break;

			case SmControls:
				if (nNew & BTN_1)
				{
					int nPad = ChoosePad ();
					if (nPad >= 0)
					{
						RunMapping ((unsigned) nPad, m_nLoaded >= 0);
					}
					else if (m_pInput->GetPadCount () == 0)
					{
						pMsg = "CONNECT A USB GAMEPAD";
					}
					nPrev = ~0u;
				}
				break;

			case SmPower:
				if ((nNew & BTN_1) && PowerOffCountdown ())
				{
					PowerOff ();
				}
				nPrev = ~0u;
				break;
			}
		}

		if (nNew & BTN_2)
		{
			break;
		}

		static const char *Labels[SmCount] = {"PICTURE", "SCANLINES", "OVERLAY", "CONTROLS", "POWER OFF"};
		TMenuItem Items[SmCount];
		for (unsigned i = 0; i < SmCount; i++)
		{
			Items[i].pLabel = Labels[i];
			Items[i].pValue = 0;
			Items[i].bDisabled = FALSE;
		}
		static const char *FilterLabels[FILTER_COUNT] = {"OFF", "LIGHT", "MEDIUM", "STRONG"};
		static const char *AspectLabels[ASPECT_COUNT] = {"4:3", "4:3 SHARP", "PIXEL"};
		Items[SmAspect].pValue = AspectLabels[m_nAspect < ASPECT_COUNT ? m_nAspect : 0];
		Items[SmFilter].pValue = FilterLabels[m_nFilter];
		Items[SmOverlay].pValue = !m_Video.HasOverlay () ? "NONE" : (m_bOverlay ? "ON" : "OFF");

		static const TButtonHint Hints[2] = {{'1', "CHANGE"}, {'2', "BACK"}};
		m_pUI->DrawPanelMenu (m_nLoaded >= 0, "OPTIONS", Items, SmCount, nSel, pMsg, UI_VALUE,
				      Hints, 2, Footer);
		ShowUIFrame ();
	}

	if (m_nFilter != nOldFilter || m_nAspect != nOldAspect || m_bOverlay != bOldOverlay)
	{
		SaveConfig ();
		char Line[120];
		snprintf (Line, sizeof Line, "New settings: aspect %s, filter %s, overlay %s",
			  AspectName (m_nAspect), FilterName (m_nFilter), m_bOverlay ? "on" : "off");
		Log (Line, COLOR_TEXT);
	}
}

// ---- control mapping --------------------------------------------------------

void CKernel::LoadPads (void)
{
	static char Text[4096];
	FIL File;
	UINT nRead = 0;
	if (f_open (&File, PADS_PATH, FA_READ) == FR_OK)
	{
		f_read (&File, Text, sizeof Text - 1, &nRead);
		f_close (&File);
	}
	Text[nRead] = '\0';
	m_pInput->LoadMaps (Text);
}

void CKernel::SavePads (void)
{
	static char Text[4096];
	m_pInput->FormatMaps (Text, sizeof Text);
	WriteTextFile (PADS_PATH, Text);
}

// CONTROLS from the OPTIONS menu: which pad? (the only one, or the one that is pressed)
int CKernel::ChoosePad (void)
{
	unsigned nCount = m_pInput->GetPadCount ();
	if (nCount == 0)
	{
		return -1;
	}
	if (nCount == 1)
	{
		for (unsigned i = 0; i < MAX_PADS; i++)
		{
			if (m_pInput->IsPadConnected (i))
			{
				return (int) i;
			}
		}
	}

	boolean bReleased = FALSE;
	for (unsigned f = 0; f < 600; f++)		// 10 seconds
	{
		int nActive = m_pInput->GetActivePad ();
		if (!bReleased)
		{
			bReleased = nActive < 0;
		}
		else if (nActive >= 0)
		{
			return nActive;
		}
		if (m_pInput->GetKeyboard () & BTN_MENU)
		{
			break;
		}
		m_pUI->DrawMessage ("CONTROLS", "PRESS A BUTTON ON THE PAD", "YOU WANT TO SET UP");
		ShowUIFrame ();
	}
	return -1;
}

// Asks for 1 2 3 4 5 6 START SELECT in turn on one pad (directions are automatic),
// then saves on its own. A button already used is ignored; Esc on the
// keyboard cancels.
boolean CKernel::RunMapping (unsigned nSlot, boolean bOverGame)
{
	static const char *Names[8] = {"1", "2", "3", "4", "5", "6", "START", "SELECT / COIN"};
	const unsigned nSteps = MAP_COUNT - MAP_FIRST_BUTTON;

	TBind Binds[MAP_COUNT];
	memset (Binds, 0, sizeof Binds);
	static char Values[8][16];
	const char *pValues[8];
	for (unsigned i = 0; i < 8; i++)
	{
		Values[i][0] = '\0';
		pValues[i] = Values[i];
	}

	unsigned nStep = 0;
	boolean bWaitRelease = TRUE;		// e.g. 1 still held from the OPTIONS menu
	u32 nKeyPrev = ~0u;
	unsigned nFrame = 0;

	char Line[120];
	snprintf (Line, sizeof Line, "Controls: setting up pad %u (%04x:%04x)",
		  nSlot + 1, m_pInput->GetVendor (nSlot), m_pInput->GetProduct (nSlot));
	Log (Line, COLOR_TEXT);

	char Title[32], Footer[40];
	snprintf (Title, sizeof Title, "CONTROLS  PLAYER %u", m_pInput->GetPlayerOfPad (nSlot) + 1);
	snprintf (Footer, sizeof Footer, "PAD %04X:%04X", m_pInput->GetVendor (nSlot),
		  m_pInput->GetProduct (nSlot));

	while (nStep < nSteps)
	{
		if (nFrame++ % 15 == 0)
		{
			m_pInput->UpdateDevices ();
			if (!m_pInput->IsPadConnected (nSlot))
			{
				Log ("  pad unplugged: set-up cancelled", COLOR_TEXT);
				return FALSE;
			}
		}

		u32 nKey = m_pInput->GetKeyboard ();
		u32 nKeyNew = nKeyPrev == ~0u ? 0 : (nKey & ~nKeyPrev);
		nKeyPrev = nKey;
		if (nKeyNew & BTN_MENU)
		{
			Log ("  set-up cancelled", COLOR_TEXT);
			return FALSE;
		}

		if (bWaitRelease)
		{
			bWaitRelease = !m_pInput->IsReleased (nSlot);
		}
		else
		{
			int nButton = m_pInput->CaptureButton (nSlot);
			if (nButton >= 0)
			{
				boolean bUsed = FALSE;
				for (unsigned i = 0; i < nStep; i++)
				{
					if (Binds[MAP_FIRST_BUTTON + i].nIndex == (u8) nButton)
					{
						bUsed = TRUE;
					}
				}
				if (!bUsed)
				{
					TBind &B = Binds[MAP_FIRST_BUTTON + nStep];
					B.nType = BindButton;
					B.nIndex = (u8) nButton;
					B.nValue = 0;
					CInput::DescribeBind (B, Values[nStep], sizeof Values[nStep]);
					nStep++;
				}
				bWaitRelease = TRUE;
			}
		}

		m_pUI->DrawMapping (bOverGame, Title, (int) nStep, pValues, Footer);
		ShowUIFrame ();
	}

	m_pInput->SetMap (nSlot, Binds);
	SavePads ();
	for (unsigned i = 0; i < nSteps; i++)
	{
		snprintf (Line, sizeof Line, "  %s = %s", Names[i], Values[i]);
		Log (Line, COLOR_TEXT);
	}
	Log ("  controls saved to cpspi/pads.cfg", COLOR_TEXT);
	SaveLog ();

	// "SAVED" for one second
	for (unsigned f = 0; f < 60; f++)
	{
		m_pUI->DrawMapping (bOverGame, Title, (int) nSteps, pValues, Footer);
		ShowUIFrame ();
	}
	return TRUE;
}

boolean CKernel::PowerOffCountdown (void)
{
	// 3 seconds, 2 cancels; drawn over whatever the UI buffer shows
	static u16 Saved[GAME_SRC_PITCH * GAME_SRC_LINES];
	memcpy (Saved, m_pUI->GetBuffer (), sizeof Saved);
	u32 nPrev = ~0u;
	for (unsigned f = 0; f < 180; f++)
	{
		u32 n = m_pInput->GetAny ();
		u32 nNew = nPrev == ~0u ? 0 : (n & ~nPrev);
		nPrev = n;
		if (nNew & BTN_2)
		{
			memcpy (m_pUI->GetBuffer (), Saved, sizeof Saved);
			Log ("  power off cancelled", COLOR_TEXT);
			return FALSE;
		}
		char Line[24];
		snprintf (Line, sizeof Line, "POWER OFF IN %u", 3 - f / 60);
		memcpy (m_pUI->GetBuffer (), Saved, sizeof Saved);
		static const TButtonHint Cancel = {'2', "CANCEL"};
		m_pUI->DrawBox (Line, "", &Cancel);
		ShowUIFrame ();
	}
	return TRUE;
}

void CKernel::PowerOff (void)
{
	Log ("Power off: saving data and halting", COLOR_TITLE);
	UnloadGame ();
	SaveLog ();
	f_mount (0, "SD:", 0);
	m_bSDOK = FALSE;

	// black picture, then halt (the firmware stops the HDMI output)
	m_Video.Clear (0, RGB (0, 0, 0));
	m_Video.Clear (1, RGB (0, 0, 0));
	m_Video.ShowBuffer (0);
	CTimer::SimpleMsDelay (300);

	CBcmWatchdog Watchdog;
	Watchdog.Restart (CBcmWatchdog::PartitionHalt);
	while (1)
	{
	}
}

// ---- game list ------------------------------------------------------------

unsigned CKernel::RunList (unsigned nSel)
{
	u32 nPrev = ~0u;		// require a release before acting
	unsigned nRepeat = 0;
	unsigned nFrame = 0;

	while (1)
	{
		if (nFrame++ % 15 == 0)
		{
			m_pInput->UpdateDevices ();
			m_pInput->FlushDiagnostics ();

			// a pad never seen before: set up its controls first
			int nPad = m_pInput->GetUnmappedPad ();
			if (nPad >= 0)
			{
				RunMapping ((unsigned) nPad, m_nLoaded >= 0);
				nPrev = ~0u;
				continue;
			}
		}

		u32 n = m_pInput->GetAny ();
		u32 nNew = n & ~nPrev;

		// up/down with auto-repeat, left/right = page
		u32 nMove = nNew;
		if (n & (BTN_UP | BTN_DOWN))
		{
			if (++nRepeat > 20 && nRepeat % 4 == 0)
			{
				nMove |= n & (BTN_UP | BTN_DOWN);
			}
		}
		else
		{
			nRepeat = 0;
		}
		if ((nMove & BTN_UP) && nSel > 0)			nSel--;
		if ((nMove & BTN_DOWN) && nSel + 1 < m_nGames)		nSel++;
		if (nNew & BTN_LEFT)	nSel = nSel >= 8 ? nSel - 8 : 0;
		if (nNew & BTN_RIGHT)	nSel = nSel + 8 < m_nGames ? nSel + 8 : m_nGames - 1;

		if (nNew & (BTN_1 | BTN_START))
		{
			return nSel;
		}
		if (nNew & BTN_4)
		{
			RunSystemMenu ();
			n = ~0u;		// wait for release
		}
		nPrev = n;

		const char *pMsg = m_pInput->GetPadCount () == 0 ? "CONNECT A USB GAMEPAD" : "";
		m_pUI->DrawList (m_pGames, m_nGames, nSel, "", m_nLoaded >= 0);
		ShowUIFrame (TRUE, pMsg);
	}
}

// ---- game loop ------------------------------------------------------------

// statistics windows restart after a pause (menu, credits)
void CKernel::ResetGameStats (void)
{
	s_nWindowStart = CTimer::GetClockTicks ();
	s_HistEmu.Reset (); s_HistRender.Reset (); s_HistWork.Reset (); s_HistDraw.Reset ();
	s_HistPeriod.Reset (); s_HistLag.Reset (); s_HistSlack.Reset ();
	s_HistMargin.Reset (); s_HistPost.Reset (); s_HistWait.Reset ();
	s_nOverBudget = s_nMissedVSync = s_nWindowFrames = 0;
	s_nLateHooks = s_nMissUnexplained = s_nBlocksLate = s_nGapMaxWin = s_nCarried = 0;
}

// Credits over the paused game; 2 (or Esc) goes back to the game
void CKernel::RunCredits (void)
{
	Log ("  credits", COLOR_TEXT);
	u32 nPrev = ~0u;
	for (unsigned nFrame = 0; ; nFrame++)
	{
		if (nFrame % 15 == 0)
		{
			m_pInput->UpdateDevices ();	// a new pad is set up after the credits
		}
		u32 n = m_pInput->GetAny ();
		u32 nNew = nPrev == ~0u ? 0 : (n & ~nPrev);
		if (nPrev == ~0u)
		{
			if (n == 0) nPrev = 0;	// wait for Select and 3 to be released
		}
		else
		{
			nPrev = n;
		}
		if (nNew & (BTN_2 | BTN_MENU))
		{
			return;
		}
		m_pUI->DrawCredits (CPSPI_VERSION);
		ShowUIFrame ();
	}
}

void CKernel::RunGame (void)
{
	const char *pName = m_pGames[m_nLoaded].Name;
	const unsigned nPlayers = core_players () < CORE_MAX_PLAYERS ? core_players () : CORE_MAX_PLAYERS;

	// status line (Select+2), refreshed once per second
	char StFps[16] = "", StLoad[8] = "", StTemp[8] = "", StRes[8] = "", StScale[16] = "";
	u32 nFpsColor = UI_WHITE, nLoadColor = UI_OK, nTempColor = UI_WHITE;
	boolean bStatusReady = FALSE;

	// message in the top band (screenshot), in frames
	char Toast[64] = "";
	unsigned nToast = 0, nClearToast = 0;

	boolean bMenu = FALSE;
	unsigned nMenuSel = 0;
	char MenuMsg[40] = "";
	u32 nMenuMsgColor = UI_OK;
	boolean bCpuChanged = FALSE;		// CPU SPEED changed in the menu: save on resume
	char MsgLine[MaxLineLen];

	u32 nPrev[CORE_MAX_PLAYERS];
	boolean bSelectCombo[CORE_MAX_PLAYERS];
	unsigned nCoin[CORE_MAX_PLAYERS];
	unsigned nSelectHeld[CORE_MAX_PLAYERS];	// frames Select has been held alone
	for (unsigned p = 0; p < CORE_MAX_PLAYERS; p++)
	{
		nPrev[p] = ~0u;			// wait for release after the list
		bSelectCombo[p] = FALSE;
		nCoin[p] = 0;
		nSelectHeld[p] = 0;
	}
	unsigned nTestFrames = 0;		// > 0: TEST switch held (service menu)
	u32 nMenuPrev = ~0u;
	m_pInput->RestartDiagnostics ();		// log the pad reports of this game too

	unsigned nFrames = 0;
	unsigned nWinStart = CTimer::GetClockTicks ();
	unsigned nWorkMax = 0;
	unsigned nPostMax = 0;			// worst "controls read -> frame over" of the second
	unsigned nLastVSync = CTimer::GetClockTicks ();
	unsigned nNextDeadline = nLastVSync + FRAME_US;
	unsigned nTotalFrames = 0;

	// the controls go in at the core's vblank (CoreVBlank), as late before
	// the vsync as the margin allows: start cautious, then from what is measured
	s_nInputMargin = m_nInputMarginMin > 2000 ? m_nInputMarginMin : 2000;
	s_nMarginBoost = 0;
	s_nLastVSync = nLastVSync;
	s_nReportLimit = 900;			// a first statistics window after 15 s, then per minute
	// pacing and queue from the first frame, with what a Pi 3 settles at (a
	// 1080p frame of 16.8 ms: blocks at 3.1, 6.2 and 9.3 ms, a queue of
	// 600 frames); measured and corrected after the first second
	for (unsigned b = 0; b < 3; b++) s_BlockDeadline[b] = 3100 * (b + 1);
	s_nBlockWorst = s_nLastBlockWorst = s_nDrawWorst = s_nGapMax = 0;
	s_tLastAudio = 0;
	m_Audio.SetTarget (700);
	for (unsigned p = 0; p < CORE_MAX_PLAYERS; p++)
	{
		s_LateIgnore[p] = TRUE;
		s_LateExtra[p] = 0;
	}
	s_bLateEnabled = TRUE;

	ResetGameStats ();
	{
		unsigned nOld;
		m_pInput->TakeChange (&nOld);	// a press from the list does not count
	}
	m_Audio.Prime ();			// no gap in the sound while the queue fills
	{
		unsigned a, b, c;
		int d;
		m_Audio.GetStats (&a, &b, &c, &d);	// reset audio statistics
	}

	// black bands: status line at the bottom, messages at the top (only while
	// playing); drawn into the back buffer before the frame, outside the picture
	auto DrawBands = [&] (void)
	{
		if (m_bStatusOn && !bMenu && bStatusReady)
		{
			const CVideoOut::TTextSpan Spans[] =
			{
				{StFps, nFpsColor}, {"   LOAD ", UI_GREY}, {StLoad, nLoadColor},
				{"   ", UI_GREY}, {StTemp, nTempColor},
				{"   ", UI_GREY}, {StRes, UI_WHITE}, {StScale, UI_GREY}
			};
			m_Video.DrawBandText (FALSE, Spans, sizeof Spans / sizeof Spans[0]);
			m_nClearStatus = 2;
		}
		else if (m_nClearStatus)
		{
			m_Video.DrawBandText (FALSE, 0, 0);
			m_nClearStatus--;
		}
		if (nToast && !bMenu)
		{
			const CVideoOut::TTextSpan Span = {Toast, UI_WHITE};
			m_Video.DrawBandText (TRUE, &Span, 1);
			nToast--;
			nClearToast = 2;
		}
		else if (nClearToast)
		{
			m_Video.DrawBandText (TRUE, 0, 0);
			nClearToast--;
		}
	};

	while (1)
	{
		unsigned t0 = CTimer::GetClockTicks ();
		s_bPresented = FALSE;

		if (nTotalFrames % 30 == 0)
		{
			m_pInput->UpdateDevices ();

			int nPad = bMenu ? -1 : m_pInput->GetUnmappedPad ();
			if (nPad >= 0)
			{
				// new pad during the game: pause and set it up
				m_pUI->SnapshotGame (core_video (), core_flipped ());
				RunMapping ((unsigned) nPad, TRUE);
				for (unsigned p = 0; p < CORE_MAX_PLAYERS; p++) nPrev[p] = ~0u;
				unsigned nOld;
				m_pInput->TakeChange (&nOld);	// presses during the set-up do not count
				m_Audio.Prime ();		// the queue ran dry meanwhile
				s_tLastAudio = 0;
			}
		}

		// the controls now (menus, combinations); the game itself gets them
		// again at its vblank (CoreVBlank), where the lag is measured from
		u32 Pad[CORE_MAX_PLAYERS];
		for (unsigned p = 0; p < CORE_MAX_PLAYERS; p++)
		{
			Pad[p] = m_pInput->Get (p);
		}

		if (!bMenu)
		{
			boolean bOpenMenu = FALSE, bOpenCredits = FALSE, bScreenshot = FALSE;
			for (unsigned p = 0; p < CORE_MAX_PLAYERS; p++)
			{
				u32 n = Pad[p];
				if (nPrev[p] == ~0u)
				{
					// buttons still held from the list or the menu: ignore until released
					if (n == 0)
					{
						nPrev[p] = 0;
					}
					s_LateIgnore[p] = TRUE;
					core_set_input (p, 0);
					continue;
				}
				s_LateIgnore[p] = FALSE;

				u32 nNew = n & ~nPrev[p];

				if (n & BTN_SELECT)
				{
					// Select + 1: screenshot, + 2: status line, + 3: credits
					if (nNew & BTN_ANYBUTTON)
					{
						bSelectCombo[p] = TRUE;
					}
					if (nNew & BTN_1)
					{
						bScreenshot = TRUE;
					}
					if (nNew & BTN_2)
					{
						m_bStatusOn = !m_bStatusOn;
						m_nClearStatus = 2;
					}
					if (nNew & BTN_3)
					{
						bOpenCredits = TRUE;
					}
					// Select held alone for one second: menu (no coin)
					if (!bSelectCombo[p] && ++nSelectHeld[p] >= 60)
					{
						bOpenMenu = TRUE;
						bSelectCombo[p] = TRUE;
					}
				}
				else
				{
					if ((nPrev[p] & BTN_SELECT) && !bSelectCombo[p])
					{
						nCoin[p] = 6;		// short press: coin
					}
					bSelectCombo[p] = FALSE;
					nSelectHeld[p] = 0;
				}
				if (nNew & BTN_MENU)
				{
					bOpenMenu = TRUE;
				}
				nPrev[p] = n;

				// CPS1 inputs now (the core samples them again at its vblank)
				u32 nExtra = 0;
				if (nCoin[p]) { nExtra |= CORE_IN_COIN; nCoin[p]--; }
				if (p == 0 && nTestFrames) { nExtra |= CORE_IN_TEST; nTestFrames--; }
				s_LateExtra[p] = nExtra;
				core_set_input (p, GameFlags (n) | nExtra);
			}

			if (bOpenMenu || bOpenCredits)
			{
				for (unsigned p = 0; p < CORE_MAX_PLAYERS; p++) core_set_input (p, 0);
			}
			if (bOpenCredits)
			{
				// Select + 3: credits over the paused game (2 closes)
				m_pUI->SnapshotGame (core_video (), core_flipped ());
				RunCredits ();
				for (unsigned p = 0; p < CORE_MAX_PLAYERS; p++)
				{
					nPrev[p] = ~0u;
					bSelectCombo[p] = FALSE;
				}
				nToast = 0;
				nClearToast = m_nClearStatus = 2;
				ResetGameStats ();
				m_Audio.Prime ();
				s_tLastAudio = 0;
				continue;
			}
			if (bOpenMenu)
			{
				bMenu = TRUE;
				nMenuSel = 0;
				MenuMsg[0] = '\0';
				nMenuPrev = ~0u;
				nToast = 0;
				m_pUI->SnapshotGame (core_video (), core_flipped ());
				m_pInput->FlushDiagnostics ();
				ReportWindow (TRUE);
				SaveLog ();
			}
			else
			{
				if (bScreenshot)
				{
					// the frame on screen, before emulating the next one
					const char *pFile = SaveScreenshot ();
					snprintf (Toast, sizeof Toast, pFile ? "SCREENSHOT SAVED  %s" : "SCREENSHOT FAILED",
						  pFile);
					nToast = 60;
				}
				DrawBands ();
				s_tBlockStart = CTimer::GetClockTicks ();
				s_nWaitUs = 0;
				core_frame ();		// -> CoreAudioBlock x4, CoreVBlank: picture, flip, wait, controls

				if (core_fatal ())
				{
					Log ("  the core stopped with an error, back to the list", COLOR_ERR);
					SaveLog ();
					s_bLateEnabled = FALSE;
					UnloadGame ();
					return;
				}
			}
		}

		if (bMenu)
		{
			u32 n = 0;
			for (unsigned p = 0; p < CORE_MAX_PLAYERS; p++) n |= Pad[p];
			u32 nNew = nMenuPrev == ~0u ? 0 : (n & ~nMenuPrev);
			nMenuPrev = n;

			if (nNew & BTN_UP)	{ nMenuSel = (nMenuSel + GmCount - 1) % GmCount; MenuMsg[0] = '\0'; }
			if (nNew & BTN_DOWN)	{ nMenuSel = (nMenuSel + 1) % GmCount; MenuMsg[0] = '\0'; }

			if (nMenuSel == GmSpeed && (nNew & (BTN_LEFT | BTN_RIGHT | BTN_1)))
			{
				// the 68000's share of its clock, 5% at a time, kept for this set
				int nDir = (nNew & BTN_LEFT) ? -5 : 5;
				int nPercent = (int) core_cpu_percent () + nDir;
				if (nPercent < 50) nPercent = 50;
				if (nPercent > 100) nPercent = 100;
				core_set_cpu_percent ((unsigned) nPercent);
				SetCpuPercentFor (m_pGames[m_nLoaded].File, (unsigned) nPercent);
				bCpuChanged = TRUE;
				nNew &= ~BTN_1;
			}

			if (nNew & BTN_2)
			{
				bMenu = FALSE;		// resume
				if (bCpuChanged)
				{
					SaveConfig ();
					bCpuChanged = FALSE;
					snprintf (MsgLine, sizeof MsgLine, "  68000 at %u%% of its clock for %s (cpspi.cfg)",
						  core_cpu_percent (), m_pGames[m_nLoaded].File);
					Log (MsgLine, COLOR_TEXT);
				}
			}
			else if (nNew & BTN_1)
			{
				switch (nMenuSel)
				{
				case GmReset:
					core_reset ();
					Log ("  reset", COLOR_TEXT);
					bMenu = FALSE;
					break;

				case GmService:
					nTestFrames = 10;		// press the TEST switch
					Log ("  service menu: TEST switch", COLOR_TEXT);
					bMenu = FALSE;
					break;

				case GmList:
					SaveLog ();
					s_bLateEnabled = FALSE;
					return;

				case GmPower:
					if (PowerOffCountdown ())
					{
						PowerOff ();
					}
					nMenuPrev = ~0u;
					break;
				}
			}
			if (!bMenu)
			{
				for (unsigned p = 0; p < CORE_MAX_PLAYERS; p++)
				{
					nPrev[p] = ~0u;	// ignore buttons until released
					bSelectCombo[p] = FALSE;
				}
				ResetGameStats ();
				m_Audio.Prime ();	// the queue ran dry in the menu
				s_tLastAudio = 0;
			}

			if (bMenu)
			{
				static const char *Labels[GmCount] =
				{
					"RESET", "CPU SPEED", "SERVICE MENU", "GAME LIST", "POWER OFF"
				};
				TMenuItem Items[GmCount];
				for (unsigned i = 0; i < GmCount; i++)
				{
					Items[i].pLabel = Labels[i];
					Items[i].pValue = 0;
					Items[i].bDisabled = FALSE;
				}
				char Speed[20];
				snprintf (Speed, sizeof Speed, "%u%%", core_cpu_percent ());
				Items[GmSpeed].pValue = Speed;
				const char *pMsg = MenuMsg;
				u32 nMsgColor = nMenuMsgColor;
				if (!MenuMsg[0] && nMenuSel == GmService)
				{
					pMsg = "DIP SWITCHES AND TESTS";
					nMsgColor = UI_DIM;
				}
				else if (!MenuMsg[0] && nMenuSel == GmSpeed)
				{
					pMsg = "LOWER = MORE LIKE THE BOARD'S WAIT STATES";
					nMsgColor = UI_DIM;
				}
				static const TButtonHint Hints[2] = {{'1', "SELECT"}, {'2', "RESUME"}};
				m_pUI->DrawPanelMenu (TRUE, pName, Items, GmCount, nMenuSel, pMsg, nMsgColor,
						      Hints, 2, 0);
				m_Video.DrawGame (m_pUI->GetBuffer ());
			}
			else
			{
				m_Video.DrawGame (core_video (), core_flipped ());
			}
			DrawBands ();
			m_Video.Flip ();
		}
		unsigned tRet = CTimer::GetClockTicks ();

		unsigned nLate = 0;		// the frame began this long ago (HVS: carried on, not lost)
		if (m_bVSync)
		{
			m_Video.WaitForVSync (&nLate);
		}
		else
		{
			while ((int) (CTimer::GetClockTicks () - nNextDeadline) < 0)
			{
			}
			nNextDeadline += FRAME_US;
		}
		unsigned t4 = CTimer::GetClockTicks () - nLate;	// when the frame began
		for (unsigned c = 0; c < 3; c++)
		{
			s_HVSAtVSync[c] = m_Video.ReadHVSStat (c);
		}
		unsigned nPeriod = t4 - nLastVSync;
		unsigned nPredicted = nLastVSync + FRAME_US;	// where this vsync was expected
		nLastVSync = t4;
		s_nLastVSync = t4;
		nTotalFrames++;

		if (s_bPresented)
		{
			unsigned nPost = tRet - s_tSample;		// handler lines + return, after the controls
			if (nLate)
			{
				s_nCarried++;
			}
			if (nPeriod > 25000 && (int) (s_tFlip - (nPredicted - s_nInputMargin)) <= 0)
			{
				// the picture was ready and the controls read in time, yet the
				// vsync notification came a frame late: read the controls earlier
				s_nMissUnexplained++;
				if (s_nMarginBoost < 3000)
				{
					s_nMarginBoost += 500;
					s_nInputMargin += 500;
				}
			}
			unsigned nWait = s_nWaitUs < s_tEmuEnd - t0 ? s_nWaitUs : 0;
			s_HistEmu.Add (s_tEmuEnd - t0 - nWait);	// without the pacing waits
			s_HistRender.Add (core_render_us ());
			s_HistWork.Add (s_tFlip - t0 - nWait);
			s_HistDraw.Add (s_tFlip - s_tEmuEnd);
			s_HistWait.Add (nWait);
			s_HistPeriod.Add (nPeriod);
			s_HistSlack.Add ((int) (t4 - s_tFlip) > 0 ? t4 - s_tFlip : 0);
			s_HistMargin.Add ((int) (t4 - s_tSample) > 0 ? t4 - s_tSample : 0);
			s_HistPost.Add (nPost);
			if (nPost > nPostMax) nPostMax = nPost;
			if (s_bLateChange)
			{
				// pad report -> the vblank sample that took it in
				s_HistLag.Add (s_tSample - s_nLateChangeTicks);
				s_bLateChange = FALSE;
			}
			if (s_tFlip - t0 - nWait > 16000) s_nOverBudget++;
			if (nPeriod > 25000) s_nMissedVSync++;
			s_nWindowFrames++;
			if (s_nWindowFrames >= s_nReportLimit)
			{
				ReportWindow (FALSE);
				m_pInput->FlushDiagnostics ();
				s_nReportLimit = REPORT_FRAMES;
				SaveLog (TRUE);		// by core 3: the log survives a power cut, the game does not notice
			}
			// status line: the worst frame of the second (emulation and draw, not the waits)
			if (s_tFlip - t0 - nWait > nWorkMax)
			{
				nWorkMax = s_tFlip - t0 - nWait;
			}
		}

		// status line, once per second
		if (++nFrames == 60)
		{
			unsigned nElapsed = t4 - nWinStart;
			unsigned nFPS100 = (unsigned) (6000ULL * 1000000 / nElapsed);

			if (m_bVSync && nFPS100 > 7500)
			{
				m_bVSync = FALSE;
				nNextDeadline = CTimer::GetClockTicks () + FRAME_US;
				Log ("firmware vsync does not block: switching to timer pacing", COLOR_ERR);
			}

			// the board's 59.64 fps (or 60.0 with a standard HDMI mode)
			snprintf (StFps, sizeof StFps, "%u.%u FPS", nFPS100 / 100, (nFPS100 % 100) / 10);
			nFpsColor = (nFPS100 + 10 >= CPS1_FPS100 && nFPS100 <= 6010) ? UI_WHITE : UI_WARN;
			unsigned nLoad = nWorkMax * 100 / FRAME_US;
			snprintf (StLoad, sizeof StLoad, "%u%%", nLoad);
			nLoadColor = nLoad > 95 ? UI_ERR : (nLoad >= 75 ? UI_WARN : UI_OK);
			unsigned nTemp = m_CPUThrottle.GetTemperature ();
			snprintf (StTemp, sizeof StTemp, "%u" FONT_DEGREE "C", nTemp);
			nTempColor = nTemp > 75 ? UI_ERR : (nTemp > 65 ? UI_WARN : UI_WHITE);
			snprintf (StRes, sizeof StRes, "%uP", m_Video.GetHeight ());
			unsigned k = m_Video.GetHVSFactor (), S = m_Video.GetScale ();
			snprintf (StScale, sizeof StScale, " %uX %s", S,
				  k == S ? "HVS" : (k == 1 ? "CPU" : "CPU+HVS"));
			bStatusReady = TRUE;

			// the controls as late as possible: twice the worst "after the
			// controls" time of the second plus half a millisecond covers the
			// handler's lines and the vsync's own jitter; never under the minimum
			unsigned nMarginMin = m_nInputMarginMin ? m_nInputMarginMin
					      : (m_Video.GetHVSChannel () >= 0 ? 1000 : 2000);
			unsigned nMargin = nPostMax * 2 + 500;
			if (nMargin < nMarginMin) nMargin = nMarginMin;
			nMargin += s_nMarginBoost;
			if (nMargin > 5000) nMargin = 5000;
			s_nInputMargin = nMargin;
			nPostMax = 0;

			// pacing of the sound blocks: the last block (with the picture)
			// and the draw, at their slowest of this second and a quarter
			// more, must end before the controls; the three earlier blocks
			// are spread evenly up to that point
			unsigned nTail = nMargin + 500 + (s_nDrawWorst + s_nLastBlockWorst) * 5 / 4;
			unsigned nLast = FRAME_US > nTail ? FRAME_US - nTail : 0;
			for (unsigned b = 0; b < 3; b++)
			{
				s_BlockDeadline[b] = nLast * (b + 1) / 3;
			}
			// the audio queue: the longest gap between blocks, a DMA chunk and a little
			unsigned nNeed = (unsigned) ((u64) s_nGapMax * 48 / 1000) + 192 + 120;
			unsigned nTargetNow = m_Audio.GetTarget ();
			if (nNeed > nTargetNow + 40 || nNeed + 40 < nTargetNow)
			{
				m_Audio.SetTarget (nNeed);
			}
			s_nBlockWorst = s_nLastBlockWorst = s_nDrawWorst = s_nGapMax = 0;

			m_CPUThrottle.Update ();
			m_ActLED.On ();
			nFrames = 0;
			nWorkMax = 0;
			nWinStart = CTimer::GetClockTicks ();
		}
		else if (nFrames == 30)
		{
			m_ActLED.Off ();
		}
	}
	(void) nPlayers;
}

// ---- main flow ------------------------------------------------------------

TShutdownMode CKernel::Run (void)
{
	if (!m_bVideoOK)
	{
		Fatal (CPSPI_ERR_FB, "cannot set up the HDMI output");
	}

	char Line[180];
	Log ("cps-pi - bare metal CPS1 emulator", COLOR_TITLE);
	snprintf (Line, sizeof Line, "Version %s, built %s %s", CPSPI_VERSION, __DATE__, __TIME__);
	Log (Line, COLOR_DIM);

	CMachineInfo *pInfo = CMachineInfo::Get ();
	m_CPUThrottle.SetSpeed (CPUSpeedMaximum);
	snprintf (Line, sizeof Line, "%s, %u MB RAM, CPU %u MHz, temperature %u C",
		  pInfo->GetMachineName (), pInfo->GetRAMSize (),
		  m_CPUThrottle.GetClockRate () / 1000000, m_CPUThrottle.GetTemperature ());
	Log (Line, COLOR_TEXT);
	snprintf (Line, sizeof Line, "HDMI %ux%u, pixel order %s", m_Video.GetWidth (), m_Video.GetHeight (),
		  m_Video.GetPixelOrder () ? "RGB" : "BGR");
	Log (Line, COLOR_TEXT);

	// SD card
	if (!m_EMMC.Initialize ())
	{
		Fatal (CPSPI_ERR_SD, "SD card not initialised");
	}
	if (f_mount (&m_FileSystem, "SD:", 1) != FR_OK)
	{
		Fatal (CPSPI_ERR_SD, "cannot read the SD card file system");
	}
	m_bSDOK = TRUE;
	f_mkdir (CPSPI_DIR);
	f_mkdir (ROMS_DIR);
	f_mkdir (NVRAM_DIR);
	f_mkdir (SHOT_DIR);
	LoadConfig ();
	LoadOverlay ();

	// the picture is written to the framebuffer by all four cores
	if (!m_bMultiCore)
	{
		Log ("Drawing: core 0 alone (multicore=off)", COLOR_TEXT);
	}
	else if (m_Video.StartDrawCores ())
	{
		Log ("Drawing: the picture is written by all four cores", COLOR_TEXT);
	}
	else
	{
		Log ("Drawing: cores 1-3 did not start, core 0 draws alone", COLOR_ERR);
	}

	m_pUI = new CUI;

	// games
	ScanGames ();
	snprintf (Line, sizeof Line, "Games found: %u", m_nGames);
	Log (Line, m_nGames ? COLOR_OK : COLOR_ERR);
	if (m_nGames == 0)
	{
		Fatal (CPSPI_ERR_GAME, "no supported CPS1 zip in the cpspi/roms folder");
	}
	SaveLog ();

	// USB and audio
	Log ("USB...", COLOR_TEXT);
	if (!m_USBHCI.Initialize ())
	{
		Log ("  USB controller not initialised", COLOR_ERR);
	}
	m_pInput = new CInput (&m_USBHCI, &m_DeviceNameService);
	LoadPads ();
	m_pInput->UpdateDevices ();

	if (m_Audio.Initialize (&m_Interrupt))
	{
		Log ("Audio HDMI: OK (48 kHz)", COLOR_OK);
	}
	else
	{
		Log ("HDMI audio: not available, playing without sound", COLOR_ERR);
	}

	if (!core_init (CPSPI_DIR))
	{
		Fatal (CPSPI_ERR_CORE, "the emulator core did not initialise");
	}
	s_pLateInput = m_pInput;
	core_set_input_callback (CoreVBlankHook);
	core_set_audio_callback (CoreAudioHook);

	// last played game
	unsigned nSel = 0;
	{
		FIL File;
		char Last[64];
		UINT nRead = 0;
		if (f_open (&File, LAST_PATH, FA_READ) == FR_OK)
		{
			f_read (&File, Last, sizeof Last - 1, &nRead);
			f_close (&File);
			Last[nRead] = '\0';
			for (unsigned i = 0; i < m_nGames; i++)
			{
				if (strcmp (m_pGames[i].File, Last) == 0)
				{
					nSel = i;
				}
			}
		}
	}

	FILINFO Info;
	boolean bBench = f_stat (BENCH_FLAG, &Info) == FR_OK;
	if (bBench)
	{
		if (LoadGame (nSel))
		{
			Benchmark ();
		}
		CTimer::SimpleMsDelay (8000);		// time to read the results on screen
	}
	SaveLog ();

	// from here on the screen belongs to the UI
	ApplyVideoMode ();
	if (m_Video.ProbeHVS ())
	{
		snprintf (Line, sizeof Line, "VSync: HVS channel %d line counter (frame start = line 0)",
			  m_Video.GetHVSChannel ());
		Log (Line, COLOR_TEXT);
	}
	else
	{
		Log ("VSync: firmware notification (HVS line counter not found)", COLOR_ERR);
	}
	SaveLog ();
	m_bQuiet = TRUE;

	boolean bDirect = m_nGames == 1;
	while (1)
	{
		if (!bDirect)
		{
			nSel = RunList (nSel);
		}
		bDirect = FALSE;

		if (m_nLoaded != (int) nSel && !LoadGame (nSel))
		{
			m_pUI->DrawMessage ("CANNOT LOAD GAME", m_pGames[nSel].Name, "SEE cpspi/LOG.TXT");
			SaveLog ();
			for (unsigned i = 0; i < 180; i++)
			{
				ShowUIFrame ();
			}
			continue;
		}
		SaveLog ();
		RunGame ();
		m_bQuiet = TRUE;
	}

	return ShutdownHalt;
}
