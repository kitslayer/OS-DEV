/* cssprop.c — inline-style property scanning over untrusted page bytes.
 *
 * Split out of browser.c (M583). style_prop is the scanner the browser's
 * per-property style helpers (colour, font-weight, text-align, font-size, …) all
 * build on; it walks a page's style="" / <style> bytes, which are attacker-
 * controlled. Every read is bounded by the length n. Pure — fuzzed by tests/css. */
#include "cssprop.h"
#include "htmlattr.h"   /* lc() — ASCII lowercase */

int style_prop(const char *s, int n, const char *prop, int plen, int *vs, int *ve) {
    for (int i = 0; i + plen + 1 <= n; i++) {                /* room for prop + ':' */
        int m = 1;
        for (int j = 0; j < plen; j++) if (lc(s[i+j]) != prop[j]) { m = 0; break; }
        if (!m || s[i+plen] != ':') continue;
        char before = (i > 0) ? s[i-1] : ' ';                /* must start a property (not a hyphen-suffix like -weight) */
        if (!(before==' '||before==';'||before=='\t'||before=='\n'||before=='"'||before=='\'')) continue;
        int k = i + plen + 1; while (k < n && (s[k]==' '||s[k]=='\t')) k++;   /* ws after ':' */
        int a = k; while (k < n && s[k] != ';' && s[k] != '}') k++;          /* value to ';' or end */
        int e = k; while (e > a && (s[e-1]==' '||s[e-1]=='\t')) e--;          /* trim trailing ws */
        *vs = a; *ve = e; return 1;
    }
    return 0;
}
