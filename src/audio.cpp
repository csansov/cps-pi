/*
 * neo-pi - HDMI audio output
 *
 * The emulated frame's sound arrives in four blocks of about 200 stereo
 * frames at 48 kHz (from the resampler in njemu_pi.c), as the game loop
 * paces the emulation through the frame. The display and the HDMI audio
 * clock are not exactly locked, so the queue level is kept around a target
 * by stretching or shrinking the blocks by a quarter of a percent (linear
 * interpolation, inaudible); a full percent only when a gap is imminent.
 *
 * Latency: the DMA takes CHUNK_SIZE / 2 frames at a time, so the queue must
 * never hold less than that when a chunk is fetched; between two writes it
 * drains by the gap between them, so the level before a write is the low
 * point and the target is the longest gap (measured by the game loop, which
 * sets it) plus a chunk and a little. The level is judged on the lowest of
 * the last four writes, a frame's worth, since the gaps within a frame are
 * not equal. With the blocks paced, the target settles around 500-600
 * frames: about 15 ms from the chip to the HDMI output.
 */
#include "audio.h"
#include <circle/util.h>

#define SAMPLE_RATE	48000
#define CHUNK_SIZE	(384 * 1)		// words per DMA chunk (multiple of 384): 192 frames, 4 ms
#define QUEUE_FRAMES	4800			// 100 ms queue capacity
#define TARGET_FRAMES	1500			// until the game loop has measured its gaps
#define BAND_FRAMES	100
#define STRONG_LOW	280			// below: 1% stretch (gap imminent)
#define STRONG_OVER	1000			// this much above the target: 1% shrink

CAudioOut::CAudioOut (void)
:	m_pSound (0),
	m_bOK (FALSE),
	m_nTarget (TARGET_FRAMES),
	m_nRecent (0),
	m_nAdjAccum (0),
	m_nMinQueue (~0u),
	m_nMaxQueue (0),
	m_nUnderruns (0),
	m_nAdjust (0)
{
}

CAudioOut::~CAudioOut (void)
{
	delete m_pSound;
}

boolean CAudioOut::Initialize (CInterruptSystem *pInterrupt)
{
	m_pSound = new CHDMISoundBaseDevice (pInterrupt, SAMPLE_RATE, CHUNK_SIZE);
	if (m_pSound == 0)
	{
		return FALSE;
	}
	if (!m_pSound->AllocateQueueFrames (QUEUE_FRAMES))
	{
		return FALSE;
	}
	m_pSound->SetWriteFormat (SoundFormatSigned16, 2);
	if (!m_pSound->Start ())
	{
		return FALSE;
	}
	m_bOK = TRUE;
	return TRUE;
}

void CAudioOut::Prime (void)
{
	if (!m_bOK)
	{
		return;
	}
	unsigned nQueued = m_pSound->GetQueueFramesAvail ();
	if (nQueued >= m_nTarget)
	{
		return;
	}
	memset (m_Buffer, 0, sizeof m_Buffer);
	unsigned nLeft = m_nTarget - nQueued;
	while (nLeft > 0)
	{
		unsigned n = nLeft < 2048 ? nLeft : 2048;
		m_pSound->Write (m_Buffer, n * 4);
		nLeft -= n;
	}
	for (unsigned i = 0; i < 4; i++) m_Recent[i] = m_nTarget;
	m_nAdjAccum = 0;
}

void CAudioOut::SetTarget (unsigned nFrames)
{
	if (nFrames < 300) nFrames = 300;
	if (nFrames > 2400) nFrames = 2400;
	m_nTarget = nFrames;
}

void CAudioOut::WriteFrame (const s16 *pSamples, unsigned nFrames)
{
	if (!m_bOK || nFrames == 0 || nFrames > 1800)
	{
		return;
	}

	unsigned nQueued = m_pSound->GetQueueFramesAvail ();
	if (nQueued < m_nMinQueue) m_nMinQueue = nQueued;
	if (nQueued > m_nMaxQueue) m_nMaxQueue = nQueued;
	if (nQueued == 0)
	{
		m_nUnderruns++;
	}
	m_Recent[m_nRecent++ & 3] = nQueued;
	unsigned nLow = nQueued;
	for (unsigned i = 0; i < 4; i++)
	{
		if (m_Recent[i] < nLow) nLow = m_Recent[i];
	}

	// correction in 1/400 of the block: 1 = 0.25% (inaudible) while the low
	// point is off the band, 4 = 1% only when a gap is imminent or the queue
	// is far too full; carried between blocks so that small blocks get it too
	int nRate = 0;
	if (nLow + BAND_FRAMES < m_nTarget)
	{
		nRate = nLow < STRONG_LOW ? 4 : 1;			// stretch
	}
	else if (nLow > m_nTarget + BAND_FRAMES)
	{
		nRate = nLow > m_nTarget + STRONG_OVER ? -4 : -1;	// shrink
	}
	m_nAdjAccum += (int) nFrames * nRate;
	int nDelta = m_nAdjAccum / 400;
	m_nAdjAccum -= nDelta * 400;
	m_nAdjust += nDelta;

	unsigned nOut = nFrames + nDelta;
	if (nDelta == 0 || nOut < 2)
	{
		m_pSound->Write (pSamples, nFrames * 4);
		return;
	}

	// resample nFrames -> nOut (16.16 fixed point, linear)
	u32 nStep = (u32) (((u64) (nFrames - 1) << 16) / (nOut - 1));
	u32 nPos = 0;
	for (unsigned i = 0; i < nOut; i++)
	{
		unsigned k = nPos >> 16;
		unsigned f = nPos & 0xFFFF;
		unsigned k1 = k + 1 < nFrames ? k + 1 : k;
		for (unsigned c = 0; c < 2; c++)
		{
			int a = pSamples[k * 2 + c];
			int b = pSamples[k1 * 2 + c];
			m_Buffer[i * 2 + c] = (s16) (a + (((b - a) * (int) f) >> 16));
		}
		nPos += nStep;
	}
	m_pSound->Write (m_Buffer, nOut * 4);
}

void CAudioOut::GetStats (unsigned *pMinQueue, unsigned *pMaxQueue, unsigned *pUnderruns,
			  int *pAdjust)
{
	*pMinQueue = m_nMinQueue == ~0u ? 0 : m_nMinQueue;
	*pMaxQueue = m_nMaxQueue;
	*pUnderruns = m_nUnderruns;
	*pAdjust = m_nAdjust;
	m_nMinQueue = ~0u;
	m_nMaxQueue = 0;
	m_nUnderruns = 0;
	m_nAdjust = 0;
}
