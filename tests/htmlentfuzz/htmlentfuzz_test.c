/*
 * htmlentfuzz_test.c — host-side fuzz of the HTML entity decoder (ASan + UBSan).
 *
 * decode_entity reads raw bytes from untrusted page HTML in-kernel, so a
 * malformed/truncated entity (&, &#, &#x, &nameWithNoSemicolon, huge numeric
 * ref) must never read out of bounds. This #includes htmlentity.c (extracted
 * from browser.c) and drives decode_entity over exactly-sized buffers (maxlen =
 * the allocation, so any read at/after the end trips an ASan red-zone): known
 * entities for regression, then every truncated prefix + single-byte corruption
 * of a battery of valid entities, plus random entity-char-biased buffers.
 * Exit 0 = pass.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "htmlentity.c"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", (msg)); fails++; } } while (0)

/* decode `lit` (its strlen) and assert the consumed count + decoded char */
static void expect(const char *lit, int adv_exp, char out_exp) {
    char out = 0; int adv = decode_entity(lit, (int)strlen(lit), &out);
    char m[64]; snprintf(m, sizeof(m), "decode '%s' adv=%d/'%c'", lit, adv, out>=32?out:'?');
    CHECK(adv == adv_exp && (adv_exp == 0 || out == out_exp), m);
}

/* fuzz one buffer: exactly-sized (maxlen == len, no NUL) so any over-read red-zones */
static void fuzz_one(const unsigned char *data, int len) {
    char *b = malloc(len ? len : 1);
    memcpy(b, data, len);
    char out = 0;
    decode_entity(b, len, &out);   /* result ignored; memory safety only */
    free(b);
}

int main(void) {
    /* ---- regression ---- */
    expect("&amp;", 5, '&');
    expect("&lt;", 4, '<');
    expect("&gt;", 4, '>');
    expect("&quot;", 6, '"');
    expect("&nbsp;", 6, ' ');
    expect("&#65;", 5, 'A');
    expect("&#x41;", 6, 'A');
    expect("&#X41;", 6, 'A');
    expect("&#233;", 6, 'e');          /* é -> folded to 'e' */
    expect("&#0;", 4, ' ');            /* control -> space */
    expect("&unknownxyz;", 0, 0);      /* unknown named entity */
    expect("&amp", 0, 0);              /* no terminating ';' */
    expect("&#zz;", 0, 0);             /* non-numeric after &# */
    expect("&", 0, 0);                 /* bare '&' */
    expect("&#99999999;", 11, ' ');    /* huge numeric ref: clamped, decodes to a char */
    printf("regression: %s\n", fails ? "FAILURES" : "ok (named + numeric + malformed)");

    /* ---- fuzz: truncations + single-byte corruptions of a battery ---- */
    const char *bank[] = {
        "&amp;","&#x1F600;","&#233;","&#x10FFFF;","&hellip;","&#0;","&nbsp;",
        "&#999999999999;","&#xFFFFFFFF;","&rsquo;","&#;","&#x;","&;","&mdash;", 0 };
    for (int i = 0; bank[i]; i++) {
        int L = (int)strlen(bank[i]);
        for (int len = 0; len <= L; len++) fuzz_one((const unsigned char *)bank[i], len);
        for (int pos = 0; pos < L; pos++)
            for (int v = 1; v <= 256; v++) {
                char tmp[32]; memcpy(tmp, bank[i], L); tmp[pos] ^= (char)v;
                fuzz_one((const unsigned char *)tmp, L);
            }
    }

    /* ---- fuzz: random entity-char-biased buffers ---- */
    srand(0xE7717);
    const char *alpha = "&#;xX0123456789abcdefABCDEFnmpquotsl";
    int alen = (int)strlen(alpha);
    for (int trial = 0; trial < 300000; trial++) {
        int len = rand() % 40;
        unsigned char tmp[40];
        tmp[0] = '&';                                  /* callers only invoke on s[0]=='&' */
        for (int i = 1; i < len; i++) tmp[i] = (trial & 1) ? (unsigned char)alpha[rand() % alen] : (unsigned char)rand();
        fuzz_one(tmp, len);
    }

    printf("fuzz: truncations + single-byte corruptions + 300000 random -> %s\n",
           fails ? "FAILURES" : "all clean");
    if (fails) { printf("FAIL: %d check(s) failed\n", fails); return 1; }
    printf("PASS: HTML entity decoder (named/numeric/malformed fuzz, ASan/UBSan clean)\n");
    return 0;
}
