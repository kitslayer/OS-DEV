/*
 * http.c — HTTP/1.x response parsing helpers (extracted from browser.c so they
 * can be host-fuzzed in isolation; see tests/http).
 *
 * These read response bytes straight off the wire, so every loop is bounded by
 * the caller-supplied length and de-chunking compacts strictly within the
 * buffer (out <= in throughout). A malicious chunk-size line, a missing CRLF,
 * or a truncated stream must never read or write out of bounds.
 */
#include "http.h"
#include "string.h"

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int http_is_chunked(const char *raw, int hdr_end) {
    const char *key = "transfer-encoding:";
    for (int i = 0; i + 18 < hdr_end; i++) {
        if (i && raw[i-1] != '\n') continue;          /* only at line starts */
        int j = 0;
        while (key[j] && lc(raw[i+j]) == key[j]) j++;
        if (key[j]) continue;
        for (int k = i + 18; k < hdr_end && raw[k] != '\n'; k++)   /* scan the value */
            if (k + 6 < hdr_end && lc(raw[k])=='c' && lc(raw[k+1])=='h' &&
                lc(raw[k+2])=='u' && lc(raw[k+3])=='n' && lc(raw[k+4])=='k' &&
                lc(raw[k+5])=='e' && lc(raw[k+6])=='d')
                return 1;
        return 0;
    }
    return 0;
}

int http_dechunk(char *body, int len, unsigned cap) {
    int in = 0, out = 0;
    while (in < len) {
        unsigned sz = 0; int sawdigit = 0;            /* parse the hex size (unsigned) */
        while (in < len && body[in] != '\r' && body[in] != '\n' && body[in] != ';') {
            char c = body[in]; int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;                               /* not a hex digit: stop */
            if (sz > cap) sz = cap;                   /* cap so sz*16 can't overflow */
            else sz = sz * 16u + (unsigned)d;
            sawdigit = 1; in++;
        }
        if (!sawdigit) break;                         /* malformed framing */
        while (in < len && body[in] != '\n') in++;    /* skip rest of size line */
        if (in < len) in++;                           /* step past the '\n' (in <= len) */
        if (sz == 0) break;                           /* final (0-size) chunk */
        unsigned room = (unsigned)(len - in);         /* headroom; never form in+sz (overflow) */
        if (sz > room) sz = room;                     /* truncated: take what we got */
        if (sz == 0) break;
        memmove(body + out, body + in, (size_t)sz);   /* compact (out <= in) */
        out += (int)sz; in += (int)sz;
        while (in < len && (body[in] == '\r' || body[in] == '\n')) in++;  /* trailing CRLF */
    }
    return out;
}

int http_find_loc(const char *raw, int n, char *out, int max) {
    for (int i = 0; i + 9 < n; i++) {
        if (i == 0 || raw[i-1] == '\n') {
            const char *h = "location:";
            int j = 0;
            while (h[j] && lc(raw[i+j]) == h[j]) j++;
            if (!h[j]) {
                int k = i + 9;
                while (k < n && (raw[k] == ' ' || raw[k] == '\t')) k++;
                int o = 0;
                while (k < n && raw[k] != '\r' && raw[k] != '\n' && o < max - 1) out[o++] = raw[k++];
                out[o] = 0;
                return 1;
            }
        }
        if (i + 3 < n && raw[i]=='\r' && raw[i+1]=='\n' && raw[i+2]=='\r' && raw[i+3]=='\n') break;
    }
    return 0;
}
