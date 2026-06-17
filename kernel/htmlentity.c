/*
 * htmlentity.c — HTML character-reference decoding (extracted from browser.c so
 * it can be host-fuzzed; see tests/htmlentfuzz).
 *
 * Reads raw bytes from untrusted page HTML, so every scan is bounded by the
 * caller-supplied length and numeric references are clamped to a valid code
 * point. Pure: no allocation, no device access.
 */
#include "htmlentity.h"

static int ent_is(const char *s, int len, const char *lit) {
    for (int i = 0; i < len; i++) if (!lit[i] || s[i] != lit[i]) return 0;
    return lit[len] == 0;
}

char uni_to_ascii(unsigned v) {
    if (v >= 32 && v < 127) return (char)v;
    switch (v) {
        case 0x2018: case 0x2019: case 0x201A: return '\'';   /* ' ' ‚ */
        case 0x201C: case 0x201D: case 0x201E: return '"';    /* " " „ */
        case 0x2013: case 0x2014: return '-';                 /* en/em dash */
        case 0x2026: return '.';                              /* … ellipsis */
        case 0x2022: case 0x00B7: return '*';                 /* • · */
        case 0x00A0: return ' ';                              /* nbsp */
        case 0x00A9: return 'c'; case 0x00AE: return 'r';     /* © ® (approx) */
        case 0x2122: return 't';                              /* ™ (approx) */
        case 0x00D7: return 'x'; case 0x00F7: return '/';     /* × ÷ */
        case 0x00AB: case 0x00BB: return '"';                 /* « » */
        case 0x00B0: return 'o';                              /* ° degree (approx) */
        case 0x20AC: return 'E';                              /* € (approx) */
    }
    /* Fold Latin-1 Supplement accented letters to their base ASCII letter so
     * numeric refs like &#233; (é) render as 'e' instead of a blank. */
    switch (v) {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: return 'A';
        case 0xC6: return 'A';                                /* Æ ~ A */
        case 0xC7: return 'C';
        case 0xC8: case 0xC9: case 0xCA: case 0xCB: return 'E';
        case 0xCC: case 0xCD: case 0xCE: case 0xCF: return 'I';
        case 0xD0: return 'D'; case 0xD1: return 'N';
        case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8: return 'O';
        case 0xD9: case 0xDA: case 0xDB: case 0xDC: return 'U';
        case 0xDD: return 'Y'; case 0xDF: return 's';         /* ß ~ s */
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: return 'a';
        case 0xE6: return 'a';                                /* æ ~ a */
        case 0xE7: return 'c';
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: return 'e';
        case 0xEC: case 0xED: case 0xEE: case 0xEF: return 'i';
        case 0xF0: return 'd'; case 0xF1: return 'n';
        case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: return 'o';
        case 0xF9: case 0xFA: case 0xFB: case 0xFC: return 'u';
        case 0xFD: case 0xFF: return 'y';
        default: return ' ';
    }
}

int decode_utf8(const char *s, int maxlen, unsigned *cp) {
    if (maxlen < 1) { *cp = 0; return 0; }                /* defensive: read s[0] only after this */
    unsigned char c0 = (unsigned char)s[0];
    int need; unsigned v;
    if      (c0 < 0xC0) { *cp = c0; return 1; }            /* ASCII or stray continuation */
    else if (c0 < 0xE0) { need = 1; v = c0 & 0x1F; }
    else if (c0 < 0xF0) { need = 2; v = c0 & 0x0F; }
    else if (c0 < 0xF8) { need = 3; v = c0 & 0x07; }
    else                { *cp = c0; return 1; }
    if (1 + need > maxlen) { *cp = c0; return 1; }         /* truncated */
    for (int k = 1; k <= need; k++) {
        unsigned char ck = (unsigned char)s[k];
        if ((ck & 0xC0) != 0x80) { *cp = c0; return 1; }   /* bad continuation */
        v = (v << 6) | (ck & 0x3F);
    }
    *cp = v;
    return 1 + need;
}

int decode_entity(const char *s, int maxlen, char *out) {
    if (maxlen < 2) return 0;   /* shortest decodable entity is >=2 chars; also makes the s[1] read below in-bounds regardless of caller (defense-in-depth: callers already pass s[0]=='&') */
    int n = 0; while (n < maxlen && n < 12 && s[n] != ';') n++;
    if (n >= maxlen || s[n] != ';') return 0;
    int len = n + 1;
    if (s[1] == '#') {                                   /* numeric: decimal or &#xHH; hex */
        unsigned v = 0; int i = 2;
        if (i < n && (s[i] == 'x' || s[i] == 'X')) {
            if (++i >= n) return 0;
            for (; i < n; i++) {
                char c = s[i]; int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else return 0;
                v = v * 16 + (unsigned)d; if (v > 0x10FFFF) v = 0x10FFFF;
            }
        } else {
            if (i >= n) return 0;
            for (; i < n; i++) { if (s[i] < '0' || s[i] > '9') return 0; v = v*10 + (unsigned)(s[i]-'0'); if (v > 0x10FFFF) v = 0x10FFFF; }
        }
        *out = uni_to_ascii(v);
        return len;
    }
    if (ent_is(s, len, "&amp;")  ) { *out = '&';  return len; }
    if (ent_is(s, len, "&lt;")   ) { *out = '<';  return len; }
    if (ent_is(s, len, "&gt;")   ) { *out = '>';  return len; }
    if (ent_is(s, len, "&quot;") ) { *out = '"';  return len; }
    if (ent_is(s, len, "&apos;") || ent_is(s, len, "&rsquo;") || ent_is(s, len, "&lsquo;")) { *out = '\''; return len; }
    if (ent_is(s, len, "&rdquo;") || ent_is(s, len, "&ldquo;")) { *out = '"';  return len; }
    if (ent_is(s, len, "&nbsp;") ) { *out = ' ';  return len; }
    if (ent_is(s, len, "&mdash;") || ent_is(s, len, "&ndash;")) { *out = '-';  return len; }
    if (ent_is(s, len, "&hellip;")) { *out = '.';  return len; }
    if (ent_is(s, len, "&middot;") || ent_is(s, len, "&bull;")) { *out = '*'; return len; }
    if (ent_is(s, len, "&copy;")  ) { *out = 'c';  return len; }
    if (ent_is(s, len, "&reg;")   ) { *out = 'r';  return len; }
    if (ent_is(s, len, "&trade;") ) { *out = 't';  return len; }
    if (ent_is(s, len, "&times;") ) { *out = 'x';  return len; }
    if (ent_is(s, len, "&divide;")) { *out = '/';  return len; }
    if (ent_is(s, len, "&deg;")   ) { *out = 'o';  return len; }
    if (ent_is(s, len, "&laquo;") || ent_is(s, len, "&raquo;")) { *out = '"'; return len; }
    if (ent_is(s, len, "&euro;")  ) { *out = 'E';  return len; }
    if (ent_is(s, len, "&rarr;") || ent_is(s, len, "&rArr;")) { *out = '>'; return len; }   /* arrows -> nearest ASCII */
    if (ent_is(s, len, "&larr;") || ent_is(s, len, "&lArr;")) { *out = '<'; return len; }
    if (ent_is(s, len, "&uarr;") ) { *out = '^'; return len; }
    if (ent_is(s, len, "&darr;") ) { *out = 'v'; return len; }
    if (ent_is(s, len, "&harr;") || ent_is(s, len, "&minus;")) { *out = '-'; return len; }
    if (ent_is(s, len, "&plusmn;")) { *out = '+'; return len; }
    if (ent_is(s, len, "&prime;") ) { *out = '\''; return len; }
    if (ent_is(s, len, "&Prime;") ) { *out = '"'; return len; }
    if (ent_is(s, len, "&sect;")  ) { *out = 'S'; return len; }
    if (ent_is(s, len, "&para;")  ) { *out = 'P'; return len; }
    if (ent_is(s, len, "&dagger;") || ent_is(s, len, "&Dagger;")) { *out = '+'; return len; }
    if (ent_is(s, len, "&cent;")  ) { *out = 'c'; return len; }
    if (ent_is(s, len, "&pound;") ) { *out = 'L'; return len; }
    if (ent_is(s, len, "&yen;")   ) { *out = 'Y'; return len; }
    if (ent_is(s, len, "&micro;") ) { *out = 'u'; return len; }
    if (ent_is(s, len, "&frasl;") ) { *out = '/'; return len; }
    if (ent_is(s, len, "&lsaquo;")) { *out = '<'; return len; }
    if (ent_is(s, len, "&rsaquo;")) { *out = '>'; return len; }
    if (ent_is(s, len, "&sbquo;") ) { *out = ','; return len; }
    if (ent_is(s, len, "&bdquo;") ) { *out = '"'; return len; }
    return 0;
}
