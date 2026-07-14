/* shsort.h — the shell's `sort` key helpers: numeric-key parsing (sort -n),
 * case-insensitive equality (sort -uf), and field extraction (sort -kN [-td]).
 * Pure (only gr_lc() from shgrep.h), so host-unit-tested by tests/shsort while
 * user/shell.c #includes it for the `sort` builtin.
 * NOTE: keep in sync with its host test (tests/shsort/shsort_test.c). */
#ifndef SHSORT_H
#define SHSORT_H
#include "shgrep.h"   /* gr_lc(): ASCII lowercase, for the fold comparison */

/* parse a leading (optionally signed) number from a line, for `sort -n`, as a
 * fixed-point long scaled by 1e6 (the shell is built without the FPU/SSE, so no
 * doubles here). Handles a +/- sign and up to 6 decimal places (M1838), so
 * `sort -n` orders 3.14 vs 3.2 vs 10 correctly — was integer-only (3.14 -> 3). */
static long sort_numval(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0; if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    long v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    v *= 1000000;                                        /* scale so a fraction is comparable in integer space */
    if (*s == '.') { s++; long f = 100000; while (*s >= '0' && *s <= '9' && f > 0) { v += (*s - '0') * f; f /= 10; s++; } }
    return neg ? -v : v;
}
static int sort_foldeq(const char *a, const char *b) {   /* case-insensitive string equality (for sort -uf) */
    while (*a && gr_lc(*a) == gr_lc(*b)) { a++; b++; }
    return gr_lc(*a) == gr_lc(*b);
}
/* sort -kN: pointer to the start of field k (1-based) — the key compares from
 * there to end of line, like `sort -kN`. Fields are whitespace-delimited, or
 * split on `d` when `sort -td` was given. Past the last field returns NUL. */
static const char *sort_field(const char *s, int k, char d) {
    for (int f = 1; f < k && *s; f++) {
        if (d) { while (*s && *s != d) s++; if (*s == d) s++; }            /* custom delimiter */
        else { while (*s == ' ' || *s == '\t') s++; while (*s && *s != ' ' && *s != '\t') s++; }
    }
    if (!d) while (*s == ' ' || *s == '\t') s++;   /* whitespace mode skips leading blanks of field k */
    return s;
}
#endif /* SHSORT_H */
