/* Host regression + fuzz for img_src_attr — the <img> URL resolver that
 * tries srcset= / data-src= / data-original= / data-lazy-src= when the
 * canonical src= is absent or empty.  It runs kernel-side over untrusted
 * page bytes; any over-read past the attribute slice is silent stack
 * corruption.  We compile under ASan+UBSan and drive it with exactly-sized
 * buffers (no trailing NUL) so any over-read hits an ASan red-zone.
 *
 * Build + run via tests/run-imgsrc-tests.sh. Exit 0 = pass. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pull in the real find_attr / has_attr / attr_int / find_href from
 * kernel/htmlattr.c (same pattern as htmlattr_test.c). */
#include "../../kernel/htmlattr.c"

/* ---------- copy of img_src_attr from kernel/browser.c ----------
 * Keep in sync with the browser.  This copy lets us fuzz it in isolation
 * without dragging in the whole kernel. */
static int img_src_attr(const char *attrs, int attrlen,
                        const char **v, int *vl) {
    /* 1. Canonical src= — unchanged behaviour. */
    if (find_attr(attrs, attrlen, "src", v, vl) && *vl > 0) return 1;
    /* 2. srcset="url descriptor, url descriptor, ..." — take first candidate URL
     *    (everything up to the first space or comma). */
    { const char *sv; int sl;
      if (find_attr(attrs, attrlen, "srcset", &sv, &sl) && sl > 0) {
          /* skip leading whitespace */
          int s = 0; while (s < sl && (sv[s]==' '||sv[s]=='\t'||sv[s]=='\n'||sv[s]=='\r')) s++;
          int e = s;
          while (e < sl && sv[e]!=' ' && sv[e]!='\t' && sv[e]!=',' &&
                 sv[e]!='\n' && sv[e]!='\r') e++;
          if (e > s) { *v = sv + s; *vl = e - s; return 1; }
      } }
    /* 3. data-src= / data-original= / data-lazy-src= (lazy-load patterns). */
    { static const char * const lazy[] = { "data-src", "data-original", "data-lazy-src", 0 };
      for (int i = 0; lazy[i]; i++) {
          if (find_attr(attrs, attrlen, lazy[i], v, vl) && *vl > 0) return 1;
      } }
    return 0;
}
/* ---------------------------------------------------------------- */

static int fails;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", (msg)); fails++; } \
} while (0)

/* Drive img_src_attr over an exactly-sized copy of data[0..len) to catch
 * any over-read via ASan red-zones.  If a URL is returned it must lie
 * entirely within the supplied slice. */
static void fuzz_one(const unsigned char *data, int len) {
    char *b = malloc(len ? (size_t)len : 1);
    if (len) memcpy(b, data, (size_t)len);
    const char *v; int vl;
    if (img_src_attr(b, len, &v, &vl)) {
        CHECK(v >= b && v + vl <= b + len, "img_src_attr value escapes the slice");
        CHECK(vl > 0, "img_src_attr returned length 0 with success");
    }
    free(b);
}

/* Regression helper: drive img_src_attr and compare with want (NULL = expect 0). */
static void expect(const char *attrs, const char *want) {
    const char *v; int vl;
    int got = img_src_attr(attrs, (int)strlen(attrs), &v, &vl);
    char msg[200];
    snprintf(msg, sizeof(msg), "img_src_attr(<%s>) -> want '%s'", attrs, want ? want : "(none)");
    if (!want) { CHECK(!got, msg); return; }
    CHECK(got && vl == (int)strlen(want) && memcmp(v, want, (size_t)vl) == 0, msg);
}

int main(void) {
    /* ---- regression: precedence rules ---- */

    /* 1. src present → returns src, ignores srcset */
    expect("img src=\"pic.png\" srcset=\"other.png 320w\"", "pic.png");
    /* 2. src present bare (unquoted) → returns src */
    expect("img src=bare.jpg", "bare.jpg");
    /* 3. src absent, srcset present → first candidate URL (no descriptor) */
    expect("img srcset=\"hero.png 640w, hero2x.png 2x\"", "hero.png");
    /* 4. src absent, srcset with no space before comma */
    expect("img srcset=\"a.png,b.png 2x\"", "a.png");
    /* 5. src absent, srcset only (single URL, no descriptor) */
    expect("img srcset=\"solo.webp\"", "solo.webp");
    /* 6. src absent, srcset absent, data-src present */
    expect("img data-src=\"lazy.jpg\"", "lazy.jpg");
    /* 7. src absent, data-original */
    expect("img data-original=\"orig.jpg\"", "orig.jpg");
    /* 8. data-lazy-src */
    expect("img data-lazy-src=\"lazier.jpg\"", "lazier.jpg");
    /* 9. nothing useful → 0 */
    expect("img alt=\"no src here\"", NULL);
    /* 10. src= present but empty → fall through to srcset */
    expect("img src=\"\" srcset=\"fallback.png 320w\"", "fallback.png");
    /* 11. src= present but empty + no srcset → data-src */
    expect("img src=\"\" data-src=\"ds.jpg\"", "ds.jpg");
    /* 12. src beats data-src (both present) */
    expect("img src=\"real.png\" data-src=\"shadow.png\"", "real.png");
    /* 13. srcset with leading whitespace in value */
    expect("img srcset=\"  spaced.png 320w\"", "spaced.png");
    /* 14. data-src priority over data-original */
    expect("img data-src=\"first.jpg\" data-original=\"second.jpg\"", "first.jpg");
    /* 15. srcset priority over data-src */
    expect("img srcset=\"set.png 1x\" data-src=\"ds.png\"", "set.png");
    /* 16. empty srcset → fall through to data-src */
    expect("img srcset=\"\" data-src=\"fallthrough.png\"", "fallthrough.png");

    printf("regression: %s\n", fails ? "FAILURES" : "ok (src/srcset/data-src precedence)");

    /* ---- fuzz: truncations + single-byte corruptions of a battery ---- */
    const char *bank[] = {
        "img src=\"http://example.com/img.png\" width=320",
        "img srcset=\"hero.png 640w, hero2x.png 1280w\" alt=\"hi\"",
        "img data-src=\"/images/lazy.jpg\" data-original=\"orig.jpg\"",
        "img src=\"\" srcset=\"fallback.webp 1x\"",
        "img data-lazy-src=\"final.jpg\"",
        "img alt=\"nothing\"",
        "img src=", "img srcset=", "img src=\"", "img srcset=\"",
        "img src=\"\" srcset=\"\" data-src=\"\"",
        0 };
    for (int i = 0; bank[i]; i++) {
        int L = (int)strlen(bank[i]);
        /* all truncations */
        for (int len = 0; len <= L; len++)
            fuzz_one((const unsigned char *)bank[i], len);
        /* single-byte corruptions */
        for (int pos = 0; pos < L && pos < 64; pos++)
            for (int b = 1; b <= 255; b++) {
                char tmp[128]; memcpy(tmp, bank[i], (size_t)L);
                tmp[pos] ^= (char)b;
                fuzz_one((const unsigned char *)tmp, L);
            }
    }

    /* ---- fuzz: random attr-char-biased slices ---- */
    srand(0xB4D1);
    const char *alpha = "abcdefghijklmnopqrstuvwxyz-=\"'/ \t0123456789.:";
    int alen = (int)strlen(alpha);
    for (int trial = 0; trial < 200000; trial++) {
        int len = rand() % 80;
        unsigned char tmp[80];
        for (int i = 0; i < len; i++)
            tmp[i] = (trial & 1) ? (unsigned char)alpha[rand() % alen] : (unsigned char)rand();
        fuzz_one(tmp, len);
    }

    printf("fuzz: truncations/corruptions + 200000 random slices -> %s\n",
           fails ? "FAILURES" : "all clean");
    if (fails) { printf("FAIL: %d check(s) failed\n", fails); return 1; }
    printf("PASS: img_src_attr (src/srcset/data-* precedence, ASan/UBSan clean)\n");
    return 0;
}
