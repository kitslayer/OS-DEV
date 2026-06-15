/*
 * X.509 certificate-parser fuzz test (host-side, ASan/UBSan).
 *
 * x509_parse() parses UNTRUSTED DER certificates fetched from the network,
 * kernel-side with no stack guard page — so an OOB is kernel corruption. The
 * M419-423 security audit verified it bounds-safe (DER lengths accumulate into
 * a size_t, defeating the integer-overflow-of-the-bound attack; every nested
 * span is validated against its parent before use). This locks that finding and
 * fuzzes the parser against adversarial DER. Run via "make x509test". A clean
 * exit = pass; any OOB/overflow aborts under ASan/UBSan.
 */
#include <stdint.h>
#include <stdio.h>
#include "x509.h"

static uint32_t rs = 0x5EED1234u;
static uint32_t xr(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    x509_cert c;

    /* Crafted adversarial DER — the classic length-field attacks. */
    static const uint8_t c1[] = { 0x30,0x84,0xFF,0xFF,0xFF,0xFF };  /* SEQ claiming a 4 GB body, none present */
    static const uint8_t c2[] = { 0x30,0x82,0x01,0x00 };           /* SEQ claiming 256 bytes, truncated */
    static const uint8_t c3[] = { 0x30,0x03,0x02,0x01 };           /* SEQ{ INTEGER len 1 } truncated mid-value */
    static const uint8_t c4[] = { 0x30,0x80 };                     /* indefinite-length (BER, not DER) */
    x509_parse(c1, sizeof c1, &c);
    x509_parse(c2, sizeof c2, &c);
    x509_parse(c3, sizeof c3, &c);
    x509_parse(c4, sizeof c4, &c);

    /* A run of nested SEQUENCE headers with no bodies — tlv must reject each
     * over-long length, never over-read (x509_parse walks a FIXED structure, so
     * this can't deep-recurse). */
    uint8_t nest[128];
    for (int i = 0; i < (int)sizeof nest; i += 2) { nest[i] = 0x30; nest[i + 1] = 0x7E; }
    x509_parse(nest, sizeof nest, &c);

    /* Deterministic fuzz: random bytes, half of them prefixed with a DER
     * SEQUENCE + long-form length header so parsing proceeds into the body. */
    uint8_t f[300];
    const int ITERS = 200000;
    for (int i = 0; i < ITERS; i++) {
        int n = 2 + (int)(xr() % 250);
        int pre = 0;
        if (i & 1) { f[0] = 0x30; f[1] = 0x82; f[2] = (uint8_t)xr(); f[3] = (uint8_t)xr(); pre = 4; }
        for (int j = pre; j < n; j++) f[j] = (uint8_t)xr();
        x509_parse(f, n, &c);
    }

    printf("x509test: crafted DER + %d fuzz iters — ASan/UBSan clean\n", ITERS);
    return 0;
}
