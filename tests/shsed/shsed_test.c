/*
 * shsed_test.c — host-side regression + fuzz test of the shell's sed
 * substitution engine (user/shsed.h: sed_sub), under ASan + UBSan.
 *
 * `sed s/RE/REPL/[g]` applies the grep regex (shgrep.h) to each line and
 * splices in the replacement (with `&` = match, `\&`/`\\` = literals). This
 * locks the substitution semantics (first vs global, empty-match stepping, &
 * and escapes, anchors) and asserts the output is always NUL-terminated and
 * never overruns the caller buffer on adversarial input. Exit 0 = pass.
 */
#include "shsed.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
static void S(const char *line, const char *re, const char *repl, int g, int ci,
              const char *exp, const char *m) {
    char out[256];
    sed_sub(line, re, repl, g, ci, out, (long)sizeof(out));
    if (strcmp(out, exp) != 0) {
        printf("  FAIL: %s  (got \"%s\" want \"%s\")\n", m, out, exp); fails++;
    }
}

int main(void) {
    /* ---- literal first vs global ---- */
    S("hello", "l", "L", 0, 0, "heLlo", "first literal");
    S("hello", "l", "L", 1, 0, "heLLo", "global literal");
    S("hello", "z", "Q", 1, 0, "hello", "no match -> unchanged");
    S("", "x", "y", 1, 0, "", "empty line");
    S("aaa", "a", "XY", 1, 0, "XYXYXY", "replacement grows the line");
    /* ---- & = the whole match; \& \\ literals ---- */
    S("abc", "b", "[&]", 0, 0, "a[b]c", "& = match");
    S("ab", "a", "&&", 0, 0, "aab", "&& duplicates the match");
    S("abc", "b", "\\&", 0, 0, "a&c", "\\& = literal ampersand");
    S("abc", "b", "\\\\", 0, 0, "a\\c", "\\\\ = literal backslash");
    /* ---- regex classes, ., * ---- */
    S("a1b2c3", "[0-9]", "#", 1, 0, "a#b#c#", "class, global");
    S("axbyc", "x.*y", "-", 0, 0, "a-c", ".* greedy span");
    S("ac", "ab*c", "Z", 0, 0, "Z", "* zero");
    S("abbbc", "ab*c", "Z", 0, 0, "Z", "* many");
    /* ---- anchors (note: the shell forces g=0 when RE starts with ^) ---- */
    S("foobar", "^foo", "X", 0, 0, "Xbar", "^ at start");
    S("barfoo", "^foo", "X", 0, 0, "barfoo", "^ only at start");
    S("ab", "^", "X", 1, 0, "Xab", "M1785: ^ with g touches only the start (not XaXbX)");
    S("aab", "^a", "X", 1, 0, "Xab", "M1785: ^-anchored pattern + g matches once at the start");
    S("foobar", "r$", "X", 0, 0, "foobaX", "$ at end");
    S("foo", "$", "!", 0, 0, "foo!", "$ empty match appends");
    /* ---- empty-match stepping must make progress + match GNU's adjacency rule ---- */
    S("abc", "x*", "-", 1, 0, "-a-b-c-", "empty match steps one char");
    S("aab", "a*", "X", 1, 0, "XbX", "empty match adjacent to a non-empty one is skipped");
    S("aa", "a*", "X", 1, 0, "X", "greedy match then no spurious trailing empty");
    S("baa", "a*", "X", 1, 0, "XbX", "leading empty, then greedy");
    /* ---- case fold ---- */
    S("Hello", "h", "J", 0, 1, "Jello", "case-insensitive");
    S("HELLO", "l", "x", 1, 1, "HExxO", "case-insensitive global");
    printf("regression: %s\n", fails ? "FAILURES" : "ok (first/global, &/escapes, classes, anchors, empty-step, -i)");

    /* ---- truncation safety: a tiny buffer must never overflow + always NUL ---- */
    for (int cap = 1; cap <= 8; cap++) {
        char out[8];
        sed_sub("aaaaaaaaaaaa", "a", "XYZ", 1, 0, out, (long)cap);
        if ((int)strlen(out) > cap - 1) { printf("  FAIL: truncation overran cap=%d\n", cap); fails++; }
    }
    printf("truncation: %s\n", fails ? "FAILURES" : "ok (bounded at cap, NUL-terminated)");

    /* ---- fuzz: random RE/REPL/line into a small buffer; ASan/UBSan guard OOB,
     *      and the run must always terminate (empty-match stepping) ---- */
    srand(1234);
    for (int trial = 0; trial < 100000; trial++) {
        /* RE kept to <=5 chars => at most two `*`, bounding the regex engine's
         * nested-`*` backtracking (an existing shgrep property, not sed's) to
         * O(n^2); ample to surface any OOB write or non-termination in sed_sub's
         * splice / empty-step / &-expand / truncation logic. `cap` is often tiny
         * on purpose, to exercise the buffer-full path that once span forever. */
        char re[8], repl[10], line[16], out[40];
        int rl = rand() % 6;
        for (int i = 0; i < rl; i++) re[i] = "ab.*^$[]\\"[rand() % 9];
        re[rl] = 0;
        int pl = rand() % 9;
        for (int i = 0; i < pl; i++) repl[i] = "ab&\\xy"[rand() % 6];
        repl[pl] = 0;
        int ll = rand() % 13;
        for (int i = 0; i < ll; i++) line[i] = (char)('a' + rand() % 3);
        line[ll] = 0;
        int cap = 1 + rand() % 40;
        sed_sub(line, re, repl, rand() & 1, rand() & 1, out, (long)cap);
        if ((int)strlen(out) > cap - 1) { printf("  FAIL: fuzz overran cap\n"); fails++; break; }
    }
    printf("fuzz: 100000 cases -> %s\n", fails ? "FAILURES" : "all clean (bounded, terminates, no OOB)");

    if (fails) { printf("FAIL: %d check(s)\n", fails); return 1; }
    printf("PASS: shell sed engine (semantics + ASan/UBSan-clean fuzz)\n");
    return 0;
}
