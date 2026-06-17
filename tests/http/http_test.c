/*
 * http_test.c — host-side regression + fuzz test of the HTTP response parsers
 * (ASan + UBSan). #includes http.c (extracted from browser.c) and exercises the
 * three functions that read untrusted server/CDN bytes:
 *   http_is_chunked  — header scan for Transfer-Encoding: chunked
 *   http_dechunk     — in-place chunked-body decode (the risky one: it memmoves
 *                      with attacker-controlled hex sizes)
 *   http_find_loc    — Location: header value extraction (redirects)
 *
 * Regression: known inputs produce the expected outputs. Fuzz: every truncated
 * prefix of a valid chunked body, every single-byte corruption, and many random
 * buffers must never OOB (ASan), must keep the decoded length within the input
 * (de-chunking only compacts), and must never write past `max` for Location.
 * Exit 0 = pass.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define RAW_MAX 262144
#include "http.c"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", (msg)); fails++; } } while (0)

static void test_is_chunked(void) {
    const char *h1 = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    CHECK(http_is_chunked(h1, (int)strlen(h1)) == 1, "missed chunked header");
    const char *h2 = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
    CHECK(http_is_chunked(h2, (int)strlen(h2)) == 0, "false chunked positive");
    const char *h3 = "HTTP/1.1 200 OK\r\nTRANSFER-ENCODING:  CHUNKED\r\n\r\n";
    CHECK(http_is_chunked(h3, (int)strlen(h3)) == 1, "case-insensitivity broken");
    /* "chunked" must be a header VALUE, not appear in a different header's value */
    const char *h4 = "HTTP/1.1 200 OK\r\nX-Note: not-transfer-encoding chunked\r\n\r\n";
    CHECK(http_is_chunked(h4, (int)strlen(h4)) == 0, "matched chunked in wrong header");
}

static void test_dechunk(void) {
    char b1[] = "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    int n = http_dechunk(b1, (int)sizeof(b1) - 1, RAW_MAX);
    CHECK(n == 9 && memcmp(b1, "Wikipedia", 9) == 0, "basic dechunk wrong");

    char b2[] = "a\r\n0123456789\r\n0\r\n\r\n";         /* hex size 'a' = 10 */
    n = http_dechunk(b2, (int)sizeof(b2) - 1, RAW_MAX);
    CHECK(n == 10 && memcmp(b2, "0123456789", 10) == 0, "hex-size dechunk wrong");

    char b3[] = "3;name=value\r\nabc\r\n0\r\n\r\n";      /* chunk extension after ';' */
    n = http_dechunk(b3, (int)sizeof(b3) - 1, RAW_MAX);
    CHECK(n == 3 && memcmp(b3, "abc", 3) == 0, "chunk-extension not skipped");

    /* truncated: claims 9 bytes but only 3 present -> take the 3 we have */
    char b4[] = "9\r\nabc";
    n = http_dechunk(b4, (int)sizeof(b4) - 1, RAW_MAX);
    CHECK(n == 3 && memcmp(b4, "abc", 3) == 0, "truncated chunk mishandled");
}

static void test_find_loc(void) {
    char out[256];
    const char *r1 = "HTTP/1.1 302 Found\r\nLocation: https://example.com/x\r\n\r\n";
    CHECK(http_find_loc(r1, (int)strlen(r1), out, sizeof(out)) == 1 &&
          strcmp(out, "https://example.com/x") == 0, "Location not found/parsed");
    const char *r2 = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n";
    CHECK(http_find_loc(r2, (int)strlen(r2), out, sizeof(out)) == 0, "false Location positive");
    /* respects max: a long value must be truncated with a NUL, never overrun */
    const char *r3 = "HTTP/1.1 301\r\nLocation: aaaaaaaaaaaaaaaaaaaaaaaa\r\n\r\n";
    char small[8];
    CHECK(http_find_loc(r3, (int)strlen(r3), small, (int)sizeof(small)) == 1 &&
          strlen(small) < sizeof(small), "Location overran small buffer");
}

static void fuzz_dechunk(void) {
    const char *valid = "4\r\nWiki\r\n5\r\npedia\r\n3;x=1\r\nabc\r\n0\r\n\r\n";
    int vlen = (int)strlen(valid);

    /* every truncated prefix, in an exactly-sized buffer so ASan red-zones any
     * read/write past the end */
    for (int len = 0; len <= vlen; len++) {
        char *t = malloc(len ? len : 1);
        memcpy(t, valid, len);
        int n = http_dechunk(t, len, RAW_MAX);
        CHECK(n >= 0 && n <= len, "dechunk returned length outside input");
        free(t);
    }

    /* every single-byte corruption */
    for (int pos = 0; pos < vlen; pos++) {
        for (int v = 1; v <= 256; v++) {
            char *t = malloc(vlen);
            memcpy(t, valid, vlen);
            t[pos] ^= (char)v;
            int n = http_dechunk(t, vlen, RAW_MAX);
            CHECK(n >= 0 && n <= vlen, "corrupt dechunk length outside input");
            free(t);
        }
    }

    /* random buffers, including ones biased toward hex digits + CRLF so the
     * size-line/data paths are reached */
    srand(99);
    const char *alpha = "0123456789abcdefABCDEF\r\n;xyz";
    int alen = (int)strlen(alpha);
    for (int trial = 0; trial < 300000; trial++) {
        int len = rand() % 128;
        char *t = malloc(len ? len : 1);
        for (int i = 0; i < len; i++)
            t[i] = (trial & 1) ? alpha[rand() % alen] : (char)rand();
        int n = http_dechunk(t, len, RAW_MAX);
        CHECK(n >= 0 && n <= len, "fuzz dechunk length outside input");
        free(t);
    }

    /* a tiny cap must still be honored without overflow */
    char b[] = "ffffffff\r\nABCD\r\n0\r\n\r\n";
    int n = http_dechunk(b, (int)sizeof(b) - 1, 4);
    CHECK(n >= 0 && n <= (int)sizeof(b) - 1, "small-cap dechunk overran");
}

static void fuzz_headers(void) {
    /* feed the header scanners truncated prefixes + random bytes of a response */
    const char *resp = "HTTP/1.1 302\r\nLocation: http://h/\r\nTransfer-Encoding: chunked\r\n\r\nbody";
    int rlen = (int)strlen(resp);
    char out[64];
    for (int len = 0; len <= rlen; len++) {
        char *t = malloc(len ? len : 1);
        memcpy(t, resp, len);
        (void)http_is_chunked(t, len);
        if (http_find_loc(t, len, out, (int)sizeof(out)))
            CHECK(strlen(out) < sizeof(out), "find_loc overran on prefix");
        free(t);
    }
    srand(7);
    for (int trial = 0; trial < 100000; trial++) {
        int len = rand() % 96;
        char *t = malloc(len ? len : 1);
        for (int i = 0; i < len; i++) t[i] = (char)rand();
        (void)http_is_chunked(t, len);
        if (http_find_loc(t, len, out, (int)sizeof(out)))
            CHECK(strlen(out) < sizeof(out), "find_loc overran on random");
        free(t);
    }
}

int main(void) {
    test_is_chunked();
    test_dechunk();
    test_find_loc();
    printf("regression: %s\n", fails ? "FAILURES" : "ok (is_chunked + dechunk + find_loc)");
    fuzz_dechunk();
    fuzz_headers();
    printf("fuzz: truncations + single-byte corruptions + 400000 random buffers -> %s\n",
           fails ? "FAILURES" : "all clean");
    if (fails) { printf("FAIL: %d check(s) failed\n", fails); return 1; }
    printf("PASS: HTTP response parsers (regression + fuzz/corrupt safe, ASan/UBSan clean)\n");
    return 0;
}
