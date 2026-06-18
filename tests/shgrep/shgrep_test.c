/*
 * shgrep_test.c — host-side regression + fuzz test of the shell's grep regex
 * matcher (user/shgrep.h: gr_match), under ASan + UBSan.
 *
 * The shell's `grep` matches its pattern with this tiny regex engine (^ $ . *
 * [..] character classes, \ escapes, -i case fold). A pattern with no
 * metacharacters must behave exactly like a literal substring search, so
 * existing greps stay correct. This locks the matcher's semantics + asserts it
 * never reads out of bounds on adversarial patterns/texts. Exit 0 = pass.
 */
#include "shgrep.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); fails++; } } while (0)
static void M(const char *re, const char *t, int ci, int exp, const char *m) {
    CHECK(gr_match(re, t, ci) == exp, m);
}

int main(void) {
    /* ---- literal (no metacharacters) behaves like substring search ---- */
    M("foo", "a foo b", 0, 1, "literal match");
    M("foo", "a bar b", 0, 0, "literal no-match");
    M("", "anything", 0, 1, "empty pattern matches");
    /* ---- ^ $ anchors ---- */
    M("^foo", "foobar", 0, 1, "^ at start");
    M("^foo", "a foo", 0, 0, "^ not mid-string");
    M("bar$", "foobar", 0, 1, "$ at end");
    M("bar$", "barfoo", 0, 0, "$ not mid-string");
    M("^abc$", "abc", 0, 1, "^$ exact");
    M("^abc$", "abcd", 0, 0, "^$ rejects longer");
    M("^$", "", 0, 1, "^$ empty line");
    M("^$", "x", 0, 0, "^$ rejects non-empty");
    /* ---- . any char ---- */
    M("a.c", "axc", 0, 1, ". matches one");
    M("a.c", "ac", 0, 0, ". needs a char");
    /* ---- * zero-or-more ---- */
    M("ab*c", "ac", 0, 1, "* zero");
    M("ab*c", "abbbc", 0, 1, "* many");
    M("ab*c", "axc", 0, 0, "* wrong char");
    M("a.*z", "abcz", 0, 1, ".* greedy");
    M("a.*z", "abc", 0, 0, ".* needs z");
    /* ---- [..] classes: ranges, negation, leading-] literal, [..]* ---- */
    M("[0-9]", "abc3", 0, 1, "[0-9] range");
    M("[0-9]", "abc", 0, 0, "[0-9] no digit");
    M("[^0-9]", "123a", 0, 1, "[^..] negation");
    M("[^0-9]", "123", 0, 0, "[^..] all excluded");
    M("a[bc]d", "acd", 0, 1, "[bc] member");
    M("a[bc]d", "aed", 0, 0, "[bc] non-member");
    M("[0-9]*x", "123x", 0, 1, "[..]* many");
    M("[0-9]*x", "x", 0, 1, "[..]* zero");
    M("^[A-Z][a-z]*$", "Hello", 0, 1, "class word");
    M("^[A-Z][a-z]*$", "HELLO", 0, 0, "class word rejects caps tail");
    M("[-a]", "-", 0, 1, "leading-dash literal");
    M("[a-]", "-", 0, 1, "trailing-dash literal");
    /* ---- \ escape of a metacharacter ---- */
    M("a\\.c", "a.c", 0, 1, "escaped dot literal");
    M("a\\.c", "axc", 0, 0, "escaped dot rejects other");
    /* ---- -i case fold (literal + class) ---- */
    M("FOO", "xfoo", 1, 1, "case-insensitive literal");
    M("FOO", "xfoo", 0, 0, "case-sensitive literal");
    M("[a-c]", "B", 1, 1, "case-insensitive class");
    printf("regression: %s\n", fails ? "FAILURES" : "ok (literal/anchors/./*/classes/escape/-i)");

    /* ---- fuzz: random metachar-biased patterns x random texts; must never OOB ---- */
    srand(20260618);
    const char *meta = "^$.*[]-\\abc09";
    int mlen = (int)strlen(meta);
    for (int trial = 0; trial < 300000; trial++) {
        char pat[20], txt[24];
        int pl = rand() % 19; for (int i = 0; i < pl; i++) pat[i] = (trial & 1) ? meta[rand() % mlen] : (char)(32 + rand() % 95);
        pat[pl] = 0;
        int tl = rand() % 23; for (int i = 0; i < tl; i++) txt[i] = (char)('a' + rand() % 4);
        txt[tl] = 0;
        gr_match(pat, txt, rand() & 1);   /* ASan/UBSan catch any over-read; bounded by pattern length */
    }
    printf("fuzz: 300000 random pattern/text pairs -> %s\n", fails ? "FAILURES" : "all clean (bounded, no OOB)");

    if (fails) { printf("FAIL: %d check(s)\n", fails); return 1; }
    printf("PASS: shell grep matcher (semantics + ASan/UBSan-clean fuzz)\n");
    return 0;
}
