/*
 * DEFLATE/gzip COMPRESSOR round-trip test (host-side, ASan/UBSan).
 *
 * The kernel's gz_deflate() (kernel/deflate.c) is the encoding counterpart to
 * the inflate()/gz_inflate() decoder (kernel/inflate.c). This test proves the
 * compressor produces VALID gzip streams by round-tripping: for a wide range of
 * inputs (empty, all-same, runs, random, structured text, sizes 0..~200 KB) it
 *   1. compresses with gz_deflate(),
 *   2. decompresses the gzip stream with gz_inflate(), and
 *   3. asserts the result EXACTLY equals the original (length + every byte).
 * It also confirms gz_deflate() honors a too-small outcap (returns -1 with no
 * overflow — ASan would abort on any OOB) and that the CRC-32/ISIZE round-trip.
 *
 * Build: see tests/run-deflate-tests.sh ("make deftest"). Clean exit = PASS;
 * any OOB/overflow aborts under -fsanitize=address,undefined.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Compressor under test (kernel/deflate.c). */
int gz_deflate (const uint8_t *, int, uint8_t *, int);
int raw_deflate(const uint8_t *, int, uint8_t *, int);
/* Reference decoder (kernel/inflate.c). */
int gz_inflate (const uint8_t *, int, uint8_t *, int);
int inflate    (const uint8_t *, int, uint8_t *, int);

/* Worst-case fixed-Huffman expansion is < 9/8 + small; allocate generously. */
#define CAP (300u << 10)              /* ~307 KB, > 200 KB * 9/8 + overhead */
static uint8_t in_buf [CAP];
static uint8_t gz_buf [CAP + 4096];
static uint8_t out_buf[CAP];

static uint32_t rs = 0x12345678u;
static uint32_t xr(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static int fails = 0;

/* Round-trip one input through gz_deflate -> gz_inflate and compare exactly. */
static void roundtrip(const uint8_t *src, int len, const char *what) {
    int gz = gz_deflate(src, len, gz_buf, (int)sizeof gz_buf);
    if (gz < 0) {
        printf("  FAIL [%s] gz_deflate returned -1 (len=%d)\n", what, len);
        fails++;
        return;
    }
    /* gzip framing sanity. */
    if (gz < 18 || gz_buf[0] != 0x1f || gz_buf[1] != 0x8b || gz_buf[2] != 0x08) {
        printf("  FAIL [%s] bad gzip header / too short (gz=%d)\n", what, gz);
        fails++;
        return;
    }
    int dec = gz_inflate(gz_buf, gz, out_buf, (int)sizeof out_buf);
    if (dec != len) {
        printf("  FAIL [%s] length mismatch: in=%d out=%d (gz=%d)\n",
               what, len, dec, gz);
        fails++;
        return;
    }
    for (int i = 0; i < len; i++) {
        if (out_buf[i] != src[i]) {
            printf("  FAIL [%s] byte %d: got 0x%02x want 0x%02x\n",
                   what, i, out_buf[i], src[i]);
            fails++;
            return;
        }
    }
    /* Also exercise the raw-deflate helper + raw inflate() on the same data. */
    int rd = raw_deflate(src, len, gz_buf, (int)sizeof gz_buf);
    if (rd < 0) { printf("  FAIL [%s] raw_deflate -1\n", what); fails++; return; }
    int rdec = inflate(gz_buf, rd, out_buf, (int)sizeof out_buf);
    if (rdec != len) {
        printf("  FAIL [%s] raw length mismatch: in=%d out=%d\n", what, len, rdec);
        fails++; return;
    }
    for (int i = 0; i < len; i++) {
        if (out_buf[i] != src[i]) {
            printf("  FAIL [%s] raw byte %d differs\n", what, i); fails++; return;
        }
    }
}

/* Verify gz_deflate refuses to overflow: for a compressible-but-nontrivial
 * input, every outcap from 0 up to (correct_size - 1) must return -1, and ASan
 * must not catch any write past the buffer end. We place the output buffer at
 * the END of a heap allocation so any overrun is immediately OOB. */
static void check_bounds(const uint8_t *src, int len) {
    int full = gz_deflate(src, len, gz_buf, (int)sizeof gz_buf);
    if (full < 0) { printf("  FAIL bounds: full encode failed\n"); fails++; return; }

    for (int cap = 0; cap < full; cap++) {
        /* Exact-size heap buffer => ASan red-zones immediately after `cap`. */
        uint8_t *tight = malloc(cap ? (size_t)cap : 1);
        int r = gz_deflate(src, len, tight, cap);
        if (r != -1) {
            printf("  FAIL bounds: cap=%d returned %d (expected -1)\n", cap, r);
            fails++;
            free(tight);
            return;
        }
        free(tight);
    }
    /* Exact capacity must succeed and reproduce the same size. */
    uint8_t *exact = malloc((size_t)full);
    int r = gz_deflate(src, len, exact, full);
    if (r != full) { printf("  FAIL bounds: exact cap got %d want %d\n", r, full); fails++; }
    free(exact);
    printf("  bounds: outcap 0..%d all returned -1, exact=%d OK\n", full - 1, full);
}

int main(void) {
    printf("deflate/gzip compressor round-trip test\n");

    /* 1. Empty input. */
    roundtrip(in_buf, 0, "empty");

    /* 2. Single byte and a few tiny sizes (exercise <MIN_MATCH tail paths). */
    for (int n = 1; n <= 8; n++) {
        for (int i = 0; i < n; i++) in_buf[i] = (uint8_t)(0x41 + i);
        roundtrip(in_buf, n, "tiny");
    }

    /* 3. All-same-byte (maximal run -> long matches at distance 1). */
    for (int n = 1; n <= 200000; n = n < 256 ? n + 17 : n * 2 + 1) {
        if (n > (int)CAP) break;
        for (int i = 0; i < n; i++) in_buf[i] = 0xAB;
        roundtrip(in_buf, n, "all-same");
    }

    /* 4. Highly repetitive period-N patterns (matches at various distances). */
    for (int period = 1; period <= 300; period += 37) {
        int n = 50000;
        for (int i = 0; i < n; i++) in_buf[i] = (uint8_t)((i % period) * 7 + 3);
        roundtrip(in_buf, n, "periodic");
    }

    /* 5. Pure random bytes (incompressible; forces mostly literals). */
    for (int t = 0; t < 6; t++) {
        int n = 1 + (int)(xr() % 100000);
        for (int i = 0; i < n; i++) in_buf[i] = (uint8_t)xr();
        roundtrip(in_buf, n, "random");
    }

    /* 6. Structured text-like data (mix of runs, words, and entropy). */
    {
        static const char *words[] = {
            "the ", "quick ", "brown ", "fox ", "jumps ", "over ",
            "lazy ", "dog ", "deflate ", "kernel ", "hello world ",
        };
        for (int n = 100; n <= 200000; n *= 3) {
            if (n > (int)CAP) break;
            int p = 0;
            while (p < n) {
                const char *wd = words[xr() % 11];
                while (*wd && p < n) in_buf[p++] = (uint8_t)*wd++;
            }
            roundtrip(in_buf, n, "text");
        }
    }

    /* 7. Sizes sweep around block/hash edge cases with semi-random content. */
    for (int n = 0; n <= 4200; n++) {
        for (int i = 0; i < n; i++)
            in_buf[i] = (uint8_t)((i * 31 + (xr() & 0x0f)));
        roundtrip(in_buf, n, "sweep");
    }

    /* 8. A near-200 KB mixed buffer: alternating random and repeated regions. */
    {
        int n = 199999;
        for (int i = 0; i < n; i++) {
            if ((i / 1000) & 1) in_buf[i] = (uint8_t)xr();
            else                in_buf[i] = (uint8_t)(i & 1 ? 0x55 : 0x55);
        }
        roundtrip(in_buf, n, "mixed-200k");
    }

    /* 9. Adversarial for the match finder: many identical 3-byte hashes spread
     *    across the whole window (stresses the bounded chain walk). */
    {
        int n = 100000;
        for (int i = 0; i < n; i++) in_buf[i] = (uint8_t)(i & 1);   /* 0,1,0,1,... */
        roundtrip(in_buf, n, "hash-collide");
    }

    /* 10. Bounds / outcap enforcement on a representative compressible input. */
    {
        int n = 5000;
        const char *pat = "compress me please, ";
        int p = 0;
        while (p < n) { const char *q = pat; while (*q && p < n) in_buf[p++] = (uint8_t)*q++; }
        check_bounds(in_buf, n);
    }
    /* Bounds on random (incompressible) input too. */
    {
        int n = 1500;
        for (int i = 0; i < n; i++) in_buf[i] = (uint8_t)xr();
        check_bounds(in_buf, n);
    }
    /* Bounds on empty input (header+trailer = 18 bytes minimum). */
    check_bounds(in_buf, 0);

    if (fails) {
        printf("\nFAILED: %d case(s) failed\n", fails);
        return 1;
    }
    printf("\nPASS: all round-trips exact, bounds enforced, ASan/UBSan clean\n");
    return 0;
}
