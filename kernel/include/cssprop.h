/* cssprop.h — inline-style property scanning over untrusted page bytes (see cssprop.c).
 *
 * The browser reads CSS declarations straight from a page's style="" attribute or
 * <style> body (both attacker-controlled). style_prop is the shared scanner the
 * per-property helpers (colour, font-weight, text-align, …) build on: it finds a
 * `prop:` declaration at a property boundary in s[0..n) and returns the value
 * span. Every read is bounded by n, so a malformed/oversized style can't OOB the
 * guard-page-less kernel stack. Split out of browser.c (M583) to be fuzzed alone. */
#pragma once

/* Find declaration `prop` (length `plen`, must start at a property boundary —
 * not e.g. the `-weight` of `font-weight` when prop is "weight") in the style
 * text s[0..n). On a match, sets the vs/ve out-params to the value span [vs,ve)
 * (whitespace-trimmed, up to the next ';' or '}') and returns 1; else returns 0. */
int style_prop(const char *s, int n, const char *prop, int plen, int *vs, int *ve);
