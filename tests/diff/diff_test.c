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
#include "patchcore.h"      /* patch_apply() — the inverse, for the diff->patch->apply round-trip */

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
/* round-trip: patch_apply(a, diff_to_patch(diff_run(a,b))) must reproduce b */
static void chk_roundtrip(const char *a, const char *b) {
    checks++;
    char patch[8192], result[8192];
    diff_run(a, b);
    diff_to_patch("a", "b", patch, sizeof patch);
    int n = patch_apply(a, patch, result, sizeof result);
    if (n < 0) { printf("  FAIL roundtrip(%s|%s): patch_apply reported no match\n", a, b); fails++; }
    else if (strcmp(result, b) != 0) { printf("  FAIL roundtrip(%s|%s):\n--got--\n%s--want--\n%s\n", a, b, result, b); fails++; }
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

    /* --- patch apply (M1731): diff -> patch -> apply must reproduce the target */
    chk_roundtrip("a\nb\nc\n", "a\nb\nc\n");                  /* identical (empty patch) */
    chk_roundtrip("a\nb\nc\n", "a\nB\nc\n");                  /* middle change */
    chk_roundtrip("a\nb\n", "a\nb\nc\n");                     /* append */
    chk_roundtrip("a\nb\nc\n", "a\nc\n");                     /* delete */
    chk_roundtrip("a\nb\nc\n", "x\ny\nz\n");                  /* full replace */
    chk_roundtrip("1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n",
                  "X\n2\n3\n4\n5\n6\n7\n8\n9\nY\n");          /* two far-apart hunks */
    /* a patch whose context/removed lines don't match src must be rejected */
    {
        char r[256];
        int n = patch_apply("y\n", "--- a\n+++ b\n@@ -1,1 +1,2 @@\n x\n+z\n", r, sizeof r);
        checks++; if (n >= 0) { printf("  FAIL patch mismatch should be rejected, got %d\n", n); fails++; }
    }

    /* --- fuzz: correctness + OOB-safety on adversarial input (M1741) ---------
     * (1) round-trip invariant: for ANY two well-formed (newline-terminated)
     *     texts, patch_apply(a, diff_to_patch(diff(a,b))) must reproduce b.
     * (2) OOB-safety: patch_apply must never read past its inputs on a MALFORMED
     *     patch -- it parses attacker-controlled @@ headers + context lines.
     * ASan/UBSan abort on any OOB; a (1) mismatch is a real diff/patch bug.
     * Texts stay well under DC_MAXLINES(400) so nothing is truncated. */
    {
        static unsigned rs = 0x1234567u;
        #define DXR() (rs ^= rs << 13, rs ^= rs >> 17, rs ^= rs << 5, rs)
        char a[256], b[256], patch[8192], result[8192];
        int rtfail = 0, badret = 0;
        for (int it = 0; it < 60000; it++) {
            int la = DXR() % 200, lb = DXR() % 200;               /* two random line-y texts */
            for (int i = 0; i < la; i++) { unsigned r = DXR(); a[i] = (r % 5 == 0) ? '\n' : (char)('a' + r % 4); }
            if (la == 0 || a[la - 1] != '\n') a[la++] = '\n';      /* well-formed: newline-terminated */
            a[la] = 0;
            for (int i = 0; i < lb; i++) { unsigned r = DXR(); b[i] = (r % 5 == 0) ? '\n' : (char)('a' + r % 4); }
            if (lb == 0 || b[lb - 1] != '\n') b[lb++] = '\n';
            b[lb] = 0;

            diff_run(a, b);                                        /* (1) round-trip invariant */
            diff_to_patch("a", "b", patch, sizeof patch);
            int n = patch_apply(a, patch, result, sizeof result);
            if (n < 0 || strcmp(result, b) != 0) rtfail++;

            int plen = (int)strlen(patch);                        /* (2) mutate into a malformed patch */
            if (plen > 0) {
                int muts = 1 + (DXR() % 8);
                for (int m = 0; m < muts; m++) patch[DXR() % plen] = (char)(DXR() & 0xFF);
                if (DXR() % 3 == 0) patch[DXR() % plen] = 0;       /* random truncation */
                int r2 = patch_apply(a, patch, result, sizeof result);
                if (r2 > (int)sizeof result) badret++;
            }
        }
        for (int it = 0; it < 40000; it++) {                      /* pure-random patch bytes, no valid prefix */
            int la = DXR() % 40; for (int i = 0; i < la; i++) a[i] = (char)('a' + DXR() % 3); a[la] = 0;
            int pl = DXR() % 240; for (int i = 0; i < pl; i++) patch[i] = (char)(DXR() & 0xFF); patch[pl] = 0;
            int r3 = patch_apply(a, patch, result, sizeof result);
            if (r3 > (int)sizeof result) badret++;
        }
        checks += 2;
        if (rtfail) { printf("  FAIL fuzz round-trip: %d/60000 mismatches\n", rtfail); fails++; }
        if (badret) { printf("  FAIL fuzz: patch_apply returned an out-of-range length %d times\n", badret); fails++; }
        #undef DXR
    }

    if (!fails) printf("PASS: %d checks, diff engine correct\n", checks);
    else printf("FAIL: %d/%d checks failed\n", fails, checks);
    return fails ? 1 : 0;
}
