/* Host fuzz + regression for kernel/htmlattr.c — the HTML attribute scanners the
 * browser runs over untrusted page bytes (find_attr / has_attr / attr_int /
 * find_href). They take a length-bounded slice a[0..n); a malformed or truncated
 * tag must never read past it (kernel-side that's a guard-page-less stack OOB).
 *
 * We #include the real htmlattr.c and drive it with exactly-sized buffers (no
 * trailing NUL) so any over-read lands in an ASan red-zone. Build under
 * ASan+UBSan via tests/run-htmlattr-tests.sh. Exit 0 = pass.
 *
 * Oracle check: change find_attr's `i + nl <= n` loop bound to `i < n` and this
 * aborts (ASan heap-buffer-overflow) — the name compare then reads past the slice. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/htmlattr.c"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

/* Run all four scanners over an exactly-sized copy of data[0..len). Result is
 * ignored beyond the find_attr value staying inside the slice — this is a
 * memory-safety probe. */
static void fuzz_one(const unsigned char *data, int len) {
    char *b = malloc(len ? (size_t)len : 1);
    if (len) memcpy(b, data, (size_t)len);
    const char *v; int vl;
    if (find_attr(b, len, "href", &v, &vl)) {
        CHECK(v >= b && v + vl <= b + len, "find_attr value escapes the slice");
    }
    find_attr(b, len, "src", &v, &vl);
    find_href(b, len, &v, &vl);
    has_attr(b, len, "checked");
    has_attr(b, len, "disabled");
    int w = attr_int(b, len, "width");
    CHECK(w >= 0 && w <= 8192, "attr_int out of clamp range");
    free(b);
}

static void expect_attr(const char *tag, const char *name, const char *want) {
    const char *v; int vl;
    int got = find_attr(tag, (int)strlen(tag), name, &v, &vl);
    char m[128]; snprintf(m, sizeof(m), "find_attr '%s' in <%s>", name, tag);
    if (!want) { CHECK(!got, m); return; }
    CHECK(got && vl == (int)strlen(want) && memcmp(v, want, (size_t)vl) == 0, m);
}

int main(void) {
    /* ---- regression: real attribute spans the browser actually parses ---- */
    expect_attr("a href=\"http://x.com\"", "href", "http://x.com");
    expect_attr("a href='single'",         "href", "single");
    expect_attr("img src=pic.png width=48", "src",  "pic.png");      /* bare (unquoted) value */
    expect_attr("img src=pic.png alt=\"hi\"", "alt", "hi");
    expect_attr("input name=\"q\"",         "name", "q");
    expect_attr("a hrefx=\"no\"",           "href", NULL);           /* not a token boundary */
    expect_attr("a data-href=\"no\"",       "href", NULL);           /* prefix, not boundary */
    expect_attr("td",                       "href", NULL);           /* absent */
    { const char *v; int vl;
      CHECK(find_href("a href=\"/p\"", 13, &v, &vl) && vl == 2 && memcmp(v, "/p", 2) == 0, "find_href"); }
    CHECK(has_attr("input checked", 13, "checked"), "has_attr bare token present");
    CHECK(!has_attr("input checkedx", 14, "checked"), "has_attr rejects non-boundary suffix");
    CHECK(has_attr("input type=cb checked disabled", 30, "disabled"), "has_attr mid-list");
    CHECK(attr_int("img width=48 height=20", 22, "width") == 48, "attr_int width=48");
    CHECK(attr_int("img width=999999", 16, "width") == 8192, "attr_int clamps huge");
    CHECK(attr_int("img width=ab", 12, "width") == 0, "attr_int non-numeric -> 0");
    printf("regression: %s\n", fails ? "FAILURES" : "ok (find_attr/has_attr/attr_int/find_href)");

    /* ---- fuzz: truncations + single-byte corruptions of a battery ---- */
    const char *bank[] = {
        "a href=\"http://example.com/path?q=1\"",
        "img src='pic.png' width=4096 height=2048 alt=\"x\"",
        "input type=checkbox checked disabled name=\"f\" value='v'",
        "a href=", "a href=\"", "a href='unterminated", "x=", "=", "  href  =  \"y\"  ",
        "div class=\"a b c\" id=main data-jsh=7", 0 };
    for (int i = 0; bank[i]; i++) {
        int L = (int)strlen(bank[i]);
        for (int len = 0; len <= L; len++) fuzz_one((const unsigned char *)bank[i], len);
        for (int pos = 0; pos < L; pos++)
            for (int v = 1; v <= 256; v++) {
                char tmp[80]; memcpy(tmp, bank[i], (size_t)L); tmp[pos] ^= (char)v;
                fuzz_one((const unsigned char *)tmp, L);
            }
    }

    /* ---- fuzz: random attribute-char-biased buffers ---- */
    srand(0xA77B);
    const char *alpha = "abcdefhinrsw= '\"<>/\t 0123456789-";
    int alen = (int)strlen(alpha);
    for (int trial = 0; trial < 400000; trial++) {
        int len = rand() % 48;
        unsigned char tmp[48];
        for (int i = 0; i < len; i++)
            tmp[i] = (trial & 1) ? (unsigned char)alpha[rand() % alen] : (unsigned char)rand();
        fuzz_one(tmp, len);
    }

    printf("fuzz: truncations/corruptions + 400000 random attribute slices -> %s\n",
           fails ? "FAILURES" : "all clean");
    if (fails) { printf("FAIL: %d check(s) failed\n", fails); return 1; }
    printf("PASS: HTML attribute scanners (find/has/int over malformed slices, ASan/UBSan clean)\n");
    return 0;
}
