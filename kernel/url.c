/* url.c — URL splitting/resolution over untrusted page bytes.
 *
 * Split out of browser.c (M580). The browser hands these URLs straight from web
 * content (address bar, <a href>, <img src>, redirect Location); every write is
 * bounded by the caller's buffer size, so an oversized/malformed URL can't
 * overrun. Pure functions (no browser state) — fuzzed by tests/url. */
#include "url.h"
#include "htmlattr.h"   /* lc() — ASCII lowercase */

/* local prefix test (browser.c has its own static startsw for its other uses) */
static int startsw(const char *s, const char *p) {
    while (*p) { if (*s != *p) return 0; s++; p++; }
    return 1;
}

/* Split a URL into host[] + return the path ("/" if none). */
const char *url_split(const char *url, char *host, int hostsz) {
    const char *u = url;
    if      (startsw(u, "http://"))  u += 7;
    else if (startsw(u, "https://")) u += 8;
    int hi = 0;
    while (*u && *u != '/' && hi < hostsz - 1) host[hi++] = *u++;
    host[hi] = 0;
    return (*u == '/') ? u : "/";
}

/* Resolve a raw <img src> (as it appears in the HTML) into a full absolute URL,
 * the SAME way goto_href resolves a link: an absolute http(s):// src is kept;
 * a protocol-relative //host/path, root-relative /path, or dir-relative src is
 * resolved against `base` (the page URL), keeping the page's scheme. file:/data:
 * srcs are rejected (returns 0). Writes a NUL-terminated URL into out[<outsz];
 * returns 1 on success, 0 if it can't be resolved or won't fit. */
int resolve_img_url(const char *base, const char *src, int srcl, char *out, int outsz) {
    if (srcl <= 0 || outsz < 2) return 0;
    /* reject file:/data: (and any scheme we don't fetch over the network) */
    if (srcl >= 5 && lc(src[0])=='f'&&lc(src[1])=='i'&&lc(src[2])=='l'&&lc(src[3])=='e'&&src[4]==':') return 0;
    if (srcl >= 5 && lc(src[0])=='d'&&lc(src[1])=='a'&&lc(src[2])=='t'&&lc(src[3])=='a'&&src[4]==':') return 0;
    /* `src` points into the HTML and is only guaranteed valid for srcl bytes
     * (not NUL-terminated), so detect an absolute URL with an explicit
     * length-bounded prefix compare rather than startsw. */
    int isabs = 0;
    if (srcl >= 7) { const char *h = "http://";  int m=1; for (int k=0;k<7;k++) if (lc(src[k])!=h[k]) { m=0; break; } if (m) isabs=1; }
    if (!isabs && srcl >= 8) { const char *h = "https://"; int m=1; for (int k=0;k<8;k++) if (lc(src[k])!=h[k]) { m=0; break; } if (m) isabs=1; }
    int p = 0;
    if (isabs) {                                          /* absolute: copy verbatim */
        for (int i = 0; i < srcl && p < outsz - 1; i++) out[p++] = src[i];
        if (p >= outsz - 1 && srcl > p) return 0;         /* would truncate -> skip */
        out[p] = 0;
        return 1;
    }
    /* relative — resolve against `base`, keeping its scheme (mirrors goto_href) */
    const char *cu = base;
    const char *scheme = startsw(cu, "https://") ? "https://" : "http://";
    if (startsw(cu, "http://")) cu += 7; else if (startsw(cu, "https://")) cu += 8;
    char host[96]; int hi = 0; while (cu[hi] && cu[hi] != '/' && hi < 95) { host[hi] = cu[hi]; hi++; } host[hi] = 0;
    for (const char *s = scheme; *s && p < outsz - 1; s++) out[p++] = *s;   /* scheme prefix */
    if (srcl >= 2 && src[0] == '/' && src[1] == '/') {    /* protocol-relative //host/path */
        for (int i = 2; i < srcl && p < outsz - 1; i++) out[p++] = src[i];
        out[p] = 0; return p > 0;
    }
    for (int i = 0; host[i] && p < outsz - 1; i++) out[p++] = host[i];
    if (srcl >= 1 && src[0] == '/') {                     /* absolute path */
        for (int i = 0; i < srcl && p < outsz - 1; i++) out[p++] = src[i];
    } else {                                              /* relative to current dir */
        const char *cp = cu + hi;                         /* current path incl leading '/' */
        int lastslash = 0;
        for (int i = 0; cp[i]; i++) if (cp[i] == '/') lastslash = i + 1;
        if (p < outsz - 1) out[p++] = '/';
        for (int i = 0; i < lastslash && cp[i] && p < outsz - 1; i++)
            if (!(i == 0 && cp[0] == '/')) out[p++] = cp[i];
        for (int i = 0; i < srcl && p < outsz - 1; i++) out[p++] = src[i];
    }
    out[p] = 0;
    return p > 0;
}
