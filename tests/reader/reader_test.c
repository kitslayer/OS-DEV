/* Host regression + fuzz for kernel/reader.c — reader_main_region, the reader-mode
 * content-extractor the browser runs over untrusted page HTML to find the article
 * region (so a linear renderer shows the article, not the nav chrome). It walks
 * arbitrary bytes scanning tags + attributes and must never read past the slice.
 *
 * We drive it with EXACTLY-sized, NON-NUL-terminated copies (malloc(len), no '\0')
 * so any read past body[len) lands in an ASan red zone, and we assert every
 * returned region stays within [0,len). Build under ASan+UBSan via
 * tests/run-reader-tests.sh (which also compiles reader.c + htmlattr.c).
 *
 * Oracle (verified): drop the `i + nl > len` guard in name_is() and ASan trips on
 * the exactly-sized inputs; remove the script/style skip in find_close() and the
 * "fake </main> inside <script>" case below mis-detects (region ends early).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "reader.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

/* Run on an exactly-sized, un-terminated copy so ASan catches any over-read. */
static int run(const char *html, int *lo, int *hi) {
    int n = (int)strlen(html);
    char *buf = malloc(n ? n : 1);
    memcpy(buf, html, n);
    *lo = *hi = -1;
    int r = reader_main_region(buf, n, lo, hi);
    if (r) {
        CHECK(*lo >= 0 && *hi <= n && *lo <= *hi, "returned region within [0,len)");
    }
    free(buf);
    return r;
}

static int region_has(const char *html, int lo, int hi, const char *needle) {
    int nl = (int)strlen(needle);
    for (int i = lo; i + nl <= hi; i++)
        if (memcmp(html + i, needle, nl) == 0) return 1;
    return 0;
}

/* shouldfind: expected return; if found, region must contain `want` and must NOT
 * contain `unwant` (pass NULL to skip a check). */
static void expect(const char *name, const char *html, int shouldfind,
                   const char *want, const char *unwant) {
    int lo, hi, r = run(html, &lo, &hi);
    char m[160]; snprintf(m, sizeof m, "%s: expected find=%d got=%d", name, shouldfind, r);
    CHECK(r == shouldfind, m);
    if (r && shouldfind) {
        if (want) { snprintf(m, sizeof m, "%s: region should contain \"%s\"", name, want);
                    CHECK(region_has(html, lo, hi, want), m); }
        if (unwant) { snprintf(m, sizeof m, "%s: region should NOT contain \"%s\"", name, unwant);
                      CHECK(!region_has(html, lo, hi, unwant), m); }
    }
}

/* >140 visible chars, with a unique marker token to assert on. */
#define BODY(marker) marker " lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut labore et dolore magna aliqua ut enim ad minim veniam quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat duis aute irure dolor in reprehenderit."

int main(void) {
    /* 1. <main> beats surrounding nav/footer chrome */
    expect("main", "<html><body><nav>MENU links here</nav><main><h1>Hi</h1><p>" BODY("ARTBODY") "</p></main><footer>FOOT</footer></body></html>",
           1, "ARTBODY", "MENU");

    /* 2. precise content id (#mw-content-text) wins over the enclosing <main> */
    expect("mw-content-text", "<main id=\"content\"><nav>LANGS aa bb cc dd ee ff gg hh ii jj kk ll mm nn oo pp qq rr ss tt uu vv ww xx yy zz</nav>"
           "<div id=\"mw-content-text\"><p>" BODY("WIKIBODY") "</p></div></main>",
           1, "WIKIBODY", "LANGS");

    /* 3. a single <article> beats the page header */
    expect("article", "<header>SITEHEADER nav stuff</header><article><p>" BODY("STORY") "</p></article>",
           1, "STORY", "SITEHEADER");

    /* 4. a multi-<article> listing is NOT extracted (no main/id either) -> whole page */
    expect("listing", "<div><article><p>" BODY("POSTA") "</p></article><article><p>" BODY("POSTB") "</p></article></div>",
           0, NULL, NULL);

    /* 5. ARIA role=main on a div */
    expect("role-main", "<header>CHROME bits</header><div role=\"main\"><p>" BODY("ROLEBODY") "</p></div>",
           1, "ROLEBODY", "CHROME");

    /* 6. no container at all -> whole page */
    expect("no-container", "<body><p>" BODY("PLAIN") "</p></body>", 0, NULL, NULL);

    /* 7. unclosed <main> -> can't bound it -> whole page */
    expect("unclosed", "<main><p>" BODY("NOCLOSE") "</p>", 0, NULL, NULL);

    /* 8. tiny <main> below the text threshold -> rejected, fall through */
    expect("tiny-main", "<main>hi</main><article><p>" BODY("REALART") "</p></article>",
           1, "REALART", NULL);

    /* 9. a fake </main> inside <script> must not end the region early */
    expect("script-fakeclose", "<main><script>var s = \"</main>\";</script><p>" BODY("AFTERSCRIPT") "</p></main>",
           1, "AFTERSCRIPT", NULL);

    /* 10. a fake </main> inside a comment must be ignored */
    expect("comment-fakeclose", "<main><!-- </main> ignore --><p>" BODY("AFTERCOMMENT") "</p></main>",
           1, "AFTERCOMMENT", NULL);

    /* 11. nested <main> (invalid, but must balance) — outer region spans both */
    expect("nested-main", "<main><p>outer</p><main><p>" BODY("INNER") "</p></main><p>tail</p></main>",
           1, "INNER", NULL);

    /* --- fuzz: random + structured-random bytes, exactly-sized buffers. Just
       assert no crash / no OOB (ASan) and that any returned region is in-bounds. --- */
    const char *toks[] = { "<main>", "</main>", "<article>", "</article>", "<div id=content>",
                           "<div id=\"mw-content-text\">", "</div>", "<script>", "</script>",
                           "<!--", "-->", "<nav>", "</nav>", "x ", "<p>", "</p>", "\"", "'", "<", ">", "role=main " };
    int ntok = (int)(sizeof toks / sizeof toks[0]);
    srand(1234);
    for (int it = 0; it < 200000; it++) {
        int parts = 1 + rand() % 40, cap = 4096, n = 0;
        char *buf = malloc(cap);
        if (rand() % 3 == 0) {                      /* pure random bytes */
            n = rand() % cap;
            for (int i = 0; i < n; i++) buf[i] = (char)(rand() & 0xFF);
        } else {                                    /* structured: concat random tokens + noise */
            for (int p = 0; p < parts && n < cap - 32; p++) {
                if (rand() % 4 == 0) { buf[n++] = (char)(rand() & 0xFF); continue; }
                const char *t = toks[rand() % ntok];
                int tl = (int)strlen(t);
                for (int k = 0; k < tl && n < cap - 1; k++) buf[n++] = t[k];
            }
        }
        char *exact = malloc(n ? n : 1);            /* exact size -> ASan red-zones the tail */
        memcpy(exact, buf, n);
        int lo = -1, hi = -1, r = reader_main_region(exact, n, &lo, &hi);
        if (r) CHECK(lo >= 0 && hi <= n && lo <= hi, "fuzz: region within bounds");
        free(exact); free(buf);
    }

    if (fails) { printf("reader_test: %d FAILURES\n", fails); return 1; }
    printf("reader_test: all checks passed\n");
    return 0;
}
