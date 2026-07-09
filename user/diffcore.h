/* diffcore.h — a pure line-based diff engine (M1705).
 *
 * Splits two texts into lines and computes a longest-common-subsequence diff,
 * producing a unified-diff-style sequence of entries tagged context/added/
 * removed. Pure (only malloc, no syscalls), so — like calc's calceval.h, sheet's
 * sheeteval.h, plot's ploteval.h and gjson's jsoncore.h — it is host-unit-tested
 * by tests/diff. The GUI (user/gdiff.c) renders the entries with colour.
 *
 * The classic O(n*m) LCS dynamic-programming table is used, capped at DC_MAXLINES
 * lines per side (a static (n+1)^2 int table — no allocator, so it drops straight
 * into the freestanding app); inputs longer than the cap are truncated to it,
 * which is far more than a screenful.
 */
#ifndef DIFFCORE_H
#define DIFFCORE_H

#define DC_MAXLINES 400
#define DC_MAXOUT   (2 * DC_MAXLINES)

typedef struct { char op; const char *text; int len; } dc_entry;   /* op: ' ' context, '-' removed, '+' added */

static dc_entry dc_out[DC_MAXOUT];
static int      dc_n;            /* number of entries */
static int      dc_add, dc_del;  /* added / removed line counts */
static int      dc_dp[(DC_MAXLINES + 1) * (DC_MAXLINES + 1)];   /* LCS table (static, no allocator) */

/* split `s` into up to DC_MAXLINES lines; fill off[]/len[] (excluding the '\n'). */
static int dc_split(const char *s, int *off, int *len) {
    int n = 0, i = 0;
    while (s[i] && n < DC_MAXLINES) {
        int start = i;
        while (s[i] && s[i] != '\n') i++;
        off[n] = start; len[n] = i - start;
        n++;
        if (s[i] == '\n') i++;
    }
    return n;
}

static int dc_eq(const char *a, int ao, int al, const char *b, int bo, int bl) {
    if (al != bl) return 0;
    for (int i = 0; i < al; i++) if (a[ao + i] != b[bo + i]) return 0;
    return 1;
}

/* Compute the diff of texts `a` and `b` into dc_out[0..dc_n). Returns dc_n. */
static int diff_run(const char *a, const char *b) {
    static int ao[DC_MAXLINES], al[DC_MAXLINES], bo[DC_MAXLINES], bl[DC_MAXLINES];
    dc_n = dc_add = dc_del = 0;
    int na = dc_split(a, ao, al), nb = dc_split(b, bo, bl);

    int *dp = dc_dp;
    #define DP(i, j) dp[(i) * (nb + 1) + (j)]
    for (int i = 0; i <= na; i++) DP(i, nb) = 0;
    for (int j = 0; j <= nb; j++) DP(na, j) = 0;
    for (int i = na - 1; i >= 0; i--)
        for (int j = nb - 1; j >= 0; j--) {
            if (dc_eq(a, ao[i], al[i], b, bo[j], bl[j])) DP(i, j) = DP(i + 1, j + 1) + 1;
            else DP(i, j) = DP(i + 1, j) >= DP(i, j + 1) ? DP(i + 1, j) : DP(i, j + 1);
        }

    int i = 0, j = 0;
    while (i < na && j < nb && dc_n < DC_MAXOUT) {
        if (dc_eq(a, ao[i], al[i], b, bo[j], bl[j])) {
            dc_out[dc_n].op = ' '; dc_out[dc_n].text = a + ao[i]; dc_out[dc_n].len = al[i]; dc_n++; i++; j++;
        } else if (DP(i + 1, j) >= DP(i, j + 1)) {
            dc_out[dc_n].op = '-'; dc_out[dc_n].text = a + ao[i]; dc_out[dc_n].len = al[i]; dc_n++; i++; dc_del++;
        } else {
            dc_out[dc_n].op = '+'; dc_out[dc_n].text = b + bo[j]; dc_out[dc_n].len = bl[j]; dc_n++; j++; dc_add++;
        }
    }
    while (i < na && dc_n < DC_MAXOUT) { dc_out[dc_n].op = '-'; dc_out[dc_n].text = a + ao[i]; dc_out[dc_n].len = al[i]; dc_n++; i++; dc_del++; }
    while (j < nb && dc_n < DC_MAXOUT) { dc_out[dc_n].op = '+'; dc_out[dc_n].text = b + bo[j]; dc_out[dc_n].len = bl[j]; dc_n++; j++; dc_add++; }
    #undef DP
    return dc_n;
}

#endif /* DIFFCORE_H */
