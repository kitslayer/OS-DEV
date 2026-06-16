/*
 * test_wav.c — host-side regression + fuzz test for the WAV header parser
 * (kernel/wav.c), under AddressSanitizer + UndefinedBehaviorSanitizer.
 *
 * wav_parse walks untrusted RIFF chunk data, so the fuzz pass feeds it
 * truncations, single-byte corruptions and random buffers and asserts it never
 * reads out of bounds (ASan) and that any "success" stays within the buffer.
 */
#include "wav.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void put32(unsigned char *p, unsigned v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put16(unsigned char *p, unsigned v) { p[0]=v; p[1]=v>>8; }

/* Build a minimal valid 16-bit PCM WAV with `dbytes` of data into buf; returns total length. */
static int make_wav(unsigned char *buf, int ch, int rate, int dbytes) {
    memcpy(buf, "RIFF", 4);  put32(buf+4, 36 + dbytes);  memcpy(buf+8, "WAVE", 4);
    memcpy(buf+12, "fmt ", 4); put32(buf+16, 16);
    put16(buf+20, 1); put16(buf+22, ch); put32(buf+24, rate);
    put32(buf+28, rate*ch*2); put16(buf+32, ch*2); put16(buf+34, 16);
    memcpy(buf+36, "data", 4); put32(buf+40, dbytes);
    for (int i = 0; i < dbytes; i++) buf[44+i] = (unsigned char)i;
    return 44 + dbytes;
}

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* If parse succeeds, the reported PCM span must lie inside the buffer. */
static void check_contract(const unsigned char *d, int len) {
    int ch, rate, bits; long off, plen;
    if (wav_parse(d, len, &ch, &rate, &bits, &off, &plen) == 0) {
        CHECK(off >= 0 && plen >= 0);
        CHECK(off + plen <= len);
        CHECK(ch >= 1 && ch <= 2 && bits == 16 && rate > 0);
    }
}

int main(void) {
    unsigned char buf[4096];

    /* 1. a valid WAV parses with the right fields */
    int n = make_wav(buf, 2, 48000, 16);
    int ch, rate, bits; long off, plen;
    CHECK(wav_parse(buf, n, &ch, &rate, &bits, &off, &plen) == 0);
    CHECK(ch == 2 && rate == 48000 && bits == 16 && off == 44 && plen == 16);

    /* mono + odd rate */
    n = make_wav(buf, 1, 11025, 64);
    CHECK(wav_parse(buf, n, &ch, &rate, &bits, &off, &plen) == 0 && ch == 1 && rate == 11025);

    /* 2. truncations: every prefix of a valid WAV must be safe (and in-contract) */
    n = make_wav(buf, 2, 44100, 200);
    for (int L = 0; L <= n; L++) check_contract(buf, L);

    /* 3. single-byte corruptions at every position */
    for (int p = 0; p < n; p++) {
        unsigned char save = buf[p];
        for (int v = 0; v < 256; v += 17) { buf[p] = (unsigned char)v; check_contract(buf, n); }
        buf[p] = save;
    }

    /* 4. adversarial chunk sizes (huge / negative-looking) */
    n = make_wav(buf, 2, 48000, 32);
    put32(buf+40, 0xFFFFFFFFu);  check_contract(buf, n);   /* data size > buffer */
    put32(buf+16, 0xFFFFFFFFu);  check_contract(buf, n);   /* fmt  size > buffer */
    put32(buf+16, 0);            check_contract(buf, n);   /* fmt  size 0 */

    /* 5. random buffers */
    srand(1234);
    for (int it = 0; it < 200000; it++) {
        int L = rand() % 300;
        for (int i = 0; i < L; i++) buf[i] = (unsigned char)rand();
        if (it % 3 == 0 && L >= 12) { memcpy(buf, "RIFF", 4); memcpy(buf+8, "WAVE", 4); }  /* pass the magic sometimes */
        check_contract(buf, L);
    }

    if (fails) { printf("wavtest: %d FAILURES\n", fails); return 1; }
    printf("wavtest: all checks passed\n");
    return 0;
}
