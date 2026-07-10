/* Host fuzz + regression for kernel/cssprop.c — style_prop, the inline-style
 * declaration scanner the browser runs over untrusted page bytes (a style=""
 * attribute or <style> body). It walks s[0..n) looking for `prop:` at a property
 * boundary and returns the value span; every read is bounded by n, so a malformed
 * or oversized style must never OOB the kernel stack. We #include the real
 * cssprop.c and drive it with exactly-sized buffers (no NUL) so any over-read
 * red-zones. Build under ASan+UBSan via tests/run-css-tests.sh. Exit 0 = pass.
 *
 * Oracle: change the loop bound `i + plen + 1 <= n` to `i + plen <= n` and this
 * aborts (ASan) — s[i+plen] then reads at offset n, one past the slice. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/cssprop.c"
#include "../../kernel/color.c"                  /* parse_color — the border parser reads a colour token (M1777) */
#include "../../kernel/include/cssborder.h"      /* parse_style_border — conformance for border:none/0 */

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

static void expect(const char *style, const char *prop, const char *want) {
    int vs, ve, n = (int)strlen(style);
    int got = style_prop(style, n, prop, (int)strlen(prop), &vs, &ve);
    char m[160]; snprintf(m, sizeof(m), "style_prop('%s','%s') -> %.*s", style, prop, got?ve-vs:0, got?style+vs:"");
    if (!want) { CHECK(!got, m); return; }
    CHECK(got && ve-vs == (int)strlen(want) && memcmp(style+vs, want, ve-vs)==0, m);
}
/* parse_style_border returns (width<<28)|(sides<<24)|(color&0xFFFFFF); 0 = no border. (M1777) */
static void expect_border(const char *style, uint32_t want) {
    uint32_t got = parse_style_border(style, (int)strlen(style));
    char m[160]; snprintf(m, sizeof(m), "parse_style_border('%s') = 0x%08X (want 0x%08X)", style, got, want);
    CHECK(got == want, m);
}
static void fuzz_border(const unsigned char *data, int len) {
    char *b = malloc(len ? len : 1);
    if (len) memcpy(b, data, len);
    (void)parse_style_border(b, len);   /* must not OOB over the exactly-sized slice */
    free(b);
}

/* exactly-sized (no NUL) so any over-read past s[0..n) red-zones */
static void fuzz_one(const unsigned char *data, int len, const char *prop) {
    char *b = malloc(len ? len : 1);
    if (len) memcpy(b, data, len);
    int vs = -1, ve = -1;
    int r = style_prop(b, len, prop, (int)strlen(prop), &vs, &ve);
    if (r) CHECK(vs >= 0 && ve >= vs && ve <= len, "style_prop value span escapes [0,n]");
    free(b);
}

int main(void) {
    /* ---- regression ---- */
    expect("color:red", "color", "red");
    expect("color: blue ;", "color", "blue");                 /* ws-trimmed, stops at ';' */
    expect("background-color:#fff;color:red", "color", "red"); /* not matched inside background-color */
    expect("font-weight:bold;color:#0f0", "font-weight", "bold");
    expect("font:bold 14px Arial", "font", "bold 14px Arial");  /* `font:` shorthand value (M590) */
    expect("font-size:14px", "font", NULL);                     /* `font` must NOT match `font-size` (prefix, not the `:` boundary) */
    expect("font-family:serif", "font", NULL);
    expect("text-align:center", "text-align", "center");
    expect("text-align : center", "text-align", "center");     /* ws before AND after ':' (valid CSS) */
    expect("color red", "color", NULL);                        /* name + ws but no ':' -> no match */
    expect("x:1", "color", NULL);                              /* absent */
    expect("colorx:1", "color", NULL);                         /* not a boundary (needs ':') */
    expect("background:#fff url(x)", "background", "#fff url(x)"); /* shorthand value to end */
    expect("a:1;b:2", "b", "2");                               /* 2nd decl, after ';' boundary */
    expect("color:red;color:blue", "color", "blue");           /* CSS cascade: a later duplicate wins */
    expect("color:red !important", "color", "red");            /* !important priority marker stripped from the value */
    expect("font-weight:bold!important;", "font-weight", "bold");
    /* border shorthand parser (M1777): none/hidden/0 -> NO border (F2); real borders keep width|sides|colour */
    expect_border("border: none", 0);                     /* F2: border-style none -> nothing */
    expect_border("border:none", 0);                      /* no space after ':' */
    expect_border("border: hidden", 0);                   /* border-style hidden -> nothing */
    expect_border("border: 0", 0);                        /* zero width -> nothing */
    expect_border("border: 0px", 0);                      /* zero width with unit -> nothing (was clamped up to 1px) */
    expect_border("border: 1px solid black", 0x1F000000); /* w=1, all sides, black */
    expect_border("border: 2px solid red", 0x2FCC0000);   /* w=2, all sides, red (0xCC0000) */
    expect_border("border: 5px", 0x5F666666);             /* w=5, all sides, default grey */
    expect_border("border-top: 3px", 0x31666666);         /* top side only, default grey */
    expect_border("color:red", 0);                        /* no border property present */
    printf("regression: %s\n", fails ? "FAILURES" : "ok (style_prop boundary + value span + border none/0/real)");

    /* ---- fuzz: truncations + single-byte corruptions of a battery ---- */
    const char *bank[] = {
        "color:red;background-color:#abcdef;font-size:14px",
        "text-align:center; font-weight:700 ; text-decoration:underline",
        "color", "color:", ":", ";color:", "background:  ", "font-size:1.5em;",
        "color:rgb(1,2,3);", "x", 0 };
    const char *props[] = { "color", "background-color", "background", "font-size", "font-weight", "text-align", "weight", 0 };
    for (int i = 0; bank[i]; i++) {
        int L = (int)strlen(bank[i]);
        for (int len = 0; len <= L; len++) for (int pp = 0; props[pp]; pp++) fuzz_one((const unsigned char *)bank[i], len, props[pp]);
        for (int pos = 0; pos < L; pos++)
            for (int v = 1; v <= 256; v += 1) { char tmp[80]; memcpy(tmp, bank[i], L); tmp[pos] ^= (char)v; fuzz_one((const unsigned char *)tmp, L, "color"); }
    }

    /* ---- fuzz: random style-char-biased buffers ---- */
    srand(0xC55A);
    const char *alpha = "color:;background-fontweight align14px#rgb()%. \t\"'{}";
    int alen = (int)strlen(alpha);
    const char *fp[] = { "color", "background-color", "font-size", "text-align" };
    for (int trial = 0; trial < 400000; trial++) {
        int len = rand() % 56;
        unsigned char tmp[56];
        for (int i = 0; i < len; i++) tmp[i] = (trial & 1) ? (unsigned char)alpha[rand() % alen] : (unsigned char)rand();
        fuzz_one(tmp, len, fp[rand() % 4]);
        fuzz_border(tmp, len);   /* border parser over the same random slice (bounds, ASan) */
    }

    printf("fuzz: truncations/corruptions + 400000 random style buffers -> %s\n", fails ? "FAILURES" : "all clean");
    if (fails) { printf("FAIL: %d check(s) failed\n", fails); return 1; }
    printf("PASS: cssprop.c style_prop (boundary scan + value span over malformed styles, ASan/UBSan clean)\n");
    return 0;
}
