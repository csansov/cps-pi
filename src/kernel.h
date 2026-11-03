/*
 * cps-pi - kernel
 */
#ifndef KERNEL_H
#define KERNEL_H

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/serial.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/cputhrottle.h>
#include <circle/device.h>
#include <circle/usb/usbhcidevice.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>
#include <circle/types.h>
#include "video.h"
#include "input.h"
#include "audio.h"
#include "ui.h"

enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
};

// Receives Circle's own log messages and forwards them to the console
class CLogSink : public CDevice
{
public:
	int Write (const void *pBuffer, size_t nCount);
};

class CKernel
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);
	TShutdownMode Run (void);

	// Console (screen during boot + serial + LOG.TXT)
	void Log (const char *pLine, u32 nColor);
	void Fatal (int nCode, const char *pMessage);
	// bBackground: written by core 3 while the game goes on
	void SaveLog (boolean bBackground = FALSE);

	// called by the core at its vblank (inside core_frame): picture out,
	// then the controls, as late as the vsync allows
	void CoreVBlank (void);
	// called by the core with each quarter of the frame's sound: queues it
	// and holds the emulation until the block's deadline
	void CoreAudioBlock (const int16_t *pSamples, unsigned nFrames, unsigned nBlock);

	static CKernel *Get (void) { return s_pThis; }

private:
	void RedrawConsole (void);
	char *LoadText (const char *pPath, size_t *pSize);
	void ScanGames (void);
	const char *TitleOf (const char *pName) const;
	boolean LoadGame (unsigned nIndex);
	void UnloadGame (void);
	void Benchmark (void);
	unsigned RunList (unsigned nSel);
	void RunGame (void);
	void ShowUIFrame (boolean bWait = TRUE, const char *pTopMessage = 0);
	void WriteTextFile (const char *pPath, const char *pText);
	const char *SaveScreenshot (void);
	void RunCredits (void);
	void ResetGameStats (void);
	void ReportWindow (boolean bFinal);
	static const char *FilterName (unsigned nFilter);
	void ApplyVideoMode (void);
	void VideoModeTag (char *pOut, size_t nSize);
	static const char *FilterShort (unsigned nFilter);
	void LoadConfig (void);
	void SaveConfig (void);
	void LoadOverlay (void);
	void RunSystemMenu (void);
	boolean PowerOffCountdown (void);
	void LoadPads (void);
	void SavePads (void);
	int ChoosePad (void);
	boolean RunMapping (unsigned nSlot, boolean bOverGame);
	void PowerOff (void);

private:
	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CSerialDevice		m_Serial;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogSink		m_LogSink;
	CLogger			m_Logger;
	CCPUThrottle		m_CPUThrottle;
	CUSBHCIDevice		m_USBHCI;
	CEMMCDevice		m_EMMC;
	FATFS			m_FileSystem;
	CVideoOut		m_Video;
	CAudioOut		m_Audio;
	CInput			*m_pInput;
	CUI			*m_pUI;

	// the log keeps its first HeadLines lines (the boot) for good and the
	// last MaxLines - HeadLines after them
	enum { MaxLines = 800, HeadLines = 120, MaxLineLen = 140, MaxGames = 256 };
	unsigned Slot (unsigned nLine) const
	{
		return nLine < HeadLines ? nLine : HeadLines + (nLine - HeadLines) % (MaxLines - HeadLines);
	}

	boolean			m_bVideoOK;
	boolean			m_bSDOK;
	boolean			m_bQuiet;	// UI/emulation running: console not drawn
	char			m_LastRaw[MaxLineLen];
	unsigned		m_nRepeat;

	// console lines
	char			m_Lines[MaxLines][MaxLineLen];
	u32			m_LineColor[MaxLines];
	unsigned		m_nLines;

	// games
	TGameEntry		*m_pGames;
	unsigned		 m_nGames;
	int			 m_nLoaded;	// index in m_pGames, -1 = none
	char			*m_pTitles;	// zipname.cps1 text

	// frame pacing / statistics
	boolean			 m_bVSync;
	boolean			 m_bStatusOn;
	unsigned		 m_nClearStatus;
	unsigned		 m_nShots;

	// configuration (cpspi.cfg)
	unsigned		 m_nFilter;	// FILTER_*
	boolean			 m_bHVS;		// FALSE: scaler=cpu in cpspi.cfg
	unsigned		 m_nAspect;	// ASPECT_* (4:3, 4:3 sharp, square pixels)
	unsigned		 m_nInputMarginMin;	// controls read at least this long before the vsync (us; 0 = auto)
	boolean			 m_bMultiCore;	// FALSE: multicore=off, core 0 draws alone
	boolean			 m_bOverlay;	// the bezel (cpspi/overlay.png) around the picture
	u8			*m_pOverlayRGB;	// the decoded bezel
	// 68000 share of its clock (percent, 0 = the core's per-game default):
	// cpu=NN for every game, cpu.<set>=NN per set (also set from the menu)
	unsigned		 m_nCpuPercent;
	enum { MaxCpuSets = 32 };
	struct { char Set[16]; unsigned nPercent; } m_CpuSets[MaxCpuSets];
	unsigned		 m_nCpuSets;
	unsigned CpuPercentFor (const char *pSet) const;
	void SetCpuPercentFor (const char *pSet, unsigned nPercent);

	static CKernel		*s_pThis;
};

#endif
