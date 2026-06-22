/* hda.h — Intel High Definition Audio (HDA) controller: PCM playback.
 *
 * The modern counterpart to ac97.c. Where AC'97 is a fixed pair of port-I/O
 * register windows, an HDA controller is an MMIO device that talks to one or
 * more *codecs* over a serial link, each codec exposing a graph of widgets
 * (DACs, pins, mixers, ...). Bring-up is therefore much more involved: reset
 * the controller, send the codec a sequence of "verbs" to wire an output
 * converter to an output pin, then drive a Buffer Descriptor List (BDL) over a
 * stream descriptor to DMA samples out — see hda.c for the full sequence.
 *
 * The public surface mirrors ac97.h exactly (same names minus the prefix) so
 * the audio.c dispatcher can bind either driver behind one set of function
 * pointers and SYS_pcm / playwav / DOOM's mixer don't care which answered.
 */
#pragma once
#include <stdint.h>

int  hda_init(void);    /* detect + bring up an HDA codec output path; 0 ok, -1 if absent */
int  hda_ready(void);   /* 1 if a controller+codec was initialised */

/* Bring-up self-test (no-op unless HDA is up): play a brief tone and confirm the
 * output stream's DMA position register (LPIB) advances — proof the controller
 * is really streaming. Logs the before/after LPIB to serial. Like ahci_selftest. */
void hda_selftest(void);

/* Play `nframes` interleaved 16-bit stereo frames at 48 kHz, blocking until the
 * sound has finished. Safe to call with the device absent (no-op). */
void hda_play(const int16_t *frames, int nframes);

/* Non-blocking streaming: queue 48 kHz stereo frames into a cyclic ring the
 * timer IRQ keeps feeding to the stream engine (the same model ac97.c uses, so
 * DOOM's mixer / background music work unchanged through the audio.c seam). */
void hda_stream_start(void);
void hda_stream_stop(void);
int  hda_stream_write(const int16_t *frames, int nframes);  /* queue; returns accepted */
int  hda_stream_avail(void);                                /* free frames in the ring */
void hda_pump(void);                                        /* timer-IRQ refill hook */
