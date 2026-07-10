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

/* Split "host[:port]" into a bare hostname + return the TCP port (see url.h).
 * The connection identity (DNS lookup, TLS SNI, cert hostname match) must use
 * the bare host; only the HTTP Host: header keeps the ":port". A malformed or
 * out-of-range port falls back to `defport` (so a hostile authority can't pick
 * an absurd port or wedge the parse). */
int url_host_port(const char *hostport, char *host, int hostsz, int defport) {
    int i = 0;
    while (hostport[i] && hostport[i] != ':' && i < hostsz - 1) { host[i] = hostport[i]; i++; }
    host[i] = 0;
    if (hostport[i] != ':') return defport;          /* no port (or host filled the buffer before any ':') */
    const char *pp = hostport + i + 1;
    if (!*pp) return defport;                        /* bare "host:" */
    int port = 0;
    for (const char *d = pp; *d; d++) {
        if (*d < '0' || *d > '9') return defport;    /* non-numeric junk -> default */
        port = port * 10 + (*d - '0');
        if (port > 65535) return defport;            /* out of range -> default */
    }
    return port ? port : defport;                    /* ":0" -> default */
}

/* Resolve a raw <img src> (as it appears in the HTML) into a full absolute URL,
 * the SAME way goto_href resolves a link: an absolute http(s):// src is kept;
 * a protocol-relative //host/path, root-relative /path, or dir-relative src is
 * resolved against `base` (the page URL), keeping the page's scheme. file:/data:
 * srcs are rejected (returns 0). Writes a NUL-terminated URL into out[<outsz];
 * returns 1 on success, 0 if it can't be resolved or won't fit. */
/* RFC 3986 remove_dot_segments, in place, on the path that starts with '/'.
 * Collapses "/./" and "/foo/../" (clamped at root) so the resolved URL the
 * client sends is canonical — a strict server/CDN may 404 on a literal "../".
 * Stops at ?/# and copies the query/fragment verbatim. */
static void norm_path(char *path) {
    if (*path != '/') return;
    char *w = path; const char *r = path;
    *w++ = '/'; r++;                                          /* keep the leading '/' */
    while (*r && *r != '?' && *r != '#') {
        if (r[0]=='.' && (r[1]=='/' || r[1]==0)) { r += (r[1]=='/')?2:1; }            /* "." segment */
        else if (r[0]=='.' && r[1]=='.' && (r[2]=='/' || r[2]==0)) { r += (r[2]=='/')?3:2;  /* ".." pops a segment */
            if (w > path+1) { w--; w--; while (w>path && *w!='/') w--; w++; } }
        else { while (*r && *r!='/' && *r!='?' && *r!='#') *w++=*r++; if (*r=='/') *w++=*r++; }
    }
    while (*r) *w++=*r++;
    *w = 0;
}
/* point at the path ('/…') of "scheme://host/path", or the trailing NUL if none. */
static char *url_path_start(char *u) {
    char *p = u;
    if (startsw(p,"https://")) p += 8; else if (startsw(p,"http://")) p += 7;
    while (*p && *p != '/') p++;
    return p;
}

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
        norm_path(url_path_start(out));                   /* canonicalize ./ and ../ */
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
        out[p] = 0; norm_path(url_path_start(out)); return p > 0;
    }
    for (int i = 0; host[i] && p < outsz - 1; i++) out[p++] = host[i];
    if (srcl >= 1 && src[0] == '/') {                     /* absolute path */
        for (int i = 0; i < srcl && p < outsz - 1; i++) out[p++] = src[i];
    } else if (srcl >= 1 && src[0] == '?') {              /* M1772: query-only ref -> keep the base PATH, replace only the query (mirrors goto_href, M1771; e.g. fetch("?page=2")) */
        const char *cp = cu + hi;                         /* base path incl leading '/' */
        if (!cp[0] && p < outsz - 1) out[p++] = '/';
        for (int i = 0; cp[i] && cp[i] != '?' && cp[i] != '#' && p < outsz - 1; i++) out[p++] = cp[i];   /* base path, sans its own query/fragment */
        for (int i = 0; i < srcl && p < outsz - 1; i++) out[p++] = src[i];   /* the new ?query */
    } else {                                              /* relative to current dir */
        const char *cp = cu + hi;                         /* current path incl leading '/' */
        int lastslash = 0;
        for (int i = 0; cp[i] && cp[i] != '?' && cp[i] != '#'; i++) if (cp[i] == '/') lastslash = i + 1;   /* M1772: the base directory is in the PATH only -- a '/' in the base's ?query is not a separator */
        if (p < outsz - 1) out[p++] = '/';
        for (int i = 0; i < lastslash && cp[i] && p < outsz - 1; i++)
            if (!(i == 0 && cp[0] == '/')) out[p++] = cp[i];
        for (int i = 0; i < srcl && p < outsz - 1; i++) out[p++] = src[i];
    }
    out[p] = 0;
    norm_path(url_path_start(out));                       /* canonicalize ./ and ../ (RFC 3986) */
    return p > 0;
}
