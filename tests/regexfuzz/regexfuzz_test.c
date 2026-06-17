/*
 * regexfuzz_test.c — host-side fuzz of the engine's regex (ASan + UBSan).
 *
 * Web-page scripts build regexes from untrusted data and run them on untrusted
 * strings, in-kernel on a guard-page-less stack. The codebase's own history
 * records that the regex engine was the source of CRITICAL kernel bugs (two
 * matcher stack-overflows). So a pathological pattern (nested quantifiers, deep
 * groups, huge {n,m}, unterminated classes) compiled and run on adversarial
 * input must never read out of bounds, overflow the stack, or hang.
 *
 * This #includes js.c (JS_NO_MAIN) and drives re_compile + re_search directly
 * over fuzzed patterns and subjects in exactly-sized buffers (ASan red-zones any
 * over-read). The matcher's step-budget + depth guard must keep every run
 * bounded. A few known patterns confirm correct matching. Exit 0 = pass.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define JS_HOSTTEST 1
#define JS_NO_MAIN 1
#include "js.c"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", (msg)); fails++; } } while (0)

static void reset(void){ g_arena_off = 0; g_oom = 0; g_err = 0; g_threw = 0; g_errmsg[0] = 0; g_depth = 0; }

/* compile `pat` and (if it compiled) search `subj` — all in heap buffers so an
 * over-read past either input trips an ASan red-zone. */
static void fuzz_one(const char *pat, int plen, const char *subj, int slen, const char *flags) {
    char *p = malloc(plen + 1); memcpy(p, pat, plen); p[plen] = 0;
    char *s = malloc(slen + 1); memcpy(s, subj, slen); s[slen] = 0;
    reset();
    regex *re = re_compile(p, flags);
    if (re && re->ok) {
        int caps[2 * (RE_MAXGROUP + 1)];
        re_search(re, s, slen, 0, caps);   /* result ignored; only memory/termination safety matters */
    }
    free(p); free(s);
}

/* convenience: does `pat` match somewhere in `subj`? */
static int matches(const char *pat, const char *subj, const char *flags) {
    reset();
    regex *re = re_compile(pat, flags);
    if (!re || !re->ok) return -1;
    int caps[2 * (RE_MAXGROUP + 1)];
    return re_search(re, subj, (int)strlen(subj), 0, caps) >= 0;
}

int main(void) {
    /* ---- regression: known patterns match as expected ---- */
    CHECK(matches("\\d+", "ab123", 0) == 1, "digit class");
    CHECK(matches("^a.*z$", "abcz", 0) == 1, "anchors + dotstar");
    CHECK(matches("cat|dog", "hotdog", 0) == 1, "alternation");
    CHECK(matches("a{2,3}", "aaa", 0) == 1, "bounded quantifier");
    CHECK(matches("xyz", "abc", 0) == 0, "no match");
    CHECK(matches("(?:ab)+", "abab", 0) == 1, "non-capturing group");
    CHECK(matches("HELLO", "hello", "i") == 1, "case-insensitive flag");
    CHECK(matches("<.+?>", "<a><b>", 0) == 1, "lazy quantifier compiles+runs");
    printf("regression: %s\n", fails ? "FAILURES" : "ok (known patterns match correctly)");

    /* ---- adversarial known patterns x adversarial subjects (ReDoS shapes) ---- */
    const char *evil[] = {
        "(a+)+$", "(a*)*", "(a|a)*b", "(.*)*", "(a+)+(b+)+",
        "a{1,1000000}", "((((((((((a))))))))))", "[a-", "\\", "(", ")", "[", "*", "+?", "a**",
        "(?:", "\\d{0,}", "(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)", "(?:x){5,2}", 0 };
    for (int i = 0; evil[i]; i++) {
        char aaa[256]; for (int k = 0; k < 255; k++) aaa[k] = 'a'; aaa[255] = 0;
        fuzz_one(evil[i], (int)strlen(evil[i]), aaa, 255, 0);
        fuzz_one(evil[i], (int)strlen(evil[i]), "aaaaaaaaaaaaaaaaaaaab", 21, "i");
    }

    /* ---- fuzz: random patterns biased to regex metachars x random subjects ---- */
    srand(1313);
    const char *meta = "()[]{}|.*+?^$\\-,0123abc";
    int mlen = (int)strlen(meta);
    const char *flagset[] = { 0, "i", "g", "gi" };
    for (int trial = 0; trial < 200000; trial++) {
        int plen = rand() % 24;
        char pat[24];
        for (int i = 0; i < plen; i++) pat[i] = (trial & 1) ? meta[rand() % mlen] : (char)(33 + rand() % 94);
        int slen = rand() % 48;
        char subj[48];
        for (int i = 0; i < slen; i++) subj[i] = (trial & 2) ? (char)('a' + rand() % 3) : (char)rand();
        fuzz_one(pat, plen, subj, slen, flagset[rand() % 4]);
    }

    printf("fuzz: ReDoS shapes + 200000 random pattern/subject pairs -> %s\n",
           fails ? "FAILURES" : "all clean (bounded, no OOB/overflow)");
    if (fails) { printf("FAIL: %d check(s) failed\n", fails); return 1; }
    printf("PASS: regex engine (compile+search, ReDoS/malformed fuzz, ASan/UBSan clean)\n");
    return 0;
}
