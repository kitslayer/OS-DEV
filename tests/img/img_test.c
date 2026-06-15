/*
 * Image-decoder regression + fuzz test (host-side, ASan/UBSan).
 *
 * The kernel-side image decoders (jpeg/png/gif/inflate) parse UNTRUSTED bytes
 * fetched from the web, with no stack guard page, so an OOB is kernel
 * corruption. This locks the M422 fix (a JPEG DRI marker read 2 bytes past the
 * input on a short segment) and fuzzes all three decoders against adversarial
 * input. Run via tests/run-img-tests.sh ("make imgtest"). A clean exit = pass;
 * any OOB/overflow aborts under ASan/UBSan; a non-terminating decoder hangs.
 */
#include <stdint.h>
#include <stdio.h>

int jpeg_probe (const uint8_t *, int, int *, int *, long *);
int jpeg_decode(const uint8_t *, int, uint8_t *, int, uint8_t *, int, int *, int *);
int png_decode (const uint8_t *, int, uint8_t *, int, uint8_t *, int, int *, int *);
int gif_decode (const uint8_t *, int, uint8_t *, int, uint8_t *, int, int *, int *);

static uint8_t obuf[4u << 20], sbuf[4u << 20];   /* 4 MB each (BSS) */

static uint32_t rs = 0xC0FFEEu;                  /* deterministic xorshift32 */
static uint32_t xr(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static void run_all(const uint8_t *d, int n) {
    int w, h; long need;
    jpeg_probe (d, n, &w, &h, &need);
    jpeg_decode(d, n, obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
    png_decode (d, n, obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
    gif_decode (d, n, obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
}

int main(void) {
    /* 1. M422 regression: a DRI (0xDD) claiming segment length 2 or 3 (body 0/1
     *    bytes) at EOF must NOT make rd16 read past data+len. Without the
     *    seglen>=2 guard, ASan flags an OOB read here. */
    static const uint8_t dri_poc1[] = { 0xFF,0xD8, 0xFF,0xDD, 0x00,0x02 };
    static const uint8_t dri_poc2[] = { 0xFF,0xD8, 0xFF,0xDD, 0x00,0x03, 0x00 };
    run_all(dri_poc1, sizeof dri_poc1);
    run_all(dri_poc2, sizeof dri_poc2);

    /* 2. Truncated / bare-magic headers (must error cleanly, not over-read). */
    static const uint8_t t_jpeg[] = { 0xFF,0xD8 };
    static const uint8_t t_png [] = { 0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A };
    static const uint8_t t_gif [] = { 0x47,0x49,0x46,0x38,0x39,0x61 };
    run_all(t_jpeg, sizeof t_jpeg);
    run_all(t_png,  sizeof t_png);
    run_all(t_gif,  sizeof t_gif);

    /* 3. Deterministic fuzz: random bytes, often prefixed with a real magic so
     *    the probe lets the decode body run, through all three decoders. */
    static const uint8_t mj[] = { 0xFF,0xD8 };
    static const uint8_t mp[] = { 0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A };
    static const uint8_t mg[] = { 0x47,0x49,0x46,0x38,0x39,0x61 };
    uint8_t f[96];
    const int ITERS = 120000;
    for (int i = 0; i < ITERS; i++) {
        int n = 2 + (int)(xr() % 90);            /* 2..91 bytes */
        int pre = 0;
        switch (i & 3) {
            case 0: for (; pre < (int)sizeof mj && pre < n; pre++) f[pre] = mj[pre]; break;
            case 1: for (; pre < (int)sizeof mp && pre < n; pre++) f[pre] = mp[pre]; break;
            case 2: for (; pre < (int)sizeof mg && pre < n; pre++) f[pre] = mg[pre]; break;
            default: break;                       /* pure random */
        }
        for (int j = pre; j < n; j++) f[j] = (uint8_t)xr();
        run_all(f, n);
    }

    printf("imgtest: M422 DRI PoC + truncated headers + %d fuzz iters — ASan/UBSan clean\n", ITERS);
    return 0;
}
