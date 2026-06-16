/* ac97.h — Intel 82801AA AC'97 audio: PCM playback. */
#pragma once
#include <stdint.h>

int  ac97_init(void);   /* detect + init the codec; 0 ok, -1 if absent */
int  ac97_ready(void);  /* 1 if a device was initialised */

/* Play `nframes` interleaved 16-bit stereo frames at 48 kHz, blocking until the
 * sound has finished. Safe to call with the device absent (no-op). */
void ac97_play(const int16_t *frames, int nframes);

/* Parse a RIFF/WAVE buffer (16-bit PCM, mono or stereo, any rate), resample to
 * 48 kHz stereo, and play it (blocking). Returns 0 on success, -1 on a bad WAV. */
int  ac97_play_wav(const uint8_t *data, int len);

/* Background (non-blocking) playback: decode + stream a WAV from the timer pump,
 * so it keeps playing while other apps run. ac97_play_bg takes ownership of pcm. */
int  ac97_play_wav_bg(const uint8_t *data, int len);
void ac97_play_bg(int16_t *pcm, long nframes);
void ac97_stop_bg(void);

/* Non-blocking streaming: queue 48 kHz stereo frames into a ring the timer IRQ
 * feeds to the device continuously (DOOM's mixer uses this). */
void ac97_stream_start(void);
void ac97_stream_stop(void);
int  ac97_stream_write(const int16_t *frames, int nframes);  /* queue; returns accepted */
int  ac97_stream_avail(void);                                /* free frames in the ring */
void ac97_pump(void);                                        /* timer-IRQ refill hook */
