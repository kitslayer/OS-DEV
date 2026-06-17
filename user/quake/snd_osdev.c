/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
//
// snd_osdev.c -- SNDDMA_* sound-output backend for this OS.
//
// Quake's engine (snd_dma.c / snd_mix.c) owns the mixer: it mixes active
// channels into shm->buffer, treating it as a ring of (shm->samples/2) stereo
// frames, and asks this backend two things:
//
//   * SNDDMA_GetDMAPos() -- where is the play cursor right now (in samples,
//                           i.e. frames*channels, mod shm->samples)?  The
//                           engine divides this by channels, tracks ring
//                           wraps, and mixes AHEAD of it.
//   * SNDDMA_Submit()    -- the engine just painted up to `paintedtime`; ship
//                           the newly-painted frames to real hardware.
//
// We keep shm in the OS's native streaming format -- 48000 Hz, 16-bit, stereo
// -- so shm->buffer already holds exactly what the kernel ring wants and a
// frame is a straight 4-byte copy (no resample / format conversion).  Frames
// are pushed into the kernel's ~1-second PCM ring via sys_pcm_stream(), which
// is NON-BLOCKING and drops whatever doesn't fit.  That drop is our clock: the
// ring drains at a real 48 kHz, so the play cursor we report (advanced only by
// the frames the ring actually accepted) tracks real playback, and the engine
// neither over- nor under-runs.
//

#include "quakedef.h"

// ---- syscalls from ulib (declared here; resolved at link against user_ulib.o,
// matching the pattern in quakegeneric_osdev.c -- ulib.h is outside our -I path).
extern int sys_pcm_stream(const void *frames, int nframes);
extern int sys_pcm_avail(void);

// shm->buffer is a ring of this many 16-bit STEREO frames.  Power of two so the
// engine's S_TransferStereo16 mask ((shm->samples>>1)-1) is valid.  65536 / 2 =
// 32768 frames (~0.68 s) -- comfortably inside the kernel's 1 s ring and larger
// than the engine's mix-ahead cap (shm->samples>>1 frames).
#define SND_BUFFER_SAMPLES	65536		// total samples = frames * channels
#define SND_SPEED			48000
#define SND_CHANNELS		2
#define SND_BITS			16

static unsigned char	*dma_buffer;			// == shm->buffer (we own it)
static int				dma_frames;			// play cursor, in stereo frames
static int				submitted_frames;		// frames already shipped to ring

#define RING_FRAMES		(SND_BUFFER_SAMPLES / SND_CHANNELS)		// frames in shm->buffer


/*
==================
SNDDMA_Init

Set up the DMA description (shm) the engine mixes into, allocate the sample
buffer, and reset our cursors.  Returns true on success; on failure the engine
treats it as "no sound" and keeps running silently.
==================
*/
qboolean SNDDMA_Init(void)
{
	dma_buffer = (unsigned char *) malloc(SND_BUFFER_SAMPLES * (SND_BITS / 8));
	if (!dma_buffer)
		return false;

	memset(dma_buffer, 0, SND_BUFFER_SAMPLES * (SND_BITS / 8));

	dma_frames = 0;
	submitted_frames = 0;

	// Fill in the shared DMA description the engine reads.  Keeping speed/
	// channels/samplebits at the OS's native streaming format means shm->buffer
	// is already 48 kHz 16-bit stereo, so SNDDMA_Submit is a straight copy.
	shm = &sn;
	memset((void *) shm, 0, sizeof(*shm));
	shm->splitbuffer = 0;
	shm->channels = SND_CHANNELS;
	shm->samplebits = SND_BITS;
	shm->speed = SND_SPEED;
	shm->samples = SND_BUFFER_SAMPLES;		// total samples (frames * channels)
	shm->samplepos = 0;
	shm->soundalive = true;
	shm->gamealive = true;
	shm->submission_chunk = 1;
	shm->buffer = dma_buffer;

	return true;
}


/*
==================
SNDDMA_GetDMAPos

Return the current play cursor as a sample count (frames*channels), mod
shm->samples.  The engine (GetSoundtime) divides this by channels and tracks
ring wraparound itself, so we just expose where playback currently sits.
==================
*/
int SNDDMA_GetDMAPos(void)
{
	if (!shm)
		return 0;

	shm->samplepos = (dma_frames * SND_CHANNELS) % shm->samples;
	return shm->samplepos;
}


/*
==================
SNDDMA_Submit

The engine has mixed up to `paintedtime` (in stereo frames) into shm->buffer.
Ship every frame between our last-submitted position and paintedtime into the
kernel's PCM ring, handling the ring wraparound of shm->buffer, and advance the
play cursor by however many frames the kernel actually accepted.

sys_pcm_stream() is non-blocking and returns fewer than requested when its ring
is full; that shortfall is exactly the backpressure that paces us to real time.
We never spin -- whatever the ring won't take this frame, we offer again next
frame (paintedtime only climbs as fast as the cursor we report lets it, so the
backlog stays bounded by the engine's mix-ahead).
==================
*/
void SNDDMA_Submit(void)
{
	int		want;			// frames the engine has produced but we haven't shipped
	const short *buf;

	if (!shm || !dma_buffer)
		return;

	buf = (const short *) dma_buffer;

	// paintedtime is monotonic frames mixed; submitted_frames is what we've
	// shipped.  Their difference is the fresh, not-yet-played audio.
	want = paintedtime - submitted_frames;
	if (want <= 0)
		return;					// nothing new to play yet

	// Never offer more than one ring's worth at once (paintedtime can't get
	// more than shm->samples/2 frames ahead of the cursor anyway -- the engine
	// caps endtime there -- but clamp defensively so a wrap can't double-send).
	if (want > RING_FRAMES)
		want = RING_FRAMES;

	while (want > 0)
	{
		int	ring_pos = submitted_frames % RING_FRAMES;	// frame offset in shm->buffer
		int	chunk = RING_FRAMES - ring_pos;				// frames until buffer wraps
		int	pushed;

		if (chunk > want)
			chunk = want;

		// One frame = 2 shorts (L,R); offset into the short buffer is ring_pos*2.
		pushed = sys_pcm_stream(buf + (ring_pos * SND_CHANNELS), chunk);
		if (pushed <= 0)
			break;				// ring full -- try the rest next frame

		submitted_frames += pushed;
		dma_frames       += pushed;		// advance the play cursor in lockstep
		want             -= pushed;

		if (pushed < chunk)
			break;				// ring filled mid-chunk; resume next frame
	}
}


/*
==================
SNDDMA_Shutdown
==================
*/
void SNDDMA_Shutdown(void)
{
	if (dma_buffer)
	{
		free(dma_buffer);
		dma_buffer = NULL;
	}
	shm = NULL;
}


/*
==================
SNDDMA_LockBuffer / SNDDMA_UnlockBuffer

This engine (NetQuake 1.09) drives output purely through GetDMAPos/Submit and
never locks the buffer, so these are unused.  They are provided as trivial
no-ops in case a cross-included path references them.
==================
*/
void SNDDMA_LockBuffer(void)
{
}

void SNDDMA_UnlockBuffer(void)
{
}
