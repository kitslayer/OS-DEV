/* wav.h — a bounds-checked RIFF/WAVE (PCM) header parser. */
#pragma once
#include <stdint.h>

/* Parse a RIFF/WAVE buffer. On a 16-bit PCM WAV, returns 0 and fills the format
 * plus the byte offset and length of the PCM `data` chunk (clamped to the
 * buffer); returns -1 on anything malformed or unsupported. Safe for arbitrary
 * untrusted input — every read is bounds-checked. */
int wav_parse(const uint8_t *data, int len, int *channels, int *rate, int *bits,
              long *pcm_off, long *pcm_len);
