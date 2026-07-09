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

/* Format the last diff_run() result as a real unified-diff patch into `out`
 * (<= max-1 bytes + NUL): a ---/+++ header pair (labelled `fa`/`fb`) then one or
 * more `@@ -as,ac +bs,bc @@` hunks, each covering a run of changes plus up to
 * DC_CONTEXT surrounding context lines (nearby changes merge into one hunk).
 * Returns the byte length (0 + "" if the files are identical). Pure; host-tested.
 * Must be called after diff_run(). */
#define DC_CONTEXT 3
static int diff_to_patch(const char *fa, const char *fb, char *out, int max) {
    if (max <= 0) return 0;
    if (dc_add == 0 && dc_del == 0) { out[0] = 0; return 0; }   /* identical -> empty patch */

    static int  aln[DC_MAXOUT], bln[DC_MAXOUT];   /* 1-based line-in-A / line-in-B at each entry */
    static char inh[DC_MAXOUT];                   /* is entry k within a hunk (near a change)? */
    int ai = 1, bi = 1;
    for (int k = 0; k < dc_n; k++) {
        aln[k] = ai; bln[k] = bi; inh[k] = 0;
        if (dc_out[k].op == ' ') { ai++; bi++; }
        else if (dc_out[k].op == '-') ai++;
        else bi++;
    }
    for (int k = 0; k < dc_n; k++)
        if (dc_out[k].op != ' ') {                /* mark +-DC_CONTEXT entries around each change */
            int lo = k - DC_CONTEXT, hi = k + DC_CONTEXT;
            if (lo < 0) lo = 0;
            if (hi >= dc_n) hi = dc_n - 1;
            for (int m = lo; m <= hi; m++) inh[m] = 1;
        }

    int po = 0;
    #define DPUTS(s) do { const char *_s = (s); while (*_s && po < max - 1) out[po++] = *_s++; } while (0)
    #define DPUTC(ch) do { if (po < max - 1) out[po++] = (char)(ch); } while (0)
    #define DPUTN(v) do { char _b[12]; int _n = 0; unsigned _u = (unsigned)(v); \
        if (!_u) _b[_n++] = '0'; while (_u) { _b[_n++] = (char)('0' + _u % 10); _u /= 10; } \
        while (_n) DPUTC(_b[--_n]); } while (0)

    DPUTS("--- "); DPUTS(fa); DPUTC('\n');
    DPUTS("+++ "); DPUTS(fb); DPUTC('\n');

    int k = 0;
    while (k < dc_n) {
        if (!inh[k]) { k++; continue; }
        int lo = k; while (k < dc_n && inh[k]) k++;   /* one hunk = entries [lo, k) */
        int ac = 0, bc = 0;
        for (int m = lo; m < k; m++) { if (dc_out[m].op != '+') ac++; if (dc_out[m].op != '-') bc++; }
        int as = ac ? aln[lo] : aln[lo] - 1;          /* empty A count -> the preceding A line (GNU convention) */
        int bs = bc ? bln[lo] : bln[lo] - 1;
        DPUTS("@@ -"); DPUTN(as); DPUTC(','); DPUTN(ac); DPUTS(" +"); DPUTN(bs); DPUTC(','); DPUTN(bc); DPUTS(" @@\n");
        for (int m = lo; m < k; m++) {
            DPUTC(dc_out[m].op);
            for (int i = 0; i < dc_out[m].len && po < max - 1; i++) out[po++] = dc_out[m].text[i];
            DPUTC('\n');
        }
    }
    #undef DPUTS
    #undef DPUTC
    #undef DPUTN
    out[po] = 0;
    return po;
}

#endif /* DIFFCORE_H */
