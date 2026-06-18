/* htmlattr.h — HTML attribute scanning over untrusted page bytes (see htmlattr.c).
 *
 * The browser tokenizes a tag's attribute span out of bytes fetched from an
 * arbitrary (possibly hostile) web server, then asks these helpers for specific
 * attributes. They take an explicit length-bounded slice a[0..n) and never read
 * past it, so a malformed/truncated tag can't OOB-read the kernel stack. Split
 * out of browser.c (M566) so they can be fuzzed in isolation. */
#pragma once

/* ASCII lowercase — HTML tag/attribute names are case-insensitive. Inline in
 * the header so both browser.c and htmlattr.c share one definition. */
static inline int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Find attribute `name`="..." (or '...' or bare value) in the attribute slice
 * a[0..n). The name must start at a token boundary. On a match with a value,
 * the val/vlen out-params point at the (unquoted) value within the slice and it
 * returns 1; otherwise returns 0. */
int find_attr(const char *a, int n, const char *name, const char **val, int *vlen);

/* Is a (possibly valueless) boolean attribute present? e.g. `checked`, `disabled`. */
int has_attr(const char *a, int n, const char *name);

/* Numeric attribute value (e.g. width="48"), clamped to [0,8192]; 0 if absent or
 * non-numeric. */
int attr_int(const char *a, int n, const char *name);

/* Convenience: find the href="..." value. */
int find_href(const char *a, int n, const char **val, int *vlen);
