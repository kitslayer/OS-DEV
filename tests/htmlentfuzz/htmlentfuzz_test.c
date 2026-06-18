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
/* same, for decode_utf8 — exercises the maxlen<1 guard and continuation reads */
static void fuzz_utf8(const unsigned char *data, int len) {
    if (len < 0) return;                   /* precondition; also stops GCC inferring malloc(rand()%7) could be negative (->huge) */
    char *b = malloc(len);                 /* exactly len bytes: len==0 makes an s[0] read an OOB */
    if (len) memcpy(b, data, len);
    unsigned cp = 0; int adv = decode_utf8(b, len, &cp);
    CHECK(adv >= 0 && adv <= 4, "utf8 adv out of range");
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
    expect("&le;", 4, '<'); expect("&ge;", 4, '>'); expect("&asymp;", 7, '~');   /* math entities (M591) */
    /* uni_to_ascii math/arrow folds (M591): common technical-page symbols -> nearest ASCII */
    CHECK(uni_to_ascii(0x2264)=='<' && uni_to_ascii(0x2265)=='>', "uni <= >= -> < >");
    CHECK(uni_to_ascii(0x2192)=='>' && uni_to_ascii(0x2190)=='<', "uni arrows -> > <");
    CHECK(uni_to_ascii(0x2248)=='~' && uni_to_ascii(0x2212)=='-', "uni approx/minus -> ~ -");
    CHECK(uni_to_ascii(0x00E9)=='e' && uni_to_ascii(0x2014)=='-', "uni existing folds intact (é, em-dash)");
    /* decode_utf8 regression */
    { unsigned cp; int adv;
      adv=decode_utf8("A",1,&cp);            CHECK(adv==1 && cp=='A', "utf8 ascii");
      adv=decode_utf8("\xC3\xA9",2,&cp);     CHECK(adv==2 && cp==0xE9, "utf8 2-byte (é)");
      adv=decode_utf8("\xE2\x80\x99",3,&cp); CHECK(adv==3 && cp==0x2019, "utf8 3-byte (rsquo)");
      adv=decode_utf8("\xC3",1,&cp);         CHECK(adv==1, "utf8 truncated -> lead byte");
      adv=decode_utf8("\xC3\x41",2,&cp);     CHECK(adv==1, "utf8 bad continuation -> lead byte");
      adv=decode_utf8("",0,&cp);             CHECK(adv==0, "utf8 maxlen<1 guard"); }
    printf("regression: %s\n", fails ? "FAILURES" : "ok (entity named/numeric/malformed + utf8)");

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

    /* ---- decode_utf8 fuzz: every length 0..6 of random high bytes ---- */
    for (int trial = 0; trial < 300000; trial++) {
        int len = rand() % 7;                         /* includes len==0 -> the maxlen<1 guard */
        unsigned char tmp[7];
        for (int i = 0; i < len; i++) tmp[i] = (trial & 1) ? (unsigned char)(0x80 + rand()%0x80) : (unsigned char)rand();
        fuzz_utf8(tmp, len);
    }

    printf("fuzz: entity truncations/corruptions + 300000 random + 300000 utf8 -> %s\n",
           fails ? "FAILURES" : "all clean");
    if (fails) { printf("FAIL: %d check(s) failed\n", fails); return 1; }
    printf("PASS: HTML entity decoder (named/numeric/malformed fuzz, ASan/UBSan clean)\n");
    return 0;
}
