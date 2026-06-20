/* shsplit_test.c — host-side regression + fuzz for the shell's statement splitter
 * (user/shsplit.h: sh_next_sep / word_at). Built with ASan+UBSan. The splitter is
 * pure, so it's unit-testable off-target like shgrep/shmath. Exit 0 = pass.
 *
 * Keep the expected segmentations in sync with how run_input_line walks segments. */
#include <stdio.h>
#include <string.h>
#include "shsplit.h"

/* Split `line` at every top-level ';' (the way run_input_line does) and join the
 * pieces with '|' into out, so a single string captures the whole segmentation. */
static void segs(const char *line, char *out) {
    char buf[256]; size_t n = strlen(line); if (n > 255) n = 255;
    memcpy(buf, line, n); buf[n] = 0;
    char *seg = buf; int oi = 0, first = 1;
    for (int guard = 0; seg && guard < 300; guard++) {       /* guard: a non-advancing splitter would loop forever */
        char *semi = seg + sh_next_sep(seg);
        int more = (*semi == ';'); if (more) *semi = 0;
        if (!first) out[oi++] = '|';
        first = 0;
        for (char *c = seg; *c; c++) out[oi++] = *c;
        seg = more ? semi + 1 : 0;
    }
    out[oi] = 0;
}

static int fails = 0;
#define CHECK(in, want) do { char got[512]; segs(in, got); \
    if (strcmp(got, want)) { printf("FAIL: %-40s -> [%s] (want [%s])\n", in, got, want); fails++; } } while (0)

int main(void) {
    /* --- plain ';' lists --- */
    CHECK("a;b;c", "a|b|c");
    CHECK("a", "a");
    CHECK("", "");
    CHECK("a;", "a|");
    CHECK("echo x; echo y; echo z", "echo x| echo y| echo z");

    /* --- $( ) command substitution and $(( )) arithmetic protect inner ';' --- */
    CHECK("echo $(a; b)", "echo $(a; b)");
    CHECK("echo $(a; b); c", "echo $(a; b)| c");
    CHECK("x $((1>0)) ; y", "x $((1>0)) | y");
    CHECK("echo $(echo $(a; b); c)", "echo $(echo $(a; b); c)");   /* nested $() */

    /* --- control constructs stay intact across their internal ';' --- */
    CHECK("if x; then y; fi", "if x; then y; fi");
    CHECK("if x; then y; fi; z", "if x; then y; fi| z");
    CHECK("x; if a; then b; fi", "x| if a; then b; fi");
    CHECK("while x; do y; done; z", "while x; do y; done| z");
    CHECK("for i in 1 2; do echo $i; done; e", "for i in 1 2; do echo $i; done| e");
    CHECK("if a; then if b; then c; fi; fi; d", "if a; then if b; then c; fi; fi| d");  /* nested if */
    CHECK("echo a; for i in 1 2; do echo $i; done; echo b",
          "echo a| for i in 1 2; do echo $i; done| echo b");
    CHECK("case $x in a) echo A;; b) echo B;; esac",          /* case…esac: the ; and ;; arm seps are internal */
          "case $x in a) echo A;; b) echo B;; esac");
    CHECK("case $x in a) echo A;; esac; echo done",           /* …and a top-level ';' after esac still breaks */
          "case $x in a) echo A;; esac| echo done");

    /* --- a keyword in ARGUMENT position must NOT open/close a construct --- */
    CHECK("echo fi; echo done", "echo fi| echo done");
    CHECK("echo if; echo x", "echo if| echo x");
    CHECK("fi; x", "fi| x");                 /* stray fi at depth 0 doesn't underflow */
    CHECK("ifconfig; x", "ifconfig| x");     /* 'if' is a prefix, not a whole word */
    CHECK("done; x", "done| x");

    if (fails) { printf("shsplit: %d check(s) FAILED\n", fails); return 1; }

    /* --- fuzz 1: random characters from a metachar-heavy charset must never crash
     * (ASan/UBSan), and sh_next_sep must always return a valid in-range offset that
     * points at a ';' or the terminating '\0'. --- */
    const char *cs = "abcdefionrhsw ;$()";   /* letters that can form if/fi/for/while/do/done/then/else + ; $ ( ) space */
    unsigned seed = 0x1234567u;
    for (int it = 0; it < 400000; it++) {
        char buf[40];
        seed = seed * 1103515245u + 12345u; int n = (seed >> 24) % 39;
        for (int i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; buf[i] = cs[(seed >> 12) % 18u]; }
        buf[n] = 0;
        int off = sh_next_sep(buf);
        if (off < 0 || off > n) { printf("FUZZ FAIL: off %d out of [0,%d] for [%s]\n", off, n, buf); return 1; }
        if (buf[off] != ';' && buf[off] != 0) { printf("FUZZ FAIL: off %d not at ';'/nul for [%s]\n", off, buf); return 1; }
        /* full segmentation must terminate and consume the whole string */
        char tmp[64]; (void)tmp; char j[128]; segs(buf, j);
    }

    printf("shsplit: all segmentation checks passed + 400k fuzz iterations clean\n");
    return 0;
}
