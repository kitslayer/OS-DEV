/* Host fuzz + regression for kernel/color.c — parse_color, the CSS colour parser
 * the browser runs over untrusted page bytes (a style="" attribute or <style>
 * declaration: #rgb, #rrggbb, rgb()/rgba(), hsl()/hsla(), named). It reads a
 * length-bounded slice v[0..vl) and does clamped rgb/hsl integer math; a hostile
 * token must never read past the slice or overflow. We #include the real color.c
 * and drive it with exactly-sized buffers (no NUL) so any over-read red-zones.
 * Build under ASan+UBSan via tests/run-color-tests.sh. Exit 0 = pass.
 *
 * Oracle: change the #hex loop bound `i < vl` to `i <= vl` and this aborts (ASan)
 * — it then reads one byte past the slice. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/color.c"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

static void expect(const char *tok, uint32_t want) {
    uint32_t got = parse_color(tok, (int)strlen(tok));
    char m[96]; snprintf(m, sizeof(m), "parse_color('%s') = 0x%08X (want 0x%08X)", tok, got, want);
    CHECK(got == want, m);
}

/* exactly-sized (no NUL) so any over-read past v[0..vl) lands in a red-zone */
static void fuzz_one(const unsigned char *data, int len) {
    char *b = malloc(len ? len : 1);
    if (len) memcpy(b, data, len);
    uint32_t c = parse_color(b, len);
    /* result is either 0 (unknown) or has the 0x01 valid-flag set — never garbage high bits */
    CHECK(c == 0 || (c & 0xFF000000u) == 0x01000000u, "parse_color returned a malformed value");
    free(b);
}

int main(void) {
    /* ---- regression: the colour forms the browser accepts ---- */
    expect("#ff0000", 0x01FF0000);
    expect("#0f0", 0x0100FF00);                 /* 3-digit -> doubled nibbles */
    expect("#FFF", 0x01FFFFFF);
    expect("rgb(255,0,0)", 0x01FF0000);
    expect("rgb(0, 128, 255)", 0x010080FF);
    expect("rgba(255,255,255,0.5)", 0x01FFFFFF);  /* alpha ignored */
    expect("rgb(50%,0,0)", 0x017F0000);           /* 50% of 255 = 127 (integer division) */
    expect("red", 0x01CC0000);
    expect("STEELBLUE", 0x013672A0);               /* case-insensitive named */
    expect("white", 0x01FFFFFF);                    /* the most common keyword — was missing before M588 */
    expect("lightgray", 0x01D3D3D3);
    expect("aqua", 0x01008B8B);
    expect("tomato", 0x01E0503C);                   /* M603 additions */
    expect("dodgerblue", 0x012C7BE0);
    expect("LavendeR", 0x01E6E6FA);                 /* case-insensitive */
    expect("cornflowerblue", 0x015080D0);           /* 14 chars — within the buf[16] cap */
    expect("notacolor", 0);
    expect("", 0);
    expect("#", 0);                                /* bare # */
    expect("#xyz", 0);                             /* non-hex */
    { uint32_t k = parse_color("hsl(0,100%,50%)", 15); CHECK((k & 0xFF000000u)==0x01000000u, "hsl red parses"); }
    { uint32_t k = parse_color("hsl(120,100%,50%)", 17); CHECK((k & 0xFF000000u)==0x01000000u, "hsl green parses"); }
    /* M1776: a named colour followed by more tokens (a "!important", or a `background`
     * shorthand's image/keyword) must still resolve on its leading name token */
    expect("red !important", 0x01CC0000);       /* color: red !important */
    expect("white url(bg.png)", 0x01FFFFFF);    /* background: white url(...) */
    expect("navy no-repeat", 0x01000080);       /* background: navy no-repeat */
    expect("steelblue  ", 0x013672A0);          /* trailing whitespace */
    expect("red;", 0x01CC0000);                 /* trailing ';' */
    expect("reddish", 0);                        /* NOT "red": a longer letter run is not a prefix match */
    expect("re", 0);                             /* NOT "red": a shorter run isn't either */
    printf("regression: %s\n", fails ? "FAILURES" : "ok (#hex/rgb/rgba/hsl/named + leading-token names)");

    /* ---- fuzz: truncations + single-byte corruptions of a battery ---- */
    const char *bank[] = {
        "#abcdef", "#fff", "rgb(255,128,0)", "rgba(1,2,3,0.4)", "rgb(999%,300,-5)",
        "hsl(360,100%,50%)", "hsla(-10,200%,-5%,0.2)", "rgb(", "hsl(", "#", "steelblue",
        "rgb()", "hsl(,,)", "rgb(1", "#ABCDEFFFFF", 0 };
    for (int i = 0; bank[i]; i++) {
        int L = (int)strlen(bank[i]);
        for (int len = 0; len <= L; len++) fuzz_one((const unsigned char *)bank[i], len);
        for (int pos = 0; pos < L; pos++)
            for (int v = 1; v <= 256; v++) { char tmp[40]; memcpy(tmp, bank[i], L); tmp[pos] ^= (char)v; fuzz_one((const unsigned char *)tmp, L); }
    }

    /* ---- fuzz: random colour-char-biased buffers ---- */
    srand(0xC010A);
    const char *alpha = "#rgbahsl(),%0123456789abcdefABCDEF .-";
    int alen = (int)strlen(alpha);
    for (int trial = 0; trial < 400000; trial++) {
        int len = rand() % 40;
        unsigned char tmp[40];
        for (int i = 0; i < len; i++) tmp[i] = (trial & 1) ? (unsigned char)alpha[rand() % alen] : (unsigned char)rand();
        fuzz_one(tmp, len);
    }

    printf("fuzz: truncations/corruptions + 400000 random colour tokens -> %s\n", fails ? "FAILURES" : "all clean");
    if (fails) { printf("FAIL: %d check(s) failed\n", fails); return 1; }
    printf("PASS: color.c parse_color (#hex/rgb/hsl/named over malformed tokens, ASan/UBSan clean)\n");
    return 0;
}
