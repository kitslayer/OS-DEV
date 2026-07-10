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

/* Split a "host[:port]" authority (as produced by url_split) into the bare
 * hostname (into host[0..hostsz), NUL-terminated) and return the TCP port.
 * A single trailing ":<digits>" in 1..65535 is honored; no port, a bare
 * trailing ':', ":0", an out-of-range value, or non-numeric junk all yield
 * `defport`. With no ":port", host[] equals the input byte-for-byte. The
 * hostname is what DNS/SNI/cert-matching must use; the caller keeps the full
 * "host:port" for the HTTP Host: header. IPv6 literals ([::1]) are unsupported
 * by the resolver, so only a single trailing port is parsed. */
int url_host_port(const char *hostport, char *host, int hostsz, int defport);

/* Copy the HTTP request-target out of `path`, dropping any "#fragment": the
 * fragment is a client-side concern and must never appear in the request line
 * (RFC 7230 origin-form is absolute-path [ "?" query ] — no fragment). The
 * query is kept. The caller's stored URL keeps its fragment (for scroll-to);
 * only what goes on the wire is stripped. Writes NUL-terminated into out. */
void url_request_path(const char *path, char *out, int outsz);

/* Resolve a raw <img src> slice src[0..srcl) (NOT NUL-terminated) against the
 * page URL `base` into out[0..outsz), NUL-terminated. Absolute http(s) is kept;
 * protocol-relative //host, root-relative /path, and dir-relative are resolved
 * against base's scheme/host/dir; file:/data: are rejected. Returns 1 on
 * success, 0 if it can't be resolved or won't fit. */
int resolve_img_url(const char *base, const char *src, int srcl, char *out, int outsz);
