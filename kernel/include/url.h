/* url.h — URL splitting/resolution over untrusted page bytes (see url.c).
 *
 * The browser feeds these URLs from arbitrary web content: the address bar, a
 * page's <a href>, an <img src>, or a redirect Location. They write into fixed
 * caller buffers, so every write is length-bounded — a hostile/oversized URL
 * must never overrun the host[] / out[] buffer on the guard-page-less kernel
 * stack. Split out of browser.c (M580) so they can be fuzzed in isolation. */
#pragma once

/* Split `url` into its host (into host[0..hostsz), NUL-terminated) and return a
 * pointer to the path within `url` ("/" if none). Skips an http(s):// scheme. */
const char *url_split(const char *url, char *host, int hostsz);

/* Resolve a raw <img src> slice src[0..srcl) (NOT NUL-terminated) against the
 * page URL `base` into out[0..outsz), NUL-terminated. Absolute http(s) is kept;
 * protocol-relative //host, root-relative /path, and dir-relative are resolved
 * against base's scheme/host/dir; file:/data: are rejected. Returns 1 on
 * success, 0 if it can't be resolved or won't fit. */
int resolve_img_url(const char *base, const char *src, int srcl, char *out, int outsz);
