/*
 * diff_test.c — host unit tests for the line-diff engine (user/diffcore.h).
 * Pure (malloc only, no syscalls), so — like tests/calc, tests/sheet, tests/plot
 * and tests/json — we build it for the host under ASan/UBSan and check the LCS
 * diff: the op sequence (context/added/removed), the +/- counts, and the line
 * text of individual entries. Exit 0 = all pass.
 */
#include <stdio.h>
#include <string.h>
#include "diffcore.h"       /* -Iuser on the compile line */

static int fails, checks;

/* diff a vs b and check the whole op string (' '/'-'/'+') and the +/- counts */
static void chk(const char *a, const char *b, const char *ops, int add, int del) {
    checks++;
    diff_run(a, b);
    char got[DC_MAXOUT + 1]; int i;
    for (i = 0; i < dc_n && i < DC_MAXOUT; i++) got[i] = dc_out[i].op;
    got[i] = 0;
    if (strcmp(got, ops) != 0) { printf("  FAIL ops(%s|%s): got \"%s\" want \"%s\"\n", a, b, got, ops); fails++; }
    else if (dc_add != add || dc_del != del) { printf("  FAIL counts(%s|%s): +%d -%d want +%d -%d\n", a, b, dc_add, dc_del, add, del); fails++; }
}
/* diff a vs b, format as a unified-diff patch, and check the exact text */
static void chk_patch(const char *a, const char *b, const char *want) {
    checks++;
    diff_run(a, b);
    char out[8192];
    diff_to_patch("a", "b", out, sizeof out);
    if (strcmp(out, want) != 0) { printf("  FAIL patch(%s|%s):\n--got--\n%s--want--\n%s\n", a, b, out, want); fails++; }
}
/* check that entry idx (from the most recent diff_run) has this op + text */
static void chk_line(int idx, char op, const char *text) {
    checks++;
    if (idx >= dc_n) { printf("  FAIL entry %d missing (dc_n=%d)\n", idx, dc_n); fails++; return; }
    int l = (int)strlen(text);
    if (dc_out[idx].op != op || dc_out[idx].len != l || memcmp(dc_out[idx].text, text, l) != 0) {
        printf("  FAIL entry %d: op '%c' len %d, want op '%c' text \"%s\"\n", idx, dc_out[idx].op, dc_out[idx].len, op, text); fails++;
    }
}

int main(void) {
    printf("line-diff engine tests\n");

    chk("a\nb\nc", "a\nb\nc", "   ", 0, 0);        /* identical */
    chk("", "", "", 0, 0);                          /* both empty */
    chk("a\nb", "a\nb\nc", "  +", 1, 0);            /* append a line */
    chk("a\nb\nc", "a\nb", "  -", 0, 1);            /* delete last line */
    chk("b\nc", "a\nb\nc", "+  ", 1, 0);            /* prepend a line */
    chk("a\nb\nc", "a\nB\nc", " -+ ", 1, 1);        /* change the middle line */
    chk("a", "", "-", 0, 1);
    chk("", "a", "+", 1, 0);
    chk("a\nb\nc\nd", "a\nx\nc\ny", " -+ -+", 2, 2);/* two separate changes */
    chk("a\nb\nc", "c\nb\na", "-- ++", 2, 2);       /* reversed: LCS keeps one common line (c) */

    /* entry text on the middle-change case */
    diff_run("hello\nworld\nend", "hello\nWORLD\nend");
    chk_line(0, ' ', "hello");
    chk_line(1, '-', "world");
    chk_line(2, '+', "WORLD");
    chk_line(3, ' ', "end");

    /* a blank line in the middle is a real line */
    chk("a\n\nb", "a\n\nb", "   ", 0, 0);
    chk("a\nb", "a\n\nb", " + ", 1, 0);             /* a blank line inserted */

    /* --- unified-diff patch output (M1730) --------------------------------*/
    chk_patch("a\nb\nc", "a\nb\nc", "");                          /* identical -> empty patch */
    chk_patch("a\nb\nc", "a\nB\nc",
              "--- a\n+++ b\n@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n");   /* middle change, full context */
    chk_patch("a\nb", "a\nb\nc",
              "--- a\n+++ b\n@@ -1,2 +1,3 @@\n a\n b\n+c\n");       /* append */
    chk_patch("a\nb\nc", "a\nc",
              "--- a\n+++ b\n@@ -1,3 +1,2 @@\n a\n-b\n c\n");       /* delete a middle line */
    {   /* two far-apart changes must split into TWO @@ hunks (context doesn't span the gap) */
        diff_run("A\n1\n2\n3\n4\n5\n6\n7\n8\n9\nB", "X\n1\n2\n3\n4\n5\n6\n7\n8\n9\nY");
        char out[8192]; diff_to_patch("a", "b", out, sizeof out);
        int hunks = 0; for (const char *p = out; (p = strstr(p, "@@ -")) != 0; p += 4) hunks++;
        checks++; if (hunks != 2) { printf("  FAIL multi-hunk: got %d hunks, want 2\n%s\n", hunks, out); fails++; }
    }

    if (!fails) printf("PASS: %d checks, diff engine correct\n", checks);
    else printf("FAIL: %d/%d checks failed\n", fails, checks);
    return fails ? 1 : 0;
}
