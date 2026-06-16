/*
 * wav.c — a bounds-checked RIFF/WAVE (PCM) header parser.
 *
 * Split out from the AC'97 driver so it can be fuzzed on the host (tests/wav):
 * it parses untrusted file bytes, so it walks the RIFF chunk list clamping every
 * chunk size to the buffer and validates the format before reporting where the
 * PCM samples live. No allocation, no device access — pure parsing.
 */
#include "wav.h"

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int wav_parse(const uint8_t *d, int len, int *channels, int *rate, int *bits,
              long *pcm_off, long *pcm_len) {
    if (len < 12) return -1;
    if (d[0] != 'R' || d[1] != 'I' || d[2] != 'F' || d[3] != 'F' ||
        d[8] != 'W' || d[9] != 'A' || d[10] != 'V' || d[11] != 'E') return -1;

    int  ch = 0, rt = 0, bt = 0;
    long poff = -1, plen = 0;
    int  off = 12;
    while (off + 8 <= len) {                              /* walk the RIFF chunks */
        const uint8_t *id = d + off;
        long body = off + 8;
        uint32_t csz = rd32(d + off + 4);
        if (csz > (uint32_t)(len - body)) csz = (uint32_t)(len - body);   /* clamp to buffer */
        if (id[0]=='f'&&id[1]=='m'&&id[2]=='t'&&id[3]==' ' && csz >= 16) {
            ch = rd16(d + body + 2);
            rt = (int)rd32(d + body + 4);
            bt = rd16(d + body + 14);
        } else if (id[0]=='d'&&id[1]=='a'&&id[2]=='t'&&id[3]=='a') {
            poff = body; plen = csz;
        }
        long next = body + csz + (csz & 1);               /* chunks are word-aligned */
        if (next <= off) break;                           /* never go backwards (overflow guard) */
        off = (int)next;
    }
    if (poff < 0 || bt != 16 || ch < 1 || ch > 2 || rt <= 0) return -1;
    *channels = ch; *rate = rt; *bits = bt; *pcm_off = poff; *pcm_len = plen;
    return 0;
}
