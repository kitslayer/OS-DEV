/* ac97.h — Intel 82801AA AC'97 audio: PCM playback. */
#pragma once
#include <stdint.h>

int  ac97_init(void);   /* detect + init the codec; 0 ok, -1 if absent */
int  ac97_ready(void);  /* 1 if a device was initialised */

/* Play `nframes` interleaved 16-bit stereo frames at 48 kHz, blocking until the
 * sound has finished. Safe to call with the device absent (no-op). */
void ac97_play(const int16_t *frames, int nframes);
