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
#include "san_certs.h"   /* generated: cert_match / cert_wild / cert_cnonly / cert_sanmiss */

static uint32_t rs = 0x5EED1234u;
static uint32_t xr(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void) {
    x509_cert c;

    /* ---- SAN parsing + hostname matching (real openssl-generated certs) ---- */
    CHECK(x509_parse(cert_match, cert_match_len, &c) == 0, "cert_match parses");
    CHECK(c.n_san == 2, "cert_match has 2 SAN dNSNames");
    CHECK(host_matches_cert("example.com", &c) == 1, "match: example.com");
    CHECK(host_matches_cert("www.example.com", &c) == 1, "match: www.example.com");
    CHECK(host_matches_cert("EXAMPLE.COM", &c) == 1, "match: case-insensitive");
    CHECK(host_matches_cert("evil.com", &c) == 0, "no match: evil.com");
    CHECK(host_matches_cert("xexample.com", &c) == 0, "no match: xexample.com");
    CHECK(host_matches_cert("", &c) == 0, "no match: empty host");
    CHECK(x509_parse(cert_wild, cert_wild_len, &c) == 0, "cert_wild parses");
    CHECK(host_matches_cert("a.example.org", &c) == 1, "wildcard: a.example.org");
    CHECK(host_matches_cert("WWW.example.ORG", &c) == 1, "wildcard: case-insensitive");
    CHECK(host_matches_cert("example.org", &c) == 0, "wildcard: NOT bare apex");
    CHECK(host_matches_cert("a.b.example.org", &c) == 0, "wildcard: NOT two labels");
    CHECK(host_matches_cert("a.example.com", &c) == 0, "wildcard: wrong domain");
    CHECK(x509_parse(cert_cnonly, cert_cnonly_len, &c) == 0, "cert_cnonly parses");
    CHECK(c.n_san == 0, "cert_cnonly has no SAN");
    CHECK(host_matches_cert("legacy.test", &c) == 1, "CN fallback: legacy.test");
    CHECK(host_matches_cert("other.test", &c) == 0, "CN fallback: no match");
    CHECK(x509_parse(cert_sanmiss, cert_sanmiss_len, &c) == 0, "cert_sanmiss parses");
    CHECK(c.n_san == 1, "cert_sanmiss has 1 SAN");
    CHECK(host_matches_cert("other.test", &c) == 1, "SAN present: other.test matches");
    CHECK(host_matches_cert("match.test", &c) == 0, "SAN present: CN match.test NOT consulted");

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
        if (x509_parse(f, n, &c) == 0) {
            /* fuzz the matcher against arbitrary parser output — it must never OOB
             * regardless of n_san / san[] contents, and n_san must stay in range. */
            if (c.n_san < 0 || c.n_san > X509_MAX_SAN) { printf("FAIL: n_san out of range %d\n", c.n_san); fails++; break; }
            host_matches_cert("fuzz.example.com", &c);
            host_matches_cert("*.weird", &c);
        }
    }

    /* MUTATION fuzz of the real SAN-bearing certs: corrupt copies (byte flips +
     * truncation) drive find_san() over near-valid SAN/extension DER — the realistic
     * adversarial case. n_san must always stay in [0, X509_MAX_SAN] and the matcher
     * must never OOB. (The 200k random fuzz above rarely forms a valid [3] block.) */
    {
        static const struct { const unsigned char *d; int n; } seeds[] = {
            { cert_match, sizeof cert_match }, { cert_wild, sizeof cert_wild },
            { cert_cnonly, sizeof cert_cnonly }, { cert_sanmiss, sizeof cert_sanmiss },
        };
        static uint8_t mf[2048];
        for (int i = 0; i < 400000; i++) {
            int s = (int)(xr() % 4), sl = seeds[s].n;
            int len = sl; if (xr() & 1) len = 1 + (int)(xr() % sl);   /* sometimes truncate */
            if (len > (int)sizeof mf) len = (int)sizeof mf;
            for (int k = 0; k < len; k++) mf[k] = seeds[s].d[k];
            int muts = 1 + (int)(xr() % 6);
            for (int m = 0; m < muts; m++) mf[xr() % len] = (uint8_t)xr();   /* flip bytes */
            if (x509_parse(mf, len, &c) == 0) {
                if (c.n_san < 0 || c.n_san > X509_MAX_SAN) { printf("FAIL: mutated n_san=%d\n", c.n_san); fails++; break; }
                host_matches_cert("a.example.com", &c);    /* ASan/UBSan catch any OOB in find_san/matcher */
                host_matches_cert("example.com", &c);
            }
        }
    }

    if (fails == 0)
        printf("x509test: SAN/hostname unit tests + crafted DER + %d random + 400000 SAN-mutation fuzz iters — ASan/UBSan clean, PASS\n", ITERS);
    else
        printf("x509test: %d FAILURE(S)\n", fails);
    return fails ? 1 : 0;
}
