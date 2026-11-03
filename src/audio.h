/*
 * neo-pi - HDMI audio output with a small queue and drift correction
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <circle/sound/hdmisoundbasedevice.h>
#include <circle/interrupt.h>
#include <circle/types.h>

class CAudioOut
{
public:
	CAudioOut (void);
	~CAudioOut (void);

	boolean Initialize (CInterruptSystem *pInterrupt);
	boolean IsOK (void) const	{ return m_bOK; }

	// Queue a block of interleaved stereo samples (a frame, or part of one)
	void WriteFrame (const s16 *pSamples, unsigned nFrames);
	// Silence up to the target level (before a game starts: no start-up gap)
	void Prime (void);
	// Level (frames) the queue is kept at before the writes: the longest
	// gap between writes plus a DMA chunk, decided by the game loop
	void SetTarget (unsigned nFrames);
	unsigned GetTarget (void) const		{ return m_nTarget; }

	// Statistics since the last call
	void GetStats (unsigned *pMinQueue, unsigned *pMaxQueue, unsigned *pUnderruns,
		       int *pAdjust);

private:
	CHDMISoundBaseDevice *m_pSound;
	boolean	 m_bOK;
	s16	 m_Buffer[4096];
	unsigned m_nTarget;
	unsigned m_Recent[4];			// levels before the last writes (one frame)
	unsigned m_nRecent;
	int	 m_nAdjAccum;			// correction carried between blocks (1/400 frame)

	unsigned m_nMinQueue, m_nMaxQueue, m_nUnderruns;
	int	 m_nAdjust;
};

#endif
