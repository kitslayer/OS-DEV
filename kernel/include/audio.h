/*
 * audio.h — the audio-output seam between the kernel's PCM path (SYS_pcm /
 * playwav / playbg, DOOM's mixer) and a concrete codec driver (hda.c or ac97.c).
 *
 * Exactly the NIC dispatcher (nic.c) idea applied to sound: the syscall layer
 * and the timer pump call audio_* and don't care which chip moves the samples.
 * audio_init() probes the PCI bus and binds these calls to whichever audio
 * controller is present — the Intel HD Audio controller when one is on the bus
 * (the richer, modern part), else the AC'97 codec the default QEMU config and
 * the headless test suite boot with. The high-level helpers (WAV decode +
 * background music) live in audio.c, built once on the driver's PCM primitives,
 * so adding a third codec is just its driver + one probe line here.
 */
#pragma once
#include <stdint.h>

/* Probe the PCI bus for a supported audio controller and bring it up. Prefers
 * Intel HD Audio; falls back to AC'97. Returns 0 if a device came up, -1 if no
 * supported audio controller is present (every call below then no-ops safely). */
int  audio_init(void);
int  audio_ready(void);              /* 1 if a device was initialised */
const char *audio_name(void);        /* "hda", "ac97", or "none" — for the banner */

/* Play `nframes` interleaved 16-bit stereo frames at 48 kHz, blocking until the
 * sound has finished. */
void audio_play(const int16_t *frames, int nframes);

/* Decode a 16-bit PCM WAV (resampled to 48 kHz stereo) and play it. */
int  audio_play_wav(const uint8_t *data, int len);          /* blocking;  0/-1 */
int  audio_play_wav_bg(const uint8_t *data, int len);       /* background; 0/-1 */
void audio_stop_bg(void);                                   /* stop background */

/* Non-blocking streaming ring (DOOM's mixer). */
int  audio_stream_write(const int16_t *frames, int nframes); /* queue; accepted */
int  audio_stream_avail(void);                               /* free frames     */
void audio_pump(void);                                       /* timer-IRQ hook  */
