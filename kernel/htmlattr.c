/* htmlattr.c — HTML attribute scanning over untrusted page bytes.
 *
 * Split out of browser.c (M566). The browser hands these a slice of a tag's
 * attribute bytes (fetched from an arbitrary web server); every read is bounded
 * by the explicit length `n`, so a truncated or malformed tag cannot read past
 * the slice. Pure functions (no kernel state) — fuzzed by tests/htmlattr. */
#include "htmlattr.h"

/* HTML whitespace between/around attributes: space, tab, newline, CR, form-feed
 * (HTML "space characters"). A multi-line tag's attribute slice carries the raw
 * '\n'/'\r', so recognizing only space+tab missed attributes after a newline and
 * let an unquoted value run past one (e.g. `type=text\n` -> value "text\n"). M1778 */
static int aws(int c) { return c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='\f'; }

/* Find attribute `name`="..." (or '...' or bare) within an attribute slice.
 * The name must start at a token boundary (so "href" won't match inside another
 * attribute or value). */
int find_attr(const char *a, int n, const char *name, const char **val, int *vlen) {
    int nl = 0; while (name[nl]) nl++;
    for (int i = 0; i + nl <= n; i++) {
        if (i > 0 && !aws(a[i-1])) continue;  /* attr boundary */
        int m = 0; while (m < nl && lc(a[i+m]) == name[m]) m++;
        if (m != nl) continue;
        int k = i + nl; while (k < n && aws(a[k])) k++;
        if (k < n && a[k] == '=') {
            k++; while (k < n && aws(a[k])) k++;
            char q = 0; if (k < n && (a[k]=='"'||a[k]=='\'')) { q = a[k]; k++; }
            int s = k;
            while (k < n && (q ? a[k]!=q : (!aws(a[k]) && a[k]!='>'))) k++;
            *val = a + s; *vlen = k - s; return 1;
        }
    }
    return 0;
}

/* Is a (possibly valueless) boolean attribute present? e.g. `checked`, `disabled`. */
int has_attr(const char *a, int n, const char *name) {
    int nl = 0; while (name[nl]) nl++;
    for (int i = 0; i + nl <= n; i++) {
        if (i > 0 && !aws(a[i-1])) continue;       /* attr boundary */
        int m = 0; while (m < nl && lc(a[i+m]) == name[m]) m++;
        if (m != nl) continue;
        int k = i + nl;                                               /* must be a complete token */
        if (k >= n || aws(a[k]) || a[k]=='=' || a[k]=='>' || a[k]=='/') return 1;
    }
    return 0;
}

/* Parse a numeric attribute (e.g. width="48"); 0 if absent/non-numeric. */
int attr_int(const char *a, int n, const char *name) {
    const char *v; int vl;
    if (!find_attr(a, n, name, &v, &vl)) return 0;
    int x = 0, i = 0;
    while (i < vl && v[i] >= '0' && v[i] <= '9') { x = x * 10 + (v[i] - '0'); if (x > 8192) return 8192; i++; }
    return x;
}

int find_href(const char *a, int n, const char **val, int *vlen) {
    return find_attr(a, n, "href", val, vlen);
}
