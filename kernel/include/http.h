/* http.h — parsing helpers for HTTP/1.x responses from untrusted servers.
 *
 * These operate on the raw response bytes a server (or CDN) sends us, so they
 * are bounds-careful by construction and host-fuzzed (tests/http). They are
 * pure: they read/compact the caller's buffer and have no other side effects. */
#pragma once

/* Does the response advertise "Transfer-Encoding: chunked"? Scans only the
 * header region [0, hdr_end), case-insensitive, at line starts. */
int http_is_chunked(const char *raw, int hdr_end);

/* Decode an HTTP chunked-transfer body in place. `body` points at the first
 * chunk-size line; `len` is the bytes available; `cap` caps a single chunk size
 * so the hex accumulator can't overflow. Returns the decoded length. Tolerant
 * of truncation (a fetch may stop mid-stream on a time budget). */
int http_dechunk(char *body, int len, unsigned cap);

/* Find the value of a "Location:" header. Writes up to max-1 bytes + NUL into
 * out and returns 1 if found, else 0. */
int http_find_loc(const char *raw, int n, char *out, int max);
