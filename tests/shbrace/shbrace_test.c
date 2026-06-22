/* shbrace_test.c — host-side regression + fuzz for the shell's brace expansion
 * (user/shbrace.h: expand_braces and helpers). Built with ASan+UBSan. The pass is
 * pure, so it's unit-testable off-target like shgrep/shmath/shsplit. Exit 0 = pass.
 *
 * Keep the expected expansions in sync with how run_line applies expand_braces. */
#include <stdio.h>
#include <string.h>
#include "shbrace.h"

static int fails = 0;
/* expand_braces always copies src->dst first, so `got` holds the result whether or
 * not anything changed. Compare it to the expected expansion. */
#define CHECK(in, want) do { char got[1024]; expand_braces(in, got, (int)sizeof got); \
    if (strcmp(got, want)) { printf("FAIL: %-28s -> [%s] (want [%s])\n", in, got, want); fails++; } } while (0)

int main(void) {
    /* --- comma lists --- */
    CHECK("echo {a,b,c}", "echo a b c");
    CHECK("file{a,b,c}.txt", "filea.txt fileb.txt filec.txt");
    CHECK("{a,}x", "ax x");                 /* trailing empty item */
    CHECK("{,z}y", "y zy");                 /* leading empty item */
    CHECK("pre{x,y}post", "prexpost preypost");

    /* --- numeric / char ranges --- */
    CHECK("{1..5}", "1 2 3 4 5");
    CHECK("x{9..6}", "x9 x8 x7 x6");        /* descending */
    CHECK("{1..10..2}", "1 3 5 7 9");       /* step */
    CHECK("{a..e}", "a b c d e");           /* char range */
    CHECK("{1..3}.txt", "1.txt 2.txt 3.txt");
    CHECK("pre{1..2}post", "pre1post pre2post");
    CHECK("{-2..2}", "-2 -1 0 1 2");        /* negative bounds */
    CHECK("{5..5}", "5");                   /* single-element range */

    /* --- cartesian product of adjacent groups + nesting --- */
    CHECK("{1,2}{3,4}", "13 14 23 24");
    CHECK("a{b,c}d{e,f}", "abde abdf acde acdf");
    CHECK("{a,{b,c},d}", "a b c d");        /* nested: top-level comma split, then recurse */

    /* --- left untouched (the safety rules) --- */
    CHECK("${x}", "${x}");                  /* $ -> parameter expansion, not a brace group */
    CHECK("f() { echo hi; }", "f() { echo hi; }");   /* spaces inside -> command group */
    CHECK("{ cmd; }", "{ cmd; }");          /* command group */
    CHECK("{abc}", "{abc}");                /* no comma / range */
    CHECK("{}", "{}");                      /* empty */
    CHECK("{a,b", "{a,b");                  /* unbalanced (no closing brace) */
    CHECK("a}b", "a}b");                    /* stray close */
    CHECK("", "");
    CHECK("plain text here", "plain text here");
    CHECK("echo val=${x} list={1..3}", "echo val=${x} list=1 list=2 list=3");   /* coexist; preamble applies per item */

    if (fails) { printf("\n%d regression case(s) FAILED\n", fails); return 1; }
    printf("all %s regression cases passed\n", "brace-expansion");

    /* --- fuzz: random brace-ish strings must never crash / OOB / hang ---------
     * ASan/UBSan catch memory errors; the in-function guard caps pass count, so a
     * pathological input can't loop forever. We assert the output stays NUL-
     * terminated within the buffer and that re-expanding the output is a no-op
     * (the result has no expandable braces left). */
    const char alpha[] = "{},..ab12$ \t}{,";   /* heavy on brace-expansion metacharacters */
    unsigned int seed = 0x9e3779b9u;
    for (long iter = 0; iter < 2000000; iter++) {
        char in[48]; int n = (seed >> 5) % 40;
        for (int i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; in[i] = alpha[(seed >> 7) % (int)(sizeof alpha - 1)]; }
        in[n] = 0;
        char out[1024];
        expand_braces(in, out, (int)sizeof out);
        /* must be NUL-terminated within bounds (ASan would already trap an OOB write) */
        if (memchr(out, 0, sizeof out) == NULL) { printf("FAIL: output not terminated for [%s]\n", in); return 1; }
        /* re-expanding a result should change nothing (idempotent / terminates) */
        char out2[1024]; strcpy(out2, out);
        char out3[1024]; expand_braces(out2, out3, (int)sizeof out3);
        if (strcmp(out2, out3) != 0) { printf("FAIL: non-idempotent: [%s] -> [%s] -> [%s]\n", in, out, out3); return 1; }
        seed = seed * 1103515245u + 12345u;
    }
    printf("fuzz: 2000000 random inputs, no crash / OOB / hang, expansion idempotent\n");
    return 0;
}
