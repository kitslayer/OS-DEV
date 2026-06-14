/*
 * browser.c — a tiny graphical web browser.
 *
 * The pipeline, end to end:
 *   1. parse the address bar URL into host + path
 *   2. fetch it with http_get() (real TCP/HTTP over the internet)
 *   3. strip the HTML into a flat list of "tokens": words (each tagged with a
 *      style — normal / heading / link, and a link id if it sits in an <a>)
 *      and breaks (line / paragraph)
 *   4. render that token stream word-wrapped into the window, recording the
 *      on-screen rectangle of every link word so clicks can follow it
 *
 * It is deliberately NOT a real layout engine: no CSS, no tables, no images.
 * But it turns live HTML from the internet into readable, styled, scrollable,
 * clickable pages — the first real step toward a browser.
 */
#include "browser.h"
#include "net.h"
#include "fb.h"
#include "png.h"
#include "gif.h"
#include "jpeg.h"
#include "tls.h"
#include "js.h"
#include "console.h"
#include "kheap.h"
#include "string.h"
#include "task.h"
#include "timer.h"
#include "vfs.h"
#include <stdint.h>
#include <stddef.h>

#define RAW_MAX   262144        /* response/image fetch buffer (256 KB) */
#define TEXT_MAX  49152         /* token text pool (< 65536: token off is uint16) */
#define TOK_MAX   7000
#define SCRIPT_MAX 16384        /* concatenated inline <script> text run per page */
#define HREF_MAX  8192
#define LINK_MAX  512
#define LREC_MAX  1024
#define URL_MAX   160
#define ADDR_H    30
#define GW        8                 /* glyph width (8x16 font) */
#define NO_LINK   0xFFFF
#define IMG_SLOTS 6                 /* inline images decoded per page */
#define IMG_READ_MAX 131072         /* scratch to read a local image file */
#define IMG_MAX_H 360               /* cap an inline image's on-screen height */

enum { STY_NORMAL, STY_H1, STY_H2, STY_LINK, STY_BOLD, STY_EM, STY_CODE, STY_STRIKE };
enum { TK_WORD, TK_BREAK, TK_PARA, TK_HR, TK_IMG };   /* TK_IMG: link field = image slot */

typedef struct { uint16_t off, len, link; uint8_t style, type; } tok_t;
typedef struct { uint16_t off, len; } href_t;            /* slice into hrefs[] */
typedef struct { int16_t x, y, w, h; uint16_t link; } lrec_t;  /* a clickable rect */

struct browser {
    char    url[URL_MAX];
    int     editing;
    int     edit_fresh;                                  /* just entered the address bar: first keystroke replaces the URL */
    int     viewsource;                                  /* 'u': show raw HTML instead of rendering */
    char   *raw;  int rawlen;
    char   *text; int textlen;
    tok_t  *toks; int ntok;
    char   *hrefs; int hreflen;
    href_t *links; int nlink;
    lrec_t *lrec;  int nlrec;                            /* rebuilt each render */
    int     sel;                                         /* keyboard-selected link id (NO_LINK = none) */
    int     linky[LINK_MAX];                             /* content-space y of each link (for scroll-into-view) */
    int     scroll, content_h, view_h;
    char    status[40];
    char    title[64];                                   /* <title> -> window bar */
    volatile int loading;                                /* fetch in flight     */
    volatile int need_parse;                             /* worker -> WM: parse  */
    volatile int closed;                                 /* window closed mid-fetch */
    volatile int http_n;                                 /* http_get result      */
    int     cert_status;                                 /* TLS CertVerify: -2 n/a, 0 ok, -1 fail */
    int     chain_ok;                                    /* 1 = chain anchored to a trusted root */
    volatile int want;                                   /* load queued (worker busy) */
    char    cur[URL_MAX];                                /* currently shown URL */
    char    hist[16][URL_MAX]; int histn;                /* back stack          */
    int     is_back;                                     /* this nav is a Back  */
    int     redirects;                                   /* HTTP 3xx hop count  */
    int     listdepth;                                   /* nested <ul>/<ol> depth */
    char    listtype[8];                                 /* 'u' or 'o' per level */
    int     listnum[8];                                  /* <ol> item counter per level */
    int     tdcount;                                     /* cells emitted in the current <tr> */
    int     finding;                                     /* in-page find: typing a query */
    char    findq[40];                                   /* the find query */
    int     find_tok;                                    /* highlighted match token (-1 none) */
    int     toky[TOK_MAX];                               /* content-space y of every token (scroll-to) */
    char    anc_id[32][32]; uint16_t anc_tok[32]; int anc_n;   /* id -> token index, for #fragment scroll-to */
    uint8_t *img; int imgw, imgh;                        /* current full-page frame (RGBA), or NULL */
    uint8_t *framebuf; int nframes, curframe;            /* animated GIF: all frames + current */
    int      framedelay[64];                             /* per-frame delay (centiseconds) */
    uint64_t frametick;                                  /* tick at which curframe was shown */
    uint8_t *imgs[IMG_SLOTS]; int imgsw[IMG_SLOTS], imgsh[IMG_SLOTS]; int nimg;  /* inline images */
    uint32_t curcolor;                                   /* <font color> in effect (0=none, else 0x01000000|rgb) */
    uint32_t tokcolor[TOK_MAX];                          /* per-token colour override */
    char    *scripts; int scriptlen;                     /* inline <script> text captured this parse */
    int     bodyoff, bodylen;                            /* current page's body region in raw (for click-time JS re-render) */
    char    ls_keys[16][32]; char ls_vals[16][160]; int ls_n;   /* per-page localStorage (survives per-run JS arena resets) */
    char    oc_tag[16]; int oc_depth, oc_link, oc_style;        /* active inline-onclick scope (0 depth = none) */
    char    in_id[8][32]; char in_val[8][96]; int in_n;         /* <input> field values, by id (the typed/scripted text) */
    char    in_name[8][32];                                     /* each field's name= attr (parallel to in_id), for GET submit */
    char    focus_id[32];                                       /* id of the focused input field (empty = none) */
    char    form_action[URL_MAX];                               /* current <form action>; empty = submit to the current page */
};

static void drop_image(browser_t *b);        /* fwd: free any decoded image */
static void drop_image_slots(browser_t *b);  /* fwd: free inline images */
static int  decode_local_to_slot(browser_t *b, const char *path);  /* fwd */

/* ---- small helpers ---- */
static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int tageq(const char *t, const char *lit) {
    int i = 0; while (t[i] && lit[i]) { if (t[i] != lit[i]) return 0; i++; }
    return t[i] == lit[i];
}
static int startsw(const char *s, const char *p) {
    while (*p) { if (*s != *p) return 0; s++; p++; } return 1;
}
static int streqs(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; } return *a == *b;
}
static void copy_url(char *dst, const char *src) {
    int i = 0; while (src[i] && i < URL_MAX - 1) { dst[i] = src[i]; i++; } dst[i] = 0;
}
static uint32_t color_for(int style) {
    switch (style) {
    case STY_H1:   return 0x10306E;
    case STY_H2:   return 0x243B70;
    case STY_LINK: return 0x1560C0;
    case STY_BOLD: return 0x101015;
    case STY_EM:   return 0x6A4A1A;
    case STY_CODE: return 0xA83254;          /* inline <code>/<tt>: crimson */
    case STY_STRIKE: return 0x808890;        /* <s>/<del>: muted grey, drawn with a strike-line */
    default:       return 0x202024;
    }
}
static int lineh_for(int style) { return style == STY_H1 ? 34 : style == STY_H2 ? 24 : 18; }
static int scale_for(int style) { return style == STY_H1 ? 2 : 1; }

/* ---- token emission ---- */
static void emit_word(browser_t *b, int start, int style, int link) {
    int len = b->textlen - start;
    if (len <= 0 || b->ntok >= TOK_MAX) return;
    b->tokcolor[b->ntok] = b->curcolor;                  /* <font color> override (0 = none) */
    b->toks[b->ntok++] = (tok_t){ (uint16_t)start, (uint16_t)len,
                                  (uint16_t)link, (uint8_t)style, TK_WORD };
}
static void emit_break(browser_t *b, int type) {
    if (b->ntok == 0) return;                            /* no leading blank lines */
    tok_t *last = &b->toks[b->ntok - 1];
    if (last->type == TK_WORD) {
        if (b->ntok >= TOK_MAX) return;
        b->toks[b->ntok++] = (tok_t){ 0, 0, NO_LINK, STY_NORMAL, (uint8_t)type };
    } else if (last->type == TK_BREAK && type == TK_PARA) {
        last->type = TK_PARA;                            /* upgrade, don't stack */
    }
}
/* Emit a literal word (e.g. a list bullet) by appending it to the text pool. */
static void emit_literal(browser_t *b, const char *s, int style) {
    int start = b->textlen;
    for (int i = 0; s[i] && b->textlen < TEXT_MAX - 1; i++) b->text[b->textlen++] = s[i];
    emit_word(b, start, style, NO_LINK);
}
/* Emit a literal word as a clickable/selectable link (used for <input> fields). */
static void emit_literal_link(browser_t *b, const char *s, int link) {
    int start = b->textlen;
    for (int i = 0; s[i] && b->textlen < TEXT_MAX - 1; i++) b->text[b->textlen++] = s[i];
    emit_word(b, start, STY_LINK, link);
}
static const char *in_get(browser_t *b, const char *id);   /* fwd: <input> field value lookup */
static void in_set(browser_t *b, const char *id, const char *val);          /* fwd */
static void in_name_set(browser_t *b, const char *id, const char *name);    /* fwd */

/* Does `lit` exactly equal the first `len` chars of `s`? (tageq requires full
 * equality of both strings, which is wrong here because `s` continues into the
 * rest of the page past the entity — so use this length-bounded match instead.) */
static int ent_is(const char *s, int len, const char *lit) {
    for (int i = 0; i < len; i++) if (!lit[i] || s[i] != lit[i]) return 0;
    return lit[len] == 0;
}

/* Map a Unicode codepoint to a single ASCII char (we have no Unicode font):
 * common typographic punctuation folds to its ASCII lookalike, else a space. */
static char uni_to_ascii(unsigned v) {
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

/* Decode &entity; at s (s[0]=='&'); writes *out + returns chars consumed, or 0. */
static int decode_entity(const char *s, int maxlen, char *out) {
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
    if (ent_is(s, len, "&dagger;")) { *out = '+'; return len; }
    return 0;
}

/* Copy up to `dstmax` chars of src[0..srclen) into dst, decoding HTML entities as
 * it goes. Returns chars written. Used for attribute text (e.g. img alt) which the
 * main text loop — the only other place entities are decoded — never sees. */
static int copy_decoded(char *dst, int dstmax, const char *src, int srclen) {
    int p = 0;
    for (int i = 0; i < srclen && p < dstmax; i++) {
        char c = src[i];
        if (c == '&') {
            char dec; int adv = decode_entity(src + i, srclen - i, &dec);
            if (adv) { c = dec; i += adv - 1; }
        }
        dst[p++] = c;
    }
    return p;
}

/* Decode one UTF-8 sequence at s[0] (s[0] >= 0x80). Writes the codepoint to *cp
 * and returns bytes consumed (1-4). Real pages emit UTF-8 directly (smart quotes,
 * dashes, accents) rather than entities, so the text loop folds these to ASCII via
 * uni_to_ascii. Malformed/overlong sequences fall back to a 1-byte Latin-1 read. */
static int decode_utf8(const char *s, int maxlen, unsigned *cp) {
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

/* Find attribute `name`="..." (or '...' or bare) within an attribute slice.
 * The name must start at a token boundary (so "href" won't match inside another
 * attribute or value). */
static int find_attr(const char *a, int n, const char *name, const char **val, int *vlen) {
    int nl = 0; while (name[nl]) nl++;
    for (int i = 0; i + nl <= n; i++) {
        if (i > 0 && a[i-1] != ' ' && a[i-1] != '\t') continue;  /* attr boundary */
        int m = 0; while (m < nl && lc(a[i+m]) == name[m]) m++;
        if (m != nl) continue;
        int k = i + nl; while (k < n && (a[k]==' '||a[k]=='\t')) k++;
        if (k < n && a[k] == '=') {
            k++; while (k < n && (a[k]==' '||a[k]=='\t')) k++;
            char q = 0; if (k < n && (a[k]=='"'||a[k]=='\'')) { q = a[k]; k++; }
            int s = k;
            while (k < n && (q ? a[k]!=q : (a[k]!=' '&&a[k]!='\t'&&a[k]!='>'))) k++;
            *val = a + s; *vlen = k - s; return 1;
        }
    }
    return 0;
}
/* Is a (possibly valueless) boolean attribute present? e.g. `checked`, `disabled`. */
static int has_attr(const char *a, int n, const char *name) {
    int nl = 0; while (name[nl]) nl++;
    for (int i = 0; i + nl <= n; i++) {
        if (i > 0 && a[i-1] != ' ' && a[i-1] != '\t') continue;       /* attr boundary */
        int m = 0; while (m < nl && lc(a[i+m]) == name[m]) m++;
        if (m != nl) continue;
        int k = i + nl;                                               /* must be a complete token */
        if (k >= n || a[k]==' ' || a[k]=='\t' || a[k]=='=' || a[k]=='>' || a[k]=='/') return 1;
    }
    return 0;
}
/* Parse a numeric attribute (e.g. width="48"); 0 if absent/non-numeric. */
static int attr_int(const char *a, int n, const char *name) {
    const char *v; int vl;
    if (!find_attr(a, n, name, &v, &vl)) return 0;
    int x = 0, i = 0;
    while (i < vl && v[i] >= '0' && v[i] <= '9') { x = x * 10 + (v[i] - '0'); if (x > 8192) return 8192; i++; }
    return x;
}
static int find_href(const char *a, int n, const char **val, int *vlen) {
    return find_attr(a, n, "href", val, vlen);
}

/* Store an href slice, returning its link id (or NO_LINK if full/empty). */
static int add_href(browser_t *b, const char *v, int vlen) {
    if (vlen <= 0 || b->nlink >= LINK_MAX || b->hreflen + vlen >= HREF_MAX) return NO_LINK;
    int off = b->hreflen;
    for (int i = 0; i < vlen; i++) b->hrefs[b->hreflen++] = v[i];
    b->links[b->nlink] = (href_t){ (uint16_t)off, (uint16_t)vlen };
    return b->nlink++;
}
/* Store an "input:ID" link so following it focuses that field for typing. */
static int add_input_link(browser_t *b, const char *id) {
    const char *pfx = "input:"; int pl = 6; int il = 0; while (id[il]) il++;
    if (il <= 0 || b->nlink >= LINK_MAX || b->hreflen + pl + il >= HREF_MAX) return NO_LINK;
    int off = b->hreflen;
    for (int i = 0; i < pl; i++) b->hrefs[b->hreflen++] = pfx[i];
    for (int i = 0; i < il; i++) b->hrefs[b->hreflen++] = id[i];
    b->links[b->nlink] = (href_t){ (uint16_t)off, (uint16_t)(pl + il) };
    return b->nlink++;
}
/* Store a "check:ID" link so following it toggles that checkbox/radio. */
static int add_check_link(browser_t *b, const char *id) {
    const char *pfx = "check:"; int pl = 6; int il = 0; while (id[il]) il++;
    if (il <= 0 || b->nlink >= LINK_MAX || b->hreflen + pl + il >= HREF_MAX) return NO_LINK;
    int off = b->hreflen;
    for (int i = 0; i < pl; i++) b->hrefs[b->hreflen++] = pfx[i];
    for (int i = 0; i < il; i++) b->hrefs[b->hreflen++] = id[i];
    b->links[b->nlink] = (href_t){ (uint16_t)off, (uint16_t)(pl + il) };
    return b->nlink++;
}
/* Store an onclick handler as a "javascript:CODE" link so the existing click path
 * (goto_href -> run_js_handler) runs the code; returns the link id. */
static int add_onclick(browser_t *b, const char *code, int codelen) {
    const char *pfx = "javascript:"; int pl = 11;
    if (codelen <= 0 || b->nlink >= LINK_MAX || b->hreflen + pl + codelen >= HREF_MAX) return NO_LINK;
    int off = b->hreflen;
    for (int i = 0; i < pl; i++) b->hrefs[b->hreflen++] = pfx[i];
    for (int i = 0; i < codelen; i++) b->hrefs[b->hreflen++] = code[i];
    b->links[b->nlink] = (href_t){ (uint16_t)off, (uint16_t)(pl + codelen) };
    return b->nlink++;
}
/* Store a "submit:ACTION" link; following it (browser_follow) builds a GET query
 * from the field store and navigates. The action is snapshotted at render time
 * because the parse-time form_action is cleared by the </form> close tag. */
static int add_submit_link(browser_t *b, const char *action) {
    const char *pfx = "submit:"; int pl = 7; int al = 0; while (action[al]) al++;
    if (b->nlink >= LINK_MAX || b->hreflen + pl + al >= HREF_MAX) return NO_LINK;
    int off = b->hreflen;
    for (int i = 0; i < pl; i++) b->hrefs[b->hreflen++] = pfx[i];
    for (int i = 0; i < al; i++) b->hrefs[b->hreflen++] = action[i];
    b->links[b->nlink] = (href_t){ (uint16_t)off, (uint16_t)(pl + al) };
    return b->nlink++;
}
/* Percent-encode s into out (cap = bytes available incl. NUL slot); returns bytes
 * written (no NUL). Unreserved chars pass through, space -> '+', else %XX. */
static int url_encode(char *out, int cap, const char *s) {
    int o = 0;
    for (int i = 0; s[i] && o < cap - 3; i++) {          /* -3: room for a worst-case %XX */
        char c = s[i];
        if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') out[o++] = c;
        else if (c == ' ') out[o++] = '+';
        else { const char *h = "0123456789ABCDEF"; out[o++]='%'; out[o++]=h[(c>>4)&0xF]; out[o++]=h[c&0xF]; }
    }
    return o;
}
/* Case-insensitive compare of an attribute value (v,vl) against a literal. */
static int attr_eq(const char *v, int vl, const char *lit) {
    int i = 0; for (; i < vl; i++) { if (!lit[i] || lc(v[i]) != lc(lit[i])) return 0; } return lit[i] == 0;
}

static int hexd(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lc(c); if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
/* Parse an HTML colour (named or #hex) -> 0x01000000|RGB, or 0 if unknown. */
static uint32_t parse_color(const char *v, int vl) {
    if (vl <= 0) return 0;
    if (v[0] == '#') {
        int d[6], n = 0;
        for (int i = 1; i < vl && n < 6; i++) { int hx = hexd(v[i]); if (hx < 0) break; d[n++] = hx; }
        uint32_t rgb;
        if (n >= 6)      rgb = ((uint32_t)(d[0]*16+d[1])<<16) | ((d[2]*16+d[3])<<8) | (d[4]*16+d[5]);
        else if (n >= 3) rgb = ((uint32_t)(d[0]*17)<<16) | ((d[1]*17)<<8) | (d[2]*17);
        else return 0;
        return 0x01000000u | rgb;
    }
    static const struct { const char *n; uint32_t rgb; } named[] = {
        {"red",0xCC0000},{"green",0x008000},{"blue",0x0000CC},{"black",0x000000},
        {"gray",0x808080},{"grey",0x808080},{"silver",0xC0C0C0},{"orange",0xE07000},
        {"yellow",0xB8A000},{"purple",0x800080},{"navy",0x000080},{"teal",0x008080},
        {"maroon",0x800000},{"olive",0x808000},{"lime",0x00A000},{"cyan",0x008B8B},
        {"magenta",0xB000B0},{"brown",0x8B4513},{"pink",0xD06080},
    };
    char buf[16]; int p = 0;
    for (int i = 0; i < vl && p < 15; i++) buf[p++] = (char)lc(v[i]);
    buf[p] = 0;
    for (unsigned i = 0; i < sizeof(named)/sizeof(named[0]); i++)
        if (streqs(buf, named[i].n)) return 0x01000000u | named[i].rgb;
    return 0;
}

/* void (self-closing) elements have no close tag, so they can't open an onclick scope */
static int is_void_tag(const char *t) {
    return tageq(t,"input")||tageq(t,"img")||tageq(t,"br")||tageq(t,"hr")||tageq(t,"meta")||
           tageq(t,"link")||tageq(t,"area")||tageq(t,"col")||tageq(t,"base")||tageq(t,"wbr")||
           tageq(t,"embed")||tageq(t,"source");
}
static void handle_tag(browser_t *b, const char *tag, int closing,
                       const char *attrs, int attrlen,
                       int *style, int *linkdepth, int *curlink) {
    /* inline onclick="CODE": make the element's content a clickable javascript: link,
     * scoped to the element (depth-counted so nested same-name tags don't end it early). */
    if (b->oc_depth > 0 && tageq(tag, b->oc_tag)) {
        if (closing) { if (--b->oc_depth == 0) { *curlink = b->oc_link; if (*style == STY_LINK) *style = b->oc_style; } }
        else b->oc_depth++;
    }
    if (!closing && b->oc_depth == 0 && !is_void_tag(tag)) {
        const char *oc; int ocl;
        int lk = NO_LINK;
        if (find_attr(attrs, attrlen, "onclick", &oc, &ocl)) {
            lk = add_onclick(b, oc, ocl);              /* inline handler: scope the element to a javascript: link */
        } else if (tageq(tag, "button")) {             /* a <button> with no handler submits the form (HTML default), unless type=button/reset */
            const char *tp; int tpl;
            int suppress = find_attr(attrs, attrlen, "type", &tp, &tpl) && (attr_eq(tp, tpl, "button") || attr_eq(tp, tpl, "reset"));
            if (!suppress) lk = add_submit_link(b, b->form_action);
        }
        if (lk != NO_LINK) {                           /* open a click scope (depth-counted) over the element's content */
            b->oc_link = *curlink; b->oc_style = *style;
            *curlink = lk; if (*style == STY_NORMAL) *style = STY_LINK;
            int i = 0; while (tag[i] && i < 15) { b->oc_tag[i] = tag[i]; i++; } b->oc_tag[i] = 0;
            b->oc_depth = 1;
        }
    }
    if (!closing && b->anc_n < 32) {                     /* record id -> token index for #fragment scroll-to */
        const char *idv; int idl;
        if (find_attr(attrs, attrlen, "id", &idv, &idl) && idl > 0 && idl < 32) {
            int k = 0; for (; k < idl; k++) b->anc_id[b->anc_n][k] = idv[k]; b->anc_id[b->anc_n][k] = 0;
            b->anc_tok[b->anc_n] = (uint16_t)(b->ntok < TOK_MAX ? b->ntok : TOK_MAX - 1);
            b->anc_n++;
        }
    }
    if (tageq(tag, "br")) { emit_break(b, TK_BREAK); return; }
    if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == 0) {
        if (!closing) { emit_break(b, TK_PARA); *style = (tag[1] <= '2') ? STY_H1 : STY_H2; }
        else          { *style = STY_NORMAL; emit_break(b, TK_PARA); }
        return;
    }
    if (tageq(tag, "a")) {
        if (!closing) {
            const char *v; int vl;
            *curlink = find_href(attrs, attrlen, &v, &vl) ? add_href(b, v, vl) : NO_LINK;
            if (*style == STY_NORMAL) *style = STY_LINK;
            (*linkdepth)++;
        } else {
            if (*linkdepth > 0) (*linkdepth)--;
            if (*linkdepth == 0) { if (*style == STY_LINK) *style = STY_NORMAL; *curlink = NO_LINK; }
        }
        return;
    }
    if (tageq(tag, "b") || tageq(tag, "strong")) {
        if (!closing) { if (*style == STY_NORMAL) *style = STY_BOLD; }
        else if (*style == STY_BOLD) *style = STY_NORMAL;
        return;
    }
    if (tageq(tag, "i") || tageq(tag, "em")) {
        if (!closing) { if (*style == STY_NORMAL) *style = STY_EM; }
        else if (*style == STY_EM) *style = STY_NORMAL;
        return;
    }
    if (tageq(tag, "code") || tageq(tag, "tt") || tageq(tag, "kbd") || tageq(tag, "samp")) {
        if (!closing) { if (*style == STY_NORMAL) *style = STY_CODE; }
        else if (*style == STY_CODE) *style = STY_NORMAL;
        return;
    }
    if (tageq(tag, "s") || tageq(tag, "del") || tageq(tag, "strike")) {   /* strikethrough */
        if (!closing) { if (*style == STY_NORMAL) *style = STY_STRIKE; }
        else if (*style == STY_STRIKE) *style = STY_NORMAL;
        return;
    }
    if (tageq(tag, "font")) {                            /* <font color="..."> text colour */
        if (!closing) {
            const char *v; int vl;
            if (find_attr(attrs, attrlen, "color", &v, &vl)) { uint32_t c = parse_color(v, vl); if (c) b->curcolor = c; }
        } else b->curcolor = 0;
        return;
    }
    if (tageq(tag, "hr")) { if (b->ntok < TOK_MAX) b->toks[b->ntok++] = (tok_t){0,0,NO_LINK,STY_NORMAL,TK_HR}; return; }
    if (tageq(tag, "input")) {                           /* a form field: shows its value; focusable for typing */
        if (!closing) {
            const char *v; int vl, idl;
            const char *idp;
            char idbuf[32]; idbuf[0] = 0;
            if (find_attr(attrs, attrlen, "id", &idp, &idl) && idl < 32) { int k=0; for(;k<idl;k++) idbuf[k]=idp[k]; idbuf[k]=0; }
            const char *tp; int tpl; int has_type = find_attr(attrs, attrlen, "type", &tp, &tpl);
            int is_submit = has_type && (attr_eq(tp, tpl, "submit") || attr_eq(tp, tpl, "image"));
            int is_hidden = has_type && attr_eq(tp, tpl, "hidden");
            int is_pw = has_type && attr_eq(tp, tpl, "password");
            int is_radio = has_type && attr_eq(tp, tpl, "radio");
            int is_check = is_radio || (has_type && attr_eq(tp, tpl, "checkbox"));
            if (is_check) {                              /* a checkbox/radio -> a [x]/(o) toggle link */
                if (idbuf[0] && !in_get(b, idbuf) && has_attr(attrs, attrlen, "checked")) in_set(b, idbuf, "on");  /* default-checked */
                const char *cur = idbuf[0] ? in_get(b, idbuf) : 0;
                int checked = cur && streqs(cur, "on");
                if (idbuf[0] && find_attr(attrs, attrlen, "name", &v, &vl) && vl > 0) {   /* checked -> submit name=on; unchecked -> omit */
                    if (checked) { char nb[32]; int n=vl; if(n>31)n=31; for(int i=0;i<n;i++) nb[i]=v[i]; nb[n]=0; in_name_set(b, idbuf, nb); }
                    else in_name_set(b, idbuf, "");
                }
                char s[4]; s[0]= is_radio?'(':'['; s[1]= checked?(is_radio?'o':'x'):' '; s[2]= is_radio?')':']'; s[3]=0;
                if (idbuf[0]) { int lk = add_check_link(b, idbuf); if (lk != NO_LINK) emit_literal_link(b, s, lk); else emit_literal(b, s, STY_EM); }
                else emit_literal(b, s, STY_EM);
                return;
            }
            if (is_submit) {                             /* a submit button -> a link that submits the form */
                char s[64]; int p = 0; s[p++] = '[';
                if (find_attr(attrs, attrlen, "value", &v, &vl) && vl > 0) { for (int i=0;i<vl&&p<60;i++) s[p++]=v[i]; }
                else { const char *d = "Submit"; for (int i=0; d[i] && p<60; i++) s[p++]=d[i]; }
                s[p++] = ']'; s[p] = 0;
                int lk = add_submit_link(b, b->form_action);
                if (lk != NO_LINK) emit_literal_link(b, s, lk); else emit_literal(b, s, STY_EM);
                return;
            }
            /* seed a store slot from the value= attr (only if absent) so the field's
             * default is both shown and submittable; idempotent across re-renders. */
            if (idbuf[0] && !in_get(b, idbuf) && find_attr(attrs, attrlen, "value", &v, &vl) && vl > 0) {
                char vb[96]; int n = vl; if (n > 95) n = 95; for (int i=0;i<n;i++) vb[i]=v[i]; vb[n]=0;
                in_set(b, idbuf, vb);
            }
            if (idbuf[0]) {                              /* attach the name= for GET submit (if the slot exists) */
                const char *nm; int nml;
                if (find_attr(attrs, attrlen, "name", &nm, &nml) && nml > 0) {
                    char nb[32]; int n = nml; if (n > 31) n = 31; for (int i=0;i<n;i++) nb[i]=nm[i]; nb[n]=0;
                    in_name_set(b, idbuf, nb);
                }
            }
            if (is_hidden) return;                       /* hidden fields are submittable but not drawn */
            const char *stored = idbuf[0] ? in_get(b, idbuf) : 0;   /* typed / scripted / seeded .value */
            int focused = idbuf[0] && streqs(b->focus_id, idbuf);
            char s[100]; int p = 0; s[p++] = '[';
            if (stored)                                                                 { for (int i=0; stored[i] && p<94; i++) s[p++]= is_pw ? '*' : stored[i]; }
            else if (find_attr(attrs, attrlen, "value", &v, &vl) && vl > 0)             { for (int i=0; i<vl && p<94; i++) s[p++]= is_pw ? '*' : v[i]; }
            else if (find_attr(attrs, attrlen, "placeholder", &v, &vl) && vl > 0)       { for (int i=0; i<vl && p<94; i++) s[p++]=v[i]; }
            else                                                                        { s[p++]='_'; s[p++]='_'; s[p++]='_'; s[p++]='_'; }
            if (focused) s[p++] = '|';                   /* a cursor on the focused field */
            s[p++] = ']'; s[p] = 0;
            if (idbuf[0]) { int lk = add_input_link(b, idbuf);   /* a field with an id is focusable (Enter to type) */
                if (lk != NO_LINK) emit_literal_link(b, s, lk); else emit_literal(b, s, STY_EM); }
            else emit_literal(b, s, STY_EM);
        }
        return;
    }
    if (tageq(tag, "img")) {                             /* [alt] — a clickable link to the image */
        if (!closing) {
            const char *v; int vl;
            char label[40]; int p = 0; label[p++] = '[';
            if (find_attr(attrs, attrlen, "alt", &v, &vl) && vl > 0)
                p += copy_decoded(label + p, 37 - p, v, vl);
            else { const char *im = "img"; while (*im && p < 37) label[p++] = *im++; }
            label[p++] = ']'; label[p] = 0;
            const char *src; int srcl;
            if (find_attr(attrs, attrlen, "src", &src, &srcl) && srcl > 0) {
                /* a LOCAL image (file:NAME) decodes inline; everything else
                 * (http: etc.) stays a clickable link you can follow to view */
                int shown = 0;
                if (srcl > 5 && src[0]=='f'&&src[1]=='i'&&src[2]=='l'&&src[3]=='e'&&src[4]==':') {
                    char path[96]; int pn = 0;
                    for (int i = 5; i < srcl && pn < 95; i++) path[pn++] = src[i];
                    path[pn] = 0;
                    int slot = decode_local_to_slot(b, path);
                    if (slot >= 0 && b->ntok + 2 < TOK_MAX) {
                        /* honour explicit width/height (off=w, len=h; 0 = natural) */
                        int aw = attr_int(attrs, attrlen, "width");
                        int ah = attr_int(attrs, attrlen, "height");
                        emit_break(b, TK_BREAK);
                        b->toks[b->ntok++] = (tok_t){ (uint16_t)aw, (uint16_t)ah, (uint16_t)slot, STY_NORMAL, TK_IMG };
                        emit_break(b, TK_BREAK);
                        shown = 1;
                    }
                }
                if (!shown) {
                    int link = add_href(b, src, srcl);       /* follow it to view the image */
                    int start = b->textlen;
                    for (int i = 0; label[i] && b->textlen < TEXT_MAX - 1; i++) b->text[b->textlen++] = label[i];
                    emit_word(b, start, STY_LINK, link);
                }
            } else emit_literal(b, label, STY_EM);
        }
        return;
    }
    if (tageq(tag, "ul") || tageq(tag, "ol")) {          /* track list nesting + kind */
        if (!closing) {
            if (b->listdepth < 8) { b->listtype[b->listdepth] = tag[0]; b->listnum[b->listdepth] = 0; b->listdepth++; }
        } else if (b->listdepth > 0) b->listdepth--;
        emit_break(b, TK_BREAK);
        return;
    }
    if (tageq(tag, "li")) {
        if (!closing) {
            emit_break(b, TK_BREAK);
            /* marker[24]: depth<=8 → 16 indent spaces, digits capped at p<22,
             * then '.' at p<23, then NUL at p<=23 — stays within bounds. */
            char marker[24]; int p = 0;
            int depth = b->listdepth > 0 ? b->listdepth : 1;   /* indent: 2 spaces/level */
            for (int i = 0; i < depth && p < 18; i++) { marker[p++] = ' '; marker[p++] = ' '; }
            if (b->listdepth > 0 && b->listtype[b->listdepth - 1] == 'o') {
                int n = ++b->listnum[b->listdepth - 1];        /* numbered item */
                char num[8]; int k = 0; int t = n; if (!t) num[k++] = '0';
                while (t) { num[k++] = '0' + t % 10; t /= 10; }
                while (k && p < 22) marker[p++] = num[--k];
                if (p < 23) marker[p++] = '.';
            } else if (p < 23) marker[p++] = '-';              /* bulleted item */
            marker[p] = 0;
            emit_literal(b, marker, STY_NORMAL);
        }
        return;
    }
    if (tageq(tag, "dd")) { if (!closing) { emit_break(b, TK_BREAK); emit_literal(b, "  ", STY_NORMAL); } return; }
    if (tageq(tag, "tr")) { if (!closing) { emit_break(b, TK_BREAK); b->tdcount = 0; } return; }
    if (tageq(tag, "td") || tageq(tag, "th")) {          /* table cell: pipe-separated */
        if (!closing) {
            if (b->tdcount++ > 0) emit_literal(b, "|", STY_NORMAL);  /* divider between cells */
            if (tag[0] == 't' && tag[1] == 'h' && *style == STY_NORMAL) *style = STY_BOLD;  /* <th> bold */
        } else if (tag[0] == 't' && tag[1] == 'h' && *style == STY_BOLD) *style = STY_NORMAL;
        return;
    }
    if (tageq(tag, "form")) {                            /* capture the GET action; submit buttons snapshot it */
        if (!closing) {
            const char *v; int vl; b->form_action[0] = 0;
            if (find_attr(attrs, attrlen, "action", &v, &vl) && vl > 0) {
                int n = vl; if (n > URL_MAX - 1) n = URL_MAX - 1;
                for (int i = 0; i < n; i++) b->form_action[i] = v[i];
                b->form_action[n] = 0;
            }
        } else b->form_action[0] = 0;
        emit_break(b, TK_BREAK);
        return;
    }
    if (tageq(tag, "p") || tageq(tag, "dt")) { emit_break(b, TK_PARA); return; }
    if (tageq(tag, "table")) { emit_break(b, TK_PARA); b->tdcount = 0; return; }
    if (tageq(tag, "div") || tageq(tag, "section") ||
        tageq(tag, "article") || tageq(tag, "header") || tageq(tag, "footer") ||
        tageq(tag, "nav") || tageq(tag, "pre") || tageq(tag, "dl") ||
        tageq(tag, "blockquote") || tageq(tag, "main"))
        emit_break(b, TK_BREAK);
}

/* Strip the HTML body into the token stream + href table. */
/* Append the text inside a <script>...</script> to b->scripts (so it can be run
 * after the page is parsed). Multiple scripts are separated by a newline+';'. */
static void capture_script(browser_t *b, const char *s, int len) {
    if (!b->scripts) return;
    if (b->scriptlen > 0 && b->scriptlen < SCRIPT_MAX - 2) { b->scripts[b->scriptlen++] = '\n'; b->scripts[b->scriptlen++] = ';'; }
    for (int k = 0; k < len && b->scriptlen < SCRIPT_MAX - 1; k++) b->scripts[b->scriptlen++] = s[k];
    b->scripts[b->scriptlen] = 0;
}

static void parse_html(browser_t *b, const char *body, int len) {
    drop_image(b);                                       /* a page replaces any prior image */
    drop_image_slots(b);                                 /* and its inline images */
    b->textlen = b->ntok = b->hreflen = b->nlink = 0;
    b->scriptlen = 0;                                    /* recaptured fresh each parse */
    b->oc_depth = 0;                                     /* no inline-onclick scope open yet */
    b->form_action[0] = 0;                               /* no <form> action open yet */
    b->anc_n = 0;                                        /* fresh #fragment anchor table */
    b->sel = NO_LINK;                                    /* no link selected on a fresh page */
    b->find_tok = -1;                                    /* clear any find highlight */
    b->curcolor = 0;                                     /* default text colour */
    b->viewsource = 0;                                   /* show the rendered page, not source */
    b->listdepth = 0;                                    /* reset list nesting */
    b->tdcount = 0;
    memset(b->linky, 0xFF, sizeof(b->linky));            /* -1 = "not laid out this page" */
    b->title[0] = 0;
    int titlelen = 0, titlesp = 0;
    int style = STY_NORMAL, linkdepth = 0, curlink = NO_LINK;
    /* independent suppression flags: a stray </title> must not reveal <head>,
     * and <body> force-clears head/title so a missing </head> can't blank the
     * whole page (fail-safe on malformed input). */
    int inscript = 0, instyle = 0, intitle = 0, inhead = 0, inpre = 0, insvg = 0, wstart = -1;
    int sc_start = -1;                                   /* offset where current <script> body began */

    for (int i = 0; i < len; i++) {
        char c = body[i];
        if (c == '<') {
            /* Inside <script>/<style>, content is raw: a '<' that isn't the matching
             * close tag (e.g. `i < 5`, or `<p>` inside a document.write string) must
             * be treated as content, NOT parsed as a tag — otherwise the (quote-aware)
             * tag scan can run past </script> and the block is never closed/captured. */
            if (inscript || instyle) {
                const char *ct = inscript ? "/script" : "/style";
                int match = (i+1 < len && body[i+1] == '/');
                if (match) for (int z = 0; ct[z]; z++) { if (i+1+z >= len || lc(body[i+1+z]) != ct[z]) { match = 0; break; } }
                if (!match) continue;                 /* '<' is script/style content; skip it */
            }
            if (wstart >= 0) { emit_word(b, wstart, style, curlink); wstart = -1; }
            /* HTML comment <!-- ... -->: skip to the "-->" terminator. A comment
             * may contain '>' (conditional comments, embedded markup), so we can't
             * just scan to the next '>' like an ordinary tag, or its tail leaks. */
            if (i + 3 < len && body[i+1] == '!' && body[i+2] == '-' && body[i+3] == '-') {
                int k = i + 4;
                while (k + 2 < len && !(body[k]=='-' && body[k+1]=='-' && body[k+2]=='>')) k++;
                i = (k + 2 < len) ? k + 2 : len;    /* loop ++ steps past the '>' */
                continue;
            }
            int j = i + 1, closing = 0;
            if (j < len && body[j] == '/') { closing = 1; j++; }
            char tag[12]; int tl = 0;
            while (j < len && tl < 11) {
                char tc = body[j];
                if (tc=='>'||tc==' '||tc=='/'||tc=='\t'||tc=='\n') break;
                tag[tl++] = (char)lc(tc); j++;
            }
            tag[tl] = 0;
            int astart = j;
            { char q = 0;                                 /* attributes -> unquoted '>' (so a quoted */
              while (j < len) { char ac = body[j];        /* '>' e.g. in href="javascript:...'<p>'" is kept) */
                  if (q) { if (ac == q) q = 0; }
                  else if (ac=='"' || ac=='\'') q = ac;
                  else if (ac=='>') break;
                  j++; } }

            if (tageq(tag, "script")) {
                if (!closing) { inscript = 1; sc_start = j + 1; }          /* body starts after '>' */
                else { if (inscript && sc_start >= 0 && i > sc_start) capture_script(b, body + sc_start, i - sc_start);
                       inscript = 0; sc_start = -1; }
            }
            else if (tageq(tag, "style")) instyle = !closing;
            else if (tageq(tag, "svg")) insvg = !closing;        /* inline SVG: skip its guts */
            else if (tageq(tag, "title") && !insvg) intitle = !closing;  /* (svg <title> mustn't hijack) */
            else if (tageq(tag, "head")) inhead = !closing;
            else if (tageq(tag, "body")) inhead = intitle = 0;   /* visible content */
            else if (tageq(tag, "pre")) {                        /* preformatted block */
                inpre = !closing;
                emit_break(b, TK_PARA);                          /* pre starts/ends on its own line */
            }
            else if (!inscript && !instyle && !intitle && !inhead && !insvg)
                handle_tag(b, tag, closing, body + astart, j - astart,
                           &style, &linkdepth, &curlink);

            i = j;                                        /* loop ++ steps past '>' */
            continue;
        }
        if (inscript || instyle || insvg) continue;  /* never render script/style/svg */

        if (c == '&') {
            char dec; int adv = decode_entity(body + i, len - i, &dec);
            if (adv) { c = dec; i += adv - 1; }
        } else if ((unsigned char)c >= 0x80) {    /* fold a raw UTF-8 char to ASCII */
            unsigned cp; int adv = decode_utf8(body + i, len - i, &cp);
            c = uni_to_ascii(cp); i += adv - 1;
        }
        if (intitle) {                            /* capture <title> (even inside <head>) */
            if (c==' '||c=='\t'||c=='\n'||c=='\r') { if (titlelen > 0) titlesp = 1; }
            else { if (titlesp && titlelen < 63) b->title[titlelen++] = ' ';
                   titlesp = 0; if (titlelen < 63) b->title[titlelen++] = c; }
            continue;
        }
        if (inhead) continue;                     /* skip other <head> content */
        if (inpre) {                              /* preformatted: keep spaces + line breaks */
            if (c == '\r') continue;
            if (c == '\n') {
                if (wstart >= 0) { emit_word(b, wstart, style, curlink); wstart = -1; }
                if (b->ntok > 0 && b->ntok < TOK_MAX)            /* preserve blank lines too */
                    b->toks[b->ntok++] = (tok_t){0,0,NO_LINK,STY_NORMAL,TK_BREAK};
                continue;
            }
            if (c == '\t') c = ' ';               /* render tabs as a space */
            if (b->textlen < TEXT_MAX - 1) {       /* accumulate (spaces included) into one word */
                if (wstart < 0) wstart = b->textlen;
                b->text[b->textlen++] = c;
            }
            continue;
        }
        if (c==' '||c=='\t'||c=='\n'||c=='\r') {
            if (wstart >= 0) { emit_word(b, wstart, style, curlink); wstart = -1; }
            continue;
        }
        if (b->textlen < TEXT_MAX - 1) {
            if (wstart < 0) wstart = b->textlen;
            b->text[b->textlen++] = c;
        }
    }
    if (wstart >= 0) emit_word(b, wstart, style, curlink);
    b->title[titlelen] = 0;
}

/* ---- run a page's inline JavaScript ----
 * document.write() output is spliced into b->raw right after the body and the
 * page is re-parsed, so script-generated HTML renders. A real DOM (getElementById,
 * .innerHTML) is future work; this covers the classic document.write pattern. */
static char *g_sw_raw; static int g_sw_pos, g_sw_max;
static int  g_sw_base;   /* the document.write append base (body end at script start); a DOM
                          * mutation that shifts the buffer adjusts this + g_sw_pos together, so
                          * `written = g_sw_pos - g_sw_base` stays correct when both are used. */
static void script_write_cb(const char *s) {
    if (!g_sw_raw) return;
    while (*s && g_sw_pos < g_sw_max - 1) g_sw_raw[g_sw_pos++] = *s++;
    g_sw_raw[g_sw_pos] = 0;
}

/* localStorage backing: a per-page key->value store the JS engine reads/writes
 * (survives the engine's per-run arena reset, so click handlers keep state). */
static browser_t *g_ls_b;
static const char *browser_ls_get(const char *key) {
    if (!g_ls_b) return 0;
    for (int i = 0; i < g_ls_b->ls_n; i++) if (streqs(g_ls_b->ls_keys[i], key)) return g_ls_b->ls_vals[i];
    return 0;
}
static void browser_ls_set(const char *key, const char *val) {
    if (!g_ls_b) return;
    int i; for (i = 0; i < g_ls_b->ls_n; i++) if (streqs(g_ls_b->ls_keys[i], key)) break;
    if (i == g_ls_b->ls_n) { if (g_ls_b->ls_n >= 16) return; int j=0; while(key[j]&&j<31){g_ls_b->ls_keys[i][j]=key[j];j++;} g_ls_b->ls_keys[i][j]=0; g_ls_b->ls_n++; }
    int j=0; while(val[j]&&j<159){g_ls_b->ls_vals[i][j]=val[j];j++;} g_ls_b->ls_vals[i][j]=0;
}
/* ---- minimal DOM: find/read/mutate an element by id in the page source ----
 * Locate <tag … id="ID" …>INNER</tag> in the body region of b->raw and report
 * INNER's byte range [*is, *ie). Handles nested same-name tags by depth count. */
static int dom_alnum(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'); }
static int dom_find(browser_t *b, const char *id, int *is, int *ie) {
    const char *r = b->raw; int lo = b->bodyoff, hi = b->bodyoff + b->bodylen;
    int idlen = 0; while (id[idlen]) idlen++;
    if (idlen == 0) return 0;
    for (int i = lo; i + 4 < hi; i++) {
        if (!(r[i]=='i' && r[i+1]=='d' && r[i+2]=='=' && (r[i+3]=='"' || r[i+3]=='\''))) continue;
        char q = r[i+3]; int vs = i + 4, k = 0;
        while (k < idlen && vs + k < hi && r[vs+k] == id[k]) k++;
        if (!(k == idlen && vs + k < hi && r[vs+k] == q)) continue;   /* whole attr value must equal id */
        int ts = i; while (ts > lo && r[ts] != '<') ts--;             /* back up to the opening '<' */
        if (r[ts] != '<') continue;
        char tag[16]; int tn = 0, ne = ts + 1;
        while (ne < hi && dom_alnum(r[ne]) && tn < 15) tag[tn++] = r[ne++];
        if (tn == 0) continue;
        int gt = i; while (gt < hi && r[gt] != '>') gt++;             /* end of the opening tag */
        if (gt >= hi) return 0;
        int istart = gt + 1, depth = 1, p = istart;
        while (p < hi) {
            if (r[p] == '<') {
                if (p+2+tn <= hi && r[p+1]=='/' && memcmp(r+p+2, tag, tn)==0) { if(--depth==0){ *is=istart; *ie=p; return 1; } }
                else if (p+1+tn < hi && memcmp(r+p+1, tag, tn)==0 && (r[p+1+tn]==' '||r[p+1+tn]=='>'||r[p+1+tn]=='/')) depth++;
            }
            p++;
        }
        return 0;
    }
    return 0;
}
/* <input> field values, keyed by id (the typed or scripted .value text) */
static const char *in_get(browser_t *b, const char *id) {
    for (int i = 0; i < b->in_n; i++) if (streqs(b->in_id[i], id)) return b->in_val[i];
    return 0;
}
static void in_set(browser_t *b, const char *id, const char *val) {
    int i; for (i = 0; i < b->in_n; i++) if (streqs(b->in_id[i], id)) break;
    if (i == b->in_n) { if (b->in_n >= 8 || !id[0]) return; int j=0; while(id[j]&&j<31){b->in_id[i][j]=id[j];j++;} b->in_id[i][j]=0; b->in_name[i][0]=0; b->in_n++; }
    int j=0; while(val[j]&&j<95){b->in_val[i][j]=val[j];j++;} b->in_val[i][j]=0;
}
/* Record a field's name= (for GET submit) in the slot already created for its id. */
static void in_name_set(browser_t *b, const char *id, const char *name) {
    for (int i = 0; i < b->in_n; i++) if (streqs(b->in_id[i], id)) {
        int j=0; while(name[j]&&j<31){b->in_name[i][j]=name[j];j++;} b->in_name[i][j]=0; return;
    }
}
static int browser_dom_get(const char *id, char *out, int max, int html) {
    if (max) out[0] = 0;
    if (!g_ls_b) return 0;
    if (html == 2) {   /* element.value -> the input field's stored text */
        const char *v = in_get(g_ls_b, id); if (!v) return 0;
        int i = 0; while (v[i] && i < max - 1) { out[i] = v[i]; i++; } out[i] = 0; return 1;
    }
    int is, ie; if (!dom_find(g_ls_b, id, &is, &ie)) return 0;
    int len = ie - is; if (len > max - 1) len = max - 1; if (len < 0) len = 0;
    memcpy(out, g_ls_b->raw + is, len); out[len] = 0; return 1;
}
static void browser_dom_set(const char *id, const char *value, int html) {
    browser_t *b = g_ls_b; if (!b) return;
    if (html == 2) { in_set(b, id, value); parse_html(b, b->raw + b->bodyoff, b->bodylen); return; }   /* element.value = … */
    static char esc[8192];
    if (!html) {   /* textContent: HTML-escape so the text isn't interpreted as markup (innerHTML inserts raw) */
        int o = 0;
        for (int i = 0; value[i] && o < (int)sizeof(esc) - 7; i++) {
            char c = value[i];
            if (c=='<')      { memcpy(esc+o, "&lt;", 4); o += 4; }
            else if (c=='>') { memcpy(esc+o, "&gt;", 4); o += 4; }
            else if (c=='&') { memcpy(esc+o, "&amp;", 5); o += 5; }
            else esc[o++] = c;
        }
        esc[o] = 0; value = esc;
    }
    int is, ie; if (!dom_find(b, id, &is, &ie)) return;
    int vlen = 0; while (value[vlen]) vlen++;
    int delta = vlen - (ie - is);
    int active = (g_sw_raw == b->raw);                         /* a script's document.write is appending here */
    int bodyend = b->bodyoff + b->bodylen;
    int live_end = (active && g_sw_pos > bodyend) ? g_sw_pos : bodyend;   /* include any document.write'd bytes */
    if (live_end + delta >= RAW_MAX - 1 || live_end + delta < b->bodyoff) return;   /* out of room */
    memmove(b->raw + ie + delta, b->raw + ie, live_end - ie);  /* shift the tail (incl. document.write appends) */
    memcpy(b->raw + is, value, vlen);                          /* splice in the new content */
    b->bodylen += delta;
    if (active) { if (g_sw_pos > ie) g_sw_pos += delta; if (g_sw_base > ie) g_sw_base += delta; }   /* keep cursors in sync */
    b->raw[live_end + delta] = 0;
    parse_html(b, b->raw + b->bodyoff, b->bodylen);            /* re-render the page in place */
}
/* Locate the attribute span of the element with this id: [*as, *ae) is the text
 * between the tag name and the opening tag's '>'. Mirrors dom_find's tag locate. */
static int dom_attr_region(browser_t *b, const char *id, int *as, int *ae) {
    const char *r = b->raw; int lo = b->bodyoff, hi = b->bodyoff + b->bodylen;
    int idlen = 0; while (id[idlen]) idlen++;
    if (idlen == 0) return 0;
    for (int i = lo; i + 4 < hi; i++) {
        if (!(r[i]=='i' && r[i+1]=='d' && r[i+2]=='=' && (r[i+3]=='"' || r[i+3]=='\''))) continue;
        char q = r[i+3]; int vs = i + 4, k = 0;
        while (k < idlen && vs + k < hi && r[vs+k] == id[k]) k++;
        if (!(k == idlen && vs + k < hi && r[vs+k] == q)) continue;   /* whole attr value must equal id */
        int ts = i; while (ts > lo && r[ts] != '<') ts--;             /* back up to the opening '<' */
        if (r[ts] != '<') continue;
        int ne = ts + 1, tn = 0;
        while (ne < hi && dom_alnum(r[ne]) && tn < 15) { ne++; tn++; }  /* skip the tag name */
        if (tn == 0) continue;
        int gt = i; while (gt < hi && r[gt] != '>') gt++;             /* end of the opening tag */
        if (gt >= hi) return 0;
        *as = ne; *ae = gt; return 1;
    }
    return 0;
}
/* getAttribute(id, attr): read the named attribute off the element's opening tag. */
static int browser_dom_getattr(const char *id, const char *attr, char *out, int max) {
    browser_t *b = g_ls_b; if (!b || max <= 0) return 0; out[0] = 0;
    int as, ae; if (!dom_attr_region(b, id, &as, &ae)) return 0;
    const char *v; int vl;
    if (!find_attr(b->raw + as, ae - as, attr, &v, &vl)) return 0;
    int n = vl; if (n > max - 1) n = max - 1; if (n < 0) n = 0;
    memcpy(out, v, n); out[n] = 0; return 1;
}
/* setAttribute(id, attr, val): replace the attribute's value on the element's
 * opening tag (or insert the attribute if absent), then re-render. The splice
 * mirrors browser_dom_set; the value has quotes stripped so it can't break out. */
static void browser_dom_setattr(const char *id, const char *attr, const char *val) {
    browser_t *b = g_ls_b; if (!b || !attr[0]) return;   /* an empty attribute name is a no-op */
    int as, ae; if (!dom_attr_region(b, id, &as, &ae)) return;
    char vbuf[256]; int vlen = 0;                                 /* sanitized value (no quotes) */
    for (int i = 0; val[i] && vlen < (int)sizeof(vbuf) - 1; i++) { char c = val[i]; if (c != '"' && c != '\'') vbuf[vlen++] = c; }
    vbuf[vlen] = 0;
    const char *fv; int fvl;
    int rs, rend; const char *repl; int rlen; char ins[384];
    if (find_attr(b->raw + as, ae - as, attr, &fv, &fvl)) {        /* attr exists: replace its value (between quotes) */
        rs = (int)(fv - b->raw); rend = rs + fvl; repl = vbuf; rlen = vlen;
    } else {                                                       /* attr absent: insert ` attr="val"` before '>' */
        int p = 0; ins[p++] = ' ';
        for (int i = 0; attr[i] && p < 80; i++) { char c = attr[i]; if (c=='"'||c=='\''||c=='='||c==' '||c=='<'||c=='>'||c=='/') continue; ins[p++] = c; }
        ins[p++] = '='; ins[p++] = '"';
        for (int i = 0; i < vlen && p < 380; i++) ins[p++] = vbuf[i];
        ins[p++] = '"'; ins[p] = 0;
        rs = ae; rend = ae; repl = ins; rlen = p;
    }
    int delta = rlen - (rend - rs);
    int active = (g_sw_raw == b->raw);
    int bodyend = b->bodyoff + b->bodylen;
    int live_end = (active && g_sw_pos > bodyend) ? g_sw_pos : bodyend;
    if (live_end + delta >= RAW_MAX - 1 || live_end + delta < b->bodyoff) return;   /* out of room */
    memmove(b->raw + rend + delta, b->raw + rend, live_end - rend);   /* shift the tail */
    memcpy(b->raw + rs, repl, rlen);                                  /* write the new attr/value */
    b->bodylen += delta;
    if (active) { if (g_sw_pos > rend) g_sw_pos += delta; if (g_sw_base > rend) g_sw_base += delta; }
    b->raw[live_end + delta] = 0;
    parse_html(b, b->raw + b->bodyoff, b->bodylen);
}
static void js_bind_storage(browser_t *b){ g_ls_b=b; js_set_storage(browser_ls_get, browser_ls_set); js_set_dom(browser_dom_get, browser_dom_set); js_set_dom_attr(browser_dom_getattr, browser_dom_setattr); js_set_location(b->url); }
static void run_page_scripts(browser_t *b, int bodyoff, int bodylen) {
    static char jsout[2048];
    int appendpos = bodyoff + bodylen;                   /* splice point in b->raw */
    if (appendpos >= RAW_MAX - 1) return;                /* no room to write */
    g_sw_raw = b->raw; g_sw_pos = appendpos; g_sw_base = appendpos; g_sw_max = RAW_MAX;
    js_bind_storage(b);
    js_run_doc(b->scripts, jsout, sizeof(jsout), script_write_cb);
    int written = g_sw_pos - g_sw_base;                  /* g_sw_base may have shifted if a DOM write moved the buffer */
    g_sw_raw = 0;
    if (jsout[0]) kprintf("[js] %s\n", jsout);           /* console.log / errors -> serial */
    if (written > 0)                                     /* re-render incl. the written HTML */
        parse_html(b, b->raw + bodyoff, bodylen + written);  /* scriptlen reset inside; not re-run */
}

/* Run a `javascript:` link / onclick handler: execute the code, splice any
 * document.write output into the page after the body, and re-render. Reuses the
 * stored body region so the page updates in place on click. */
static void run_js_handler(browser_t *b, const char *code) {
    static char jsout[2048];
    int saved_sel = b->sel;                              /* the clicked link; re-render below clears it */
    int appendpos = b->bodyoff + b->bodylen;
    if (appendpos >= RAW_MAX - 1) return;
    g_sw_raw = b->raw; g_sw_pos = appendpos; g_sw_base = appendpos; g_sw_max = RAW_MAX;
    js_bind_storage(b);
    js_run_doc(code, jsout, sizeof(jsout), script_write_cb);
    int written = g_sw_pos - g_sw_base;                  /* g_sw_base may have shifted if a DOM write moved the buffer */
    g_sw_raw = 0;
    if (jsout[0]) kprintf("[js] %s\n", jsout);
    if (written > 0) { b->bodylen += written; parse_html(b, b->raw + b->bodyoff, b->bodylen); }
    /* a DOM mutation or document.write re-render clears the selection; restore it so
     * pressing Enter again re-runs the same link (e.g. clicking a counter repeatedly). */
    if (saved_sel != NO_LINK && saved_sel < b->nlink) b->sel = saved_sel;
}
/* If the element has the named inline handler (onchange/oninput/…), run it.
 * Returns 1 if it ran (run_js_handler re-renders), so the caller can skip its own. */
static int fire_handler(browser_t *b, const char *id, const char *attr) {
    int as, ae; if (!dom_attr_region(b, id, &as, &ae)) return 0;
    const char *oc; int ocl;
    if (!find_attr(b->raw + as, ae - as, attr, &oc, &ocl) || ocl <= 0) return 0;
    char code[1024]; int n = ocl; if (n > (int)sizeof(code) - 1) n = (int)sizeof(code) - 1;
    for (int i = 0; i < n; i++) code[i] = oc[i];   /* copy BEFORE run_js_handler mutates b->raw */
    code[n] = 0;
    run_js_handler(b, code);
    return 1;
}
static int fire_onchange(browser_t *b, const char *id) { return fire_handler(b, id, "onchange"); }

/* Render plain text: words become WORD tokens, newlines become line breaks
 * (a blank line becomes a paragraph break), so file structure is preserved. */
static void parse_text(browser_t *b, const char *t, int len) {
    drop_image(b);                                       /* a page replaces any prior image */
    drop_image_slots(b);                                 /* and its inline images */
    b->textlen = b->ntok = b->hreflen = b->nlink = 0; b->title[0] = 0;
    b->sel = NO_LINK; b->curcolor = 0;
    memset(b->linky, 0xFF, sizeof(b->linky));
    int wstart = -1, nl = 0;
    for (int i = 0; i < len; i++) {
        char c = t[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (wstart >= 0) { emit_word(b, wstart, STY_NORMAL, NO_LINK); wstart = -1; }
            emit_break(b, ++nl >= 2 ? TK_PARA : TK_BREAK);
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (wstart >= 0) { emit_word(b, wstart, STY_NORMAL, NO_LINK); wstart = -1; }
            continue;
        }
        nl = 0;
        if (b->textlen < TEXT_MAX - 1) {
            if (wstart < 0) wstart = b->textlen;
            b->text[b->textlen++] = c;
        }
    }
    if (wstart >= 0) emit_word(b, wstart, STY_NORMAL, NO_LINK);
}

/* ---- networking (async) ----
 *
 * The HTTP fetch blocks for up to a second, so it runs on a dedicated kernel
 * worker task — the window manager keeps compositing (cursor live, "Loading…"
 * shown) while the page downloads. The worker only fills the `raw` buffer; the
 * actual HTML parse happens back on the WM thread in browser_poll(), so parsing
 * and rendering never race. One fetch at a time (guarded by g_busy/g_req).
 */
static void set_status(browser_t *b, const char *s) {
    int i = 0; while (s[i] && i < 39) { b->status[i] = s[i]; i++; } b->status[i] = 0;
}

/* Save/disable + restore interrupts: makes the WM<->worker hand-offs atomic
 * against preemption (the timer IRQ can switch tasks at any instruction). */
static inline uint64_t irq_save(void) {
    uint64_t f; __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory"); return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile("push %0; popfq" : : "r"(f) : "memory", "cc");
}

static volatile browser_t *g_req;        /* a browser queued for a fetch, or NULL */
static volatile int        g_busy;       /* worker is mid-fetch                   */
static task_t             *g_worker;

static void free_buffers(browser_t *b) {
    if (b->framebuf) kfree(b->framebuf);          /* animated GIF frames (img points into it) */
    else if (b->img) kfree(b->img);
    for (int i = 0; i < b->nimg; i++) if (b->imgs[i]) kfree(b->imgs[i]);
    kfree(b->lrec); kfree(b->links); kfree(b->hrefs);
    kfree(b->scripts);
    kfree(b->toks); kfree(b->text); kfree(b->raw); kfree(b);
}

/* Split a URL into host[] + return the path ("/" if none). */
static const char *url_split(const char *url, char *host, int hostsz) {
    const char *u = url;
    if      (startsw(u, "http://"))  u += 7;
    else if (startsw(u, "https://")) u += 8;
    int hi = 0;
    while (*u && *u != '/' && hi < hostsz - 1) host[hi++] = *u++;
    host[hi] = 0;
    return (*u == '/') ? u : "/";
}

/* Atomically claim the single fetch worker for b. 1 = claimed, 0 = busy. */
static int claim_fetch(browser_t *b) {
    uint64_t f = irq_save();
    if (g_busy || g_req) { irq_restore(f); return 0; }
    b->need_parse = 0; b->loading = 1; g_req = b;
    irq_restore(f);
    return 1;
}

/* Worker task: fetch b->url into b->raw. The close/finish decision is made under
 * a lock so EXACTLY ONE of the worker and browser_destroy frees b (review C1). */
static void worker_fetch(browser_t *b) {
    char host[96];
    const char *path = url_split(b->url, host, sizeof(host));
    int n; int https = startsw(b->url, "https://");
    if (!b->raw)    n = -1;
    else if (https) n = tls_get(host, path, (uint8_t *)b->raw, RAW_MAX - 1, (uint32_t)timer_ticks());
    else            n = http_get(host, path, b->raw, RAW_MAX - 1);
    b->cert_status = https ? tls_cert_status() : -2;   /* surface TLS cert result in the UI */
    b->chain_ok = https ? tls_chain_anchored() : 0;
    b->http_n = n;
    if (n > 0) { b->rawlen = n; b->raw[n] = 0; }
    uint64_t f = irq_save();
    int closed = b->closed;                    /* did the window close mid-fetch? */
    if (!closed) { b->need_parse = 1; b->loading = 0; }
    irq_restore(f);
    if (closed) free_buffers(b);               /* then we own the free */
}

static void worker_main(void) {
    for (;;) {
        uint64_t f = irq_save();
        browser_t *b = (browser_t *)g_req;
        if (b) { g_req = NULL; g_busy = 1; }   /* atomic pickup */
        irq_restore(f);
        if (b) { worker_fetch(b); g_busy = 0; }
        else timer_wait(2);                    /* ~20ms idle poll (hlt-based) */
    }
}

void browser_init(void) {
    if (!g_worker) {
        uint64_t cr3; __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        /* 256 KB stack: this worker runs TLS + (future) bignum/RSA/ECDSA cert
         * verification, which overflows the default 16 KB task stack. */
        g_worker = task_create_stack(worker_main, cr3, 0, 256 * 1024);  /* pin to kernel space */
    }
}

/* Build the start page into b->raw (rendered locally, no network). The built-in
 * bookmarks are listed first, then any the user added to a "SITES" file on
 * disk (one host/URL per line) — so editing that file customizes the home page. */
static void build_home(browser_t *b) {
    char *o = b->raw; int p = 0;
    #define HAPP(s) do { for (const char *q = (s); *q && p < RAW_MAX - 1; q++) o[p++] = *q; } while (0)
    HAPP("<html><head><title>OS-DEV Start</title></head><body>"
         "<h1>OS-DEV Browser</h1>"
         "<p>A from-scratch browser on a from-scratch OS, now over <b>real HTTPS</b> "
         "(a from-scratch TLS 1.3 client). Some pages that work:</p><dl>"
         "<dt><a href=\"https://text.npr.org\">https://text.npr.org</a></dt><dd>a LIVE news site over HTTPS &mdash; real headlines you can click</dd>"
         "<dt><a href=\"https://danluu.com\">https://danluu.com</a></dt><dd>a text-rich blog (minimal HTML, renders great)</dd>"
         "<dt><a href=\"https://example.com\">https://example.com</a></dt><dd>the classic example page, over TLS</dd>"
         "<dt><a href=\"https://www.gnu.org\">https://www.gnu.org</a></dt><dd>the GNU project (follow its links too)</dd>"
         "<dt><a href=\"http://example.com\">http://example.com</a></dt><dd>the same page over plain HTTP</dd>"
         "<dt><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">The WWW Project</a></dt>"
         "<dd>the original 1991 page about the World Wide Web</dd>"
         "<dt><a href=\"file:README.TXT\">file:README.TXT</a></dt><dd>a local file on the disk</dd>"
         "<dt><a href=\"file:index.htm\">Local demos</a></dt><dd>lists, tables, entities, colour, "
         "forms, inline code, and PNG / GIF / JPEG images &mdash; all rendered from scratch</dd>"
         "<dt><a href=\"file:jstest.htm\">file:jstest.htm</a></dt><dd>a page whose content is generated "
         "<b>live by JavaScript</b> (a from-scratch JS interpreter runs its &lt;script&gt;)</dd>"
         "<dt><a href=\"file:jsclick.htm\">file:jsclick.htm</a></dt><dd>links that <b>run JavaScript on click</b> "
         "(a from-scratch JS engine: arrows, JSON, try/catch &mdash; also at the shell: <tt>js</tt>)</dd>"
         "<dt><a href=\"file:anim.gif\">anim.gif</a></dt><dd>an animated GIF; "
         "<a href=\"file:pphoto.jpg\">pphoto.jpg</a> a progressive JPEG; "
         "<a href=\"file:inter.png\">inter.png</a> an interlaced PNG</dd>");

    char bm[512];
    long n = vfs_read("SITES", bm, sizeof(bm) - 1);   /* user bookmarks, if any */
    if (n > 0) {
        bm[n] = 0;
        int ls = 0;
        for (int i = 0; i <= (int)n; i++) {
            if (i == (int)n || bm[i] == '\n' || bm[i] == '\r') {
                if (i > ls) {
                    bm[i] = 0;
                    const char *url = bm + ls;
                    HAPP("<dt><a href=\"");
                    if (!startsw(url, "http://") && !startsw(url, "https://") && !startsw(url, "file:"))
                        HAPP("http://");
                    HAPP(url); HAPP("\">"); HAPP(url); HAPP("</a></dt><dd>bookmark</dd>");
                }
                ls = i + 1;
            }
        }
    }

    HAPP("</dl><hr>"
         "<p>This browser renders <b>bold</b> and <i>italic</i> text. Type a host "
         "(or file:NAME) and Enter, or click a link. Keyboard: Tab/n next link, "
         "p previous, Enter to follow, Backspace to go back, s to save, a to bookmark, "
         "\\ to find text. Bookmarks live in a SITES file (one URL per line).</p></body></html>");
    #undef HAPP
    b->bodyoff = 0; b->bodylen = p;   /* start page body region (for click-time JS) */
    parse_html(b, b->raw, p);
}

static void drop_image(browser_t *b) {
    if (b->framebuf) { kfree(b->framebuf); b->framebuf = 0; b->nframes = 0; b->img = 0; }
    else if (b->img) { kfree(b->img); }
    b->img = 0;                                   /* img points into framebuf when animated */
}

static void drop_image_slots(browser_t *b) {
    for (int i = 0; i < b->nimg; i++) { if (b->imgs[i]) kfree(b->imgs[i]); b->imgs[i] = 0; }
    b->nimg = 0;
}

/* Decode a PNG/GIF blob into a freshly kmalloc'd RGBA buffer (caller frees).
 * Bounds the dimensions, allocates exactly what's needed, frees its own
 * scratch. Returns the RGBA buffer and sets ow/oh, or NULL on any failure. */
static uint8_t *decode_image(const uint8_t *data, int len, int *ow, int *oh) {
    /* JPEG (FF D8 FF ...): probe for dimensions + scratch size, then decode */
    if (len >= 4 && data[0]==0xFF && data[1]==0xD8 && data[2]==0xFF) {
        int w, h; long need;
        if (jpeg_probe(data, len, &w, &h, &need) != 0) return 0;
        if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || (long)w * h > 1024*1024) return 0;
        if (need <= 0 || need > 8*1024*1024) return 0;
        long rgba_sz = (long)w * h * 4;
        uint8_t *rgba = kmalloc((unsigned long)rgba_sz);
        uint8_t *scr  = kmalloc((unsigned long)need);
        if (!rgba || !scr) { if (rgba) kfree(rgba); if (scr) kfree(scr); return 0; }
        int r = jpeg_decode(data, len, rgba, (int)rgba_sz, scr, (int)need, ow, oh);
        kfree(scr);
        if (r != 0) { kfree(rgba); return 0; }
        return rgba;
    }
    static const uint8_t pngsig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    int ispng = 0, isgif = 0;
    if (len >= 24) { ispng = 1; for (int i = 0; i < 8; i++) if (data[i] != pngsig[i]) ispng = 0; }
    if (!ispng && len >= 13 && data[0]=='G'&&data[1]=='I'&&data[2]=='F'&&data[3]=='8') isgif = 1;
    if (!ispng && !isgif) return 0;

    int w, h;
    if (ispng) {                                              /* IHDR width/height */
        w = (int)(((uint32_t)data[16]<<24)|((uint32_t)data[17]<<16)|((uint32_t)data[18]<<8)|data[19]);
        h = (int)(((uint32_t)data[20]<<24)|((uint32_t)data[21]<<16)|((uint32_t)data[22]<<8)|data[23]);
    } else {                                                  /* GIF logical screen size */
        w = data[6] | (data[7]<<8); h = data[8] | (data[9]<<8);
    }
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || (long)w * h > 1024*1024) return 0;
    long rgba_sz = (long)w * h * 4;
    long scr_sz  = ispng ? ((long)(w*4+1)*h + len + 64)       /* PNG: unfiltered rows + IDAT copy */
                         : ((long)w*h + 16);                  /* GIF: one index per pixel */
    uint8_t *rgba = kmalloc((unsigned long)rgba_sz);
    uint8_t *scr  = kmalloc((unsigned long)scr_sz);
    if (!rgba || !scr) { if (rgba) kfree(rgba); if (scr) kfree(scr); return 0; }
    int r = ispng ? png_decode(data, len, rgba, (int)rgba_sz, scr, (int)scr_sz, ow, oh)
                  : gif_decode(data, len, rgba, (int)rgba_sz, scr, (int)scr_sz, ow, oh);
    kfree(scr);
    if (r != 0) { kfree(rgba); return 0; }
    return rgba;
}

/* If `data` looks like a PNG or GIF, decode it into b->img (full-page image
 * view) and return 1; else 0. */
static int try_image(browser_t *b, const uint8_t *data, int len) {
    /* a GIF (possibly animated) -> decode every frame for full-page playback */
    if (len >= 13 && data[0]=='G'&&data[1]=='I'&&data[2]=='F'&&data[3]=='8') {
        int W = data[6] | (data[7]<<8), H = data[8] | (data[9]<<8);
        if (W > 0 && H > 0 && W <= 2048 && H <= 2048 && (long)W * H <= 1024*1024) {
            long fsz = (long)W * H * 4;
            int maxf = 64;
            while (maxf > 1 && fsz * maxf > 8L*1024*1024) maxf--;   /* cap total frame memory */
            uint8_t *frames = kmalloc((unsigned long)(fsz * maxf));
            uint8_t *scr    = kmalloc((unsigned long)(fsz + (long)W * H));
            if (frames && scr) {
                int w, h, nf = gif_decode_anim(data, len, frames, (int)(fsz * maxf),
                                               scr, (int)(fsz + (long)W * H), &w, &h,
                                               b->framedelay, maxf);
                kfree(scr);
                if (nf >= 1) {
                    drop_image(b); drop_image_slots(b);
                    b->framebuf = frames; b->nframes = nf; b->curframe = 0;
                    b->frametick = timer_ticks();
                    b->img = frames; b->imgw = w; b->imgh = h;
                    b->ntok = 0; b->nlrec = 0; b->sel = NO_LINK; b->find_tok = -1; b->scroll = 0;
                    return 1;
                }
                kfree(frames);
            } else { if (frames) kfree(frames); if (scr) kfree(scr); }
        }
    }
    int ow, oh;
    uint8_t *rgba = decode_image(data, len, &ow, &oh);
    if (!rgba) return 0;
    drop_image(b);
    drop_image_slots(b);                /* a full-page image replaces any inline ones */
    b->img = rgba; b->imgw = ow; b->imgh = oh;
    b->ntok = 0; b->nlrec = 0; b->sel = NO_LINK; b->find_tok = -1; b->scroll = 0;
    return 1;
}

/* Read a local file and decode it into the next inline-image slot. Returns the
 * slot index, or -1 (caller then falls back to a clickable link). */
static int decode_local_to_slot(browser_t *b, const char *path) {
    if (b->nimg >= IMG_SLOTS) return -1;
    uint8_t *buf = kmalloc(IMG_READ_MAX);
    if (!buf) return -1;
    long n = vfs_read(path, buf, IMG_READ_MAX);
    if (n <= 0) { kfree(buf); return -1; }
    int ow, oh;
    uint8_t *rgba = decode_image(buf, (int)n, &ow, &oh);
    kfree(buf);
    if (!rgba) return -1;
    int s = b->nimg++;
    b->imgs[s] = rgba; b->imgsw[s] = ow; b->imgsh[s] = oh;
    return s;
}

/* Request an async load of b->url. If the worker is busy, remember the intent
 * (b->want) and retry from browser_poll(), so a load is never silently dropped. */
static void browser_navigate(browser_t *b) {
    if (!b->raw || !b->text || !b->toks) return;
    b->bodyoff = 0; b->bodylen = 0;   /* clean baseline; HTML paths set the real region */
    b->ls_n = 0;                      /* fresh localStorage per page */
    b->in_n = 0; b->focus_id[0] = 0;  /* fresh input-field state per page */
    b->form_action[0] = 0;            /* and no carried-over form action */

    if (streqs(b->url, "home") || !b->url[0]) {       /* built-in start page, no net */
        if (b->loading) { set_status(b, "busy, retry"); return; }
        if (!b->is_back && b->cur[0] && !streqs(b->cur, "home") && b->histn < 16)
            copy_url(b->hist[b->histn++], b->cur);
        b->is_back = 0; copy_url(b->url, "home"); copy_url(b->cur, "home");
        b->want = 0; b->scroll = 0;
        build_home(b);
        set_status(b, "home");
        return;
    }

    if (startsw(b->url, "file:")) {                   /* a local file, no net */
        if (b->loading) { set_status(b, "busy, retry"); return; }
        if (!b->is_back && b->cur[0] && !streqs(b->cur, b->url) && b->histn < 16)
            copy_url(b->hist[b->histn++], b->cur);
        b->is_back = 0; copy_url(b->cur, b->url);
        b->want = 0; b->scroll = 0;
        long n = vfs_read(b->url + 5, b->raw, RAW_MAX - 1);
        if (n < 0) { b->ntok = 0; set_status(b, "file not found"); return; }
        b->raw[n] = 0; b->rawlen = (int)n;
        if (try_image(b, (const uint8_t *)b->raw, (int)n)) { set_status(b, "image"); return; }
        const char *q = b->raw; while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') q++;
        if (*q == '<') { b->bodyoff = 0; b->bodylen = (int)n; parse_html(b, b->raw, (int)n);  /* looks like HTML */
                         if (b->scriptlen > 0) run_page_scripts(b, 0, (int)n); }
        else           parse_text(b, b->raw, (int)n);  /* plain text */
        set_status(b, "local file");
        return;
    }

    if (!claim_fetch(b)) { b->want = 1; set_status(b, "queued..."); return; }
    b->want = 0;
    /* push the page we're leaving (unless this IS a Back, or a same-URL reload) */
    if (!b->is_back && b->cur[0] && !streqs(b->cur, b->url) && b->histn < 16)
        copy_url(b->hist[b->histn++], b->cur);
    b->is_back = 0;
    copy_url(b->cur, b->url);                  /* this is now the current page */
    b->ntok = 0; b->nlrec = 0; b->scroll = 0;
    set_status(b, "loading...");
}

void browser_back(browser_t *b) {
    if (!b || b->histn <= 0) return;
    const char *dest = b->hist[b->histn - 1];  /* peek the destination */

    if (streqs(dest, "home") || !dest[0] || startsw(dest, "file:")) {
        /* local page: render directly, no worker. Stay race-safe: if a fetch is
         * in flight, bail unchanged (same as the net path losing the claim). */
        uint64_t f = irq_save();
        if (g_busy || g_req) { irq_restore(f); return; }
        b->loading = 0;
        irq_restore(f);
        b->histn--;
        copy_url(b->url, dest);
        b->is_back = 1;                        /* navigate must not re-push history */
        browser_navigate(b);
        return;
    }

    /* network destination: set b->url to the target BEFORE claiming, so a worker
     * that picks up g_req in the gap can't fetch the stale (current) url. This
     * mirrors browser_navigate's set-then-claim ordering. */
    copy_url(b->url, dest);
    if (!claim_fetch(b)) { copy_url(b->url, b->cur); return; }  /* lost race: restore, keep history */
    b->histn--;                                /* pop only after a successful claim */
    b->is_back = 0;
    copy_url(b->cur, b->url);
    b->ntok = 0; b->nlrec = 0; b->scroll = 0;
    set_status(b, "loading...");
}

void browser_go(browser_t *b, const char *url) {  /* navigate to a given URL */
    if (!b || !url) return;
    copy_url(b->url, url);
    browser_navigate(b);
}

/* Called by the WM each frame: when the worker has delivered bytes, parse them
 * here (on the WM thread). Returns 1 if the display changed. */
static void goto_href(browser_t *b, const char *href, int suppress_push);  /* fwd */
static int tok_matches(browser_t *b, tok_t *tk);                            /* fwd (in-page find) */

/* Does the response advertise "Transfer-Encoding: chunked"? Scans only the
 * header region [0, hdr_end) (case-insensitive). HTTP/1.1 servers (and CDNs)
 * often chunk the body even when we asked in HTTP/1.0; without decoding it, the
 * chunk-size hex lines render as garbage interleaved in the page. */
static int is_chunked(const char *raw, int hdr_end) {
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

/* Decode an HTTP chunked-transfer body in place. `body` points at the first
 * chunk-size line; `len` is the bytes available. Returns the decoded length.
 * Each chunk is "<hexsize>[;ext]CRLF <data> CRLF", terminated by a 0-size chunk.
 * Tolerant of truncation (we may have stopped mid-stream on the time budget). */
static int dechunk(char *body, int len) {
    int in = 0, out = 0;
    while (in < len) {
        unsigned sz = 0; int sawdigit = 0;            /* parse the hex size (unsigned) */
        while (in < len && body[in] != '\r' && body[in] != '\n' && body[in] != ';') {
            char c = body[in]; int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;                               /* not a hex digit: stop */
            if (sz > (unsigned)RAW_MAX) sz = (unsigned)RAW_MAX;  /* cap so sz*16 can't overflow */
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

/* Find the value of a "Location:" header in an HTTP response. */
static int find_loc(const char *raw, int n, char *out, int max) {
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

int browser_poll(browser_t *b) {
    int changed = 0;
    /* animated GIF: advance to the next frame once its delay (centiseconds, and
     * the PIT runs at 100 Hz so 1 tick == 1 cs) has elapsed, and ask for a redraw. */
    if (b->framebuf && b->nframes > 1) {
        uint64_t now = timer_ticks();
        int d = b->framedelay[b->curframe];
        if (d < 2) d = 10;                          /* clamp 0/too-fast to ~100ms, like browsers */
        if (now - b->frametick >= (uint64_t)d) {
            b->curframe = (b->curframe + 1) % b->nframes;
            b->img = b->framebuf + (long)b->curframe * b->imgw * b->imgh * 4;
            b->frametick = now;
            changed = 1;
        }
    }
    if (b->want && !b->loading) { browser_navigate(b); changed = 1; }  /* retry queued load */
    if (!b->need_parse) return changed;
    b->need_parse = 0;
    int n = b->http_n;
    if (n <= 0) { b->ntok = 0; set_status(b, "failed"); b->redirects = 0; return 1; }

    if (n > 12 && b->raw[9] == '3') {               /* HTTP/1.x 3xx: follow Location */
        char loc[URL_MAX];
        if (b->redirects < 5 && find_loc(b->raw, n, loc, sizeof(loc))) {
            b->redirects++;
            set_status(b, "redirect...");
            goto_href(b, loc, 1);                   /* follow, replacing history */
            return 1;
        }
    }
    b->redirects = 0;                               /* reached a final page */

    int bodyoff = 0;
    for (int i = 0; i + 3 < n; i++)
        if (b->raw[i]=='\r'&&b->raw[i+1]=='\n'&&b->raw[i+2]=='\r'&&b->raw[i+3]=='\n') { bodyoff = i + 4; break; }
    int bodylen = n - bodyoff;
    if (bodyoff > 0 && is_chunked(b->raw, bodyoff))      /* de-chunk before parsing */
        bodylen = dechunk(b->raw + bodyoff, bodylen);
    if (try_image(b, (const uint8_t *)(b->raw + bodyoff), bodylen)) {  /* an image? */
        set_status(b, "image"); return 1;
    }
    b->bodyoff = bodyoff; b->bodylen = bodylen;          /* remember for click-time JS re-render */
    parse_html(b, b->raw + bodyoff, bodylen);
    if (b->scriptlen > 0) run_page_scripts(b, bodyoff, bodylen);   /* run inline <script> (once) */

    char st[40]; int v = n, k = 0, p = 0; char tmp[12];
    if (!v) tmp[k++] = '0'; while (v) { tmp[k++] = '0'+v%10; v/=10; }
    while (k) st[p++] = tmp[--k]; st[p++] = 'b';
    /* short TLS indicator (fits the narrow status area): "TLS*" = chain validated
     * to a trusted root CA; "TLS+" = the server proved leaf-key possession but the
     * chain isn't anchored to a known root; "TLS?" = a check failed. */
    const char *cs = b->chain_ok ? " TLS*"
                   : b->cert_status == 0 ? " TLS+"
                   : b->cert_status == -1 ? " TLS?" : 0;
    if (cs) for (int i = 0; cs[i] && p < 39; i++) st[p++] = cs[i];
    st[p] = 0;
    set_status(b, st);
    return 1;
}

/* Follow link id `id`, resolving its href against the current URL, then load. */
/* Resolve `href` (absolute / root-relative / relative) against the current URL
 * and navigate there. If suppress_push, replace the current history entry
 * rather than pushing (used for redirects, so Back doesn't loop). */
static void goto_href(browser_t *b, const char *href, int suppress_push) {
    { const char *jp="javascript:"; int isjs=1; for (int k=0;k<11;k++) if (lc(href[k])!=jp[k]) { isjs=0; break; }
      if (isjs) { run_js_handler(b, href + 11); return; } }   /* javascript: (any case) runs, doesn't navigate */
    if (href[0] == '#') {                                /* in-page anchor: scroll to the element with that id */
        const char *id = href + 1;
        for (int i = 0; i < b->anc_n; i++)
            if (streqs(b->anc_id[i], id)) {
                int t = b->anc_tok[i];
                if (t < b->ntok) { b->scroll = b->toky[t] - 20; if (b->scroll < 0) b->scroll = 0; }
                return;
            }
        if (!id[0] || streqs(id, "top")) b->scroll = 0;  /* "#"/"#top" -> top; an unknown id stays put */
        return;
    }
    if (startsw(href, "mailto:")) return;

    char newurl[URL_MAX];
    if (startsw(href, "http://") || startsw(href, "https://") || startsw(href, "file:")) {  /* absolute */
        int i = 0; while (href[i] && i < URL_MAX-1) { newurl[i] = href[i]; i++; } newurl[i] = 0;
    } else {
        /* relative — resolve against the current page, KEEPING its scheme so a
         * link on an HTTPS page stays HTTPS (worker_fetch picks TLS by the
         * "https://" prefix; without it a relative link would drop to plain HTTP). */
        const char *cu = b->url;
        const char *scheme = startsw(cu, "https://") ? "https://" : "http://";
        if (startsw(cu, "http://")) cu += 7; else if (startsw(cu, "https://")) cu += 8;
        char host[96]; int hi = 0; while (cu[hi] && cu[hi] != '/' && hi < 95) { host[hi] = cu[hi]; hi++; } host[hi] = 0;

        int p = 0;
        for (const char *s = scheme; *s && p < URL_MAX-1; s++) newurl[p++] = *s;   /* scheme prefix */
        if (href[0] == '/' && href[1] == '/') {          /* protocol-relative //host/path */
            for (int i = 2; href[i] && p < URL_MAX-1; i++) newurl[p++] = href[i];
            newurl[p] = 0;
            copy_url(b->url, newurl);
            if (suppress_push) b->is_back = 1;
            browser_navigate(b);
            return;
        }
        for (int i = 0; host[i] && p < URL_MAX-1; i++) newurl[p++] = host[i];
        if (href[0] == '/') {                            /* absolute path */
            for (int i = 0; href[i] && p < URL_MAX-1; i++) newurl[p++] = href[i];
        } else {                                         /* relative to current dir */
            const char *cp = cu + hi;                    /* current path incl leading '/' */
            int lastslash = 0;
            for (int i = 0; cp[i]; i++) if (cp[i] == '/') lastslash = i + 1;
            if (p < URL_MAX-1) newurl[p++] = '/';
            for (int i = 0; i < lastslash && cp[i] && p < URL_MAX-1; i++)
                if (!(i == 0 && cp[0] == '/')) newurl[p++] = cp[i];
            for (int i = 0; href[i] && p < URL_MAX-1; i++) newurl[p++] = href[i];
        }
        newurl[p] = 0;
    }
    copy_url(b->url, newurl);
    if (suppress_push) b->is_back = 1;          /* replace, don't push history */
    browser_navigate(b);
}

static void browser_follow(browser_t *b, int id) {
    if (id == NO_LINK || id >= b->nlink) return;
    int off = b->links[id].off, len = b->links[id].len;
    const char *hp = b->hrefs + off;
    /* a javascript: handler is code, not a URL — run the FULL slice; the URL_MAX
     * copy below would truncate a longer handler into a syntax error. */
    int isin = (len > 6); if (isin) for (int k = 0; k < 6; k++) if (lc(hp[k]) != "input:"[k]) { isin = 0; break; }
    if (isin) {                                          /* focus an <input> field for typing */
        int n = len - 6; if (n > 31) n = 31;
        for (int i = 0; i < n; i++) b->focus_id[i] = hp[6 + i];
        b->focus_id[n] = 0;
        if (!in_get(b, b->focus_id)) in_set(b, b->focus_id, "");   /* ensure a store slot exists */
        set_status(b, "type into the field, Enter when done");
        parse_html(b, b->raw + b->bodyoff, b->bodylen);  /* re-render to show the focus cursor */
        return;
    }
    int ischk = (len > 6); if (ischk) for (int k = 0; k < 6; k++) if (lc(hp[k]) != "check:"[k]) { ischk = 0; break; }
    if (ischk) {                                         /* toggle a checkbox/radio */
        char cid[32]; int n = len - 6; if (n > 31) n = 31;
        for (int i = 0; i < n; i++) cid[i] = hp[6 + i];
        cid[n] = 0;
        const char *cur = in_get(b, cid);
        int turning_on = !(cur && streqs(cur, "on"));
        in_set(b, cid, turning_on ? "on" : "");                   /* flip; the re-render updates its submit name */
        if (turning_on) {                                         /* a radio turning on unchecks its group siblings */
            int as, ae; const char *tp; int tpl; const char *nm; int nml;
            if (dom_attr_region(b, cid, &as, &ae)
                && find_attr(b->raw + as, ae - as, "type", &tp, &tpl) && attr_eq(tp, tpl, "radio")
                && find_attr(b->raw + as, ae - as, "name", &nm, &nml) && nml > 0) {
                char grp[32]; int gn = nml; if (gn > 31) gn = 31; for (int i = 0; i < gn; i++) grp[i] = nm[i]; grp[gn] = 0;
                for (int j = 0; j < b->in_n; j++)                 /* the previously-checked sibling has its name == group */
                    if (!streqs(b->in_id[j], cid) && b->in_name[j][0] && streqs(b->in_name[j], grp)) b->in_val[j][0] = 0;
            }
        }
        if (!fire_onchange(b, cid)) parse_html(b, b->raw + b->bodyoff, b->bodylen);   /* onchange (if any) re-renders */
        return;
    }
    int issub = (len > 6); if (issub) for (int k = 0; k < 7; k++) if (lc(hp[k]) != "submit:"[k]) { issub = 0; break; }
    if (issub) {                                         /* a form submit: action?name=value&… (GET) */
        char q[URL_MAX]; int p = 0;
        int al = len - 7;                                /* the snapshotted action follows "submit:" */
        if (al > 0) { for (int i = 0; i < al && p < URL_MAX - 1; i++) q[p++] = hp[7 + i]; }
        else { for (int i = 0; b->url[i] && b->url[i] != '?' && p < URL_MAX - 1; i++) q[p++] = b->url[i]; }  /* no action -> this page */
        int first = 1;
        for (int f = 0; f < b->in_n; f++) {              /* every named field becomes a query pair */
            if (!b->in_name[f][0]) continue;
            if (p < URL_MAX - 1) q[p++] = first ? '?' : '&';
            first = 0;
            p += url_encode(q + p, URL_MAX - 1 - p, b->in_name[f]);
            if (p < URL_MAX - 1) q[p++] = '=';
            p += url_encode(q + p, URL_MAX - 1 - p, b->in_val[f]);
        }
        q[p] = 0;
        b->focus_id[0] = 0;                              /* leave any field-typing mode */
        goto_href(b, q, 0);
        return;
    }
    int isjs = (len > 11); if (isjs) for (int k = 0; k < 11; k++) if (lc(hp[k]) != "javascript:"[k]) { isjs = 0; break; }
    if (isjs) {
        static char jsbuf[4096];
        int n = len - 11; if (n > (int)sizeof(jsbuf) - 1) n = (int)sizeof(jsbuf) - 1;
        for (int i = 0; i < n; i++) jsbuf[i] = hp[11 + i];
        jsbuf[n] = 0;
        run_js_handler(b, jsbuf);
        return;
    }
    char href[URL_MAX]; int hl = len; if (hl > URL_MAX - 1) hl = URL_MAX - 1;
    for (int i = 0; i < hl; i++) href[i] = hp[i];
    href[hl] = 0;
    goto_href(b, href, 0);
}

/* ---- drawing ---- */
static void put_word(int x, int y, const char *s, int len, uint32_t fg, uint32_t bg, int scale) {
    char w[72]; if (len > 71) len = 71;
    for (int i = 0; i < len; i++) w[i] = s[i]; w[len] = 0;
    if (scale > 1) fb_text(x, y, w, fg, scale);
    else for (int i = 0; i < len; i++) fb_glyph(x + i*GW, y, w[i], fg, bg);
}
static void box(int x, int y, int w, int h, uint32_t c) {
    fb_fill_rect(x, y, w, 1, c); fb_fill_rect(x, y+h-1, w, 1, c);
    fb_fill_rect(x, y, 1, h, c); fb_fill_rect(x+w-1, y, 1, h, c);
}

void browser_render(browser_t *b, int x, int y, int w, int h) {
    uint32_t BG = 0xFFFFFF;
    fb_fill_rect(x, y, w, h, BG);

    /* address bar */
    fb_fill_rect(x, y, w, ADDR_H, 0xE7EAF0);
    fb_fill_rect(x, y + ADDR_H - 1, w, 1, 0x9AA3B2);
    int fy = y + 7;
    /* Back button (greyed when there's no history) */
    uint32_t bbc = (b->histn > 0) ? 0x2C66D6 : 0xAAB2BE;
    fb_fill_rect(x + 6, fy - 1, 18, 18, 0xF4F6FA); box(x + 6, fy - 1, 18, 18, 0xB4BCC8);
    put_word(x + 11, fy, "<", 1, bbc, 0xF4F6FA, 1);
    int fx = x + 30, fw = w - 112;
    fb_fill_rect(fx, fy, fw, 16, 0xFFFFFF);
    box(fx - 1, fy - 1, fw + 2, 18, b->editing ? 0x2C66D6 : 0xB4BCC8);
    int maxc = (fw - 6) / GW, ulen = (int)strlen(b->url);
    int from = (ulen > maxc) ? ulen - maxc : 0;
    for (int i = from; i < ulen; i++)
        fb_glyph(fx + 4 + (i - from)*GW, fy, b->url[i], 0x102030, 0xFFFFFF);
    if (b->editing) fb_fill_rect(fx + 4 + (ulen - from)*GW, fy + 1, 1, 14, 0x2C66D6);
    put_word(x + w - 78, fy, b->status, (int)strlen(b->status), 0x55606E, 0xE7EAF0, 1);

    /* content */
    int cl = x + 10, cr = x + w - 14, ct = y + ADDR_H + 6, cb = y + h - 8;
    b->view_h = cb - ct;
    int cx = cl, cy = ct - b->scroll, curlh = 18;
    b->nlrec = 0;

    if (b->loading) { fb_text(cl, ct + 12, "Loading...", 0x4A6A9A, 2); return; }

    if (b->viewsource) {                            /* 'u': raw HTML source, wrapped */
        int maxcols = (cr - cl) / GW; if (maxcols < 1) maxcols = 1;
        int cyv = ct - b->scroll, col = 0;
        for (int i = 0; i < b->rawlen; i++) {
            char ch = b->raw[i];
            if (ch == '\n') { cyv += 16; col = 0; continue; }
            if (ch == '\t') ch = ' ';
            if (ch < 32 || ch > 126) ch = '.';
            if (col >= maxcols) { cyv += 16; col = 0; }
            if (cyv >= ct && cyv + 14 <= cb) fb_glyph(cl + col*GW, cyv, ch, 0x206020, BG);
            col++;
        }
        b->content_h = (cyv + 16) - (ct - b->scroll);
    } else if (b->img) {                            /* decoded image: blit scaled to width */
        int maxw = cr - cl, destw = b->imgw, desth = b->imgh;
        if (destw > maxw && destw > 0) { desth = (int)((long)desth * maxw / destw); destw = maxw; }
        b->content_h = desth;
        for (int dy = 0; dy < desth; dy++) {
            int py = ct + dy - b->scroll;
            if (py < ct || py >= cb) continue;      /* clip to the content area */
            int sy = (int)((long)dy * b->imgh / (desth ? desth : 1));
            const uint8_t *srow = b->img + (long)sy * b->imgw * 4;
            for (int dx = 0; dx < destw; dx++) {
                int sx = (int)((long)dx * b->imgw / (destw ? destw : 1));
                const uint8_t *pp = srow + (long)sx * 4;
                int a = pp[3];                       /* alpha-blend over white */
                int r  = (pp[0]*a + 255*(255-a)) / 255;
                int g  = (pp[1]*a + 255*(255-a)) / 255;
                int bl = (pp[2]*a + 255*(255-a)) / 255;
                fb_pixel(cl + dx, py, ((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)bl);
            }
        }
    } else {
    for (int t = 0; t < b->ntok; t++) {
        tok_t *tk = &b->toks[t];
        if (tk->type == TK_BREAK) { cy += curlh; cx = cl; curlh = 18; continue; }
        if (tk->type == TK_PARA)  { cy += curlh + 8; cx = cl; curlh = 18; continue; }
        if (tk->type == TK_HR) {
            cy += curlh;
            if (cy + 4 >= ct && cy + 4 <= cb) fb_fill_rect(cl, cy + 4, cr - cl, 1, 0xC8CED8);
            cy += 12; cx = cl; curlh = 18; continue;
        }
        if (tk->type == TK_IMG) {                       /* inline image: blit on its own line */
            int idx = tk->link;
            if (idx >= 0 && idx < b->nimg && b->imgs[idx]) {
                int iw = b->imgsw[idx], ih = b->imgsh[idx];
                int aw = tk->off, ah = tk->len;          /* explicit <img> width/height, 0=natural */
                int maxw = cr - cl, destw, desth;
                if (aw > 0 || ah > 0) {                  /* honour specified size, fill from aspect */
                    destw = aw > 0 ? aw : (int)((long)ah * iw / (ih ? ih : 1));
                    desth = ah > 0 ? ah : (int)((long)aw * ih / (iw ? iw : 1));
                } else { destw = iw; desth = ih; }
                if (destw > maxw && destw > 0) { desth = (int)((long)desth * maxw / destw); destw = maxw; }
                if (desth > IMG_MAX_H && desth > 0) { destw = (int)((long)destw * IMG_MAX_H / desth); desth = IMG_MAX_H; }
                if (cx > cl) { cy += curlh; cx = cl; }   /* drop to a fresh line */
                if (t < TOK_MAX) b->toky[t] = cy - (ct - b->scroll);
                for (int dy = 0; dy < desth; dy++) {
                    int py = cy + dy;
                    if (py < ct || py >= cb) continue;   /* clip to content area */
                    int sy = (int)((long)dy * ih / (desth ? desth : 1));
                    const uint8_t *srow = b->imgs[idx] + (long)sy * iw * 4;
                    for (int dx = 0; dx < destw; dx++) {
                        int sx = (int)((long)dx * iw / (destw ? destw : 1));
                        const uint8_t *pp = srow + (long)sx * 4;
                        int a = pp[3];                   /* alpha-blend over white */
                        int r  = (pp[0]*a + 255*(255-a)) / 255;
                        int g  = (pp[1]*a + 255*(255-a)) / 255;
                        int bl = (pp[2]*a + 255*(255-a)) / 255;
                        fb_pixel(cl + dx, py, ((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)bl);
                    }
                }
                cy += desth + 6; cx = cl; curlh = 18;
            }
            continue;
        }

        int sc = scale_for(tk->style), lh = lineh_for(tk->style);
        int wpx = tk->len * GW * sc; if (wpx > cr - cl) wpx = cr - cl;
        if (cx + wpx > cr && cx > cl) { cy += curlh; cx = cl; curlh = 18; }
        if (lh > curlh) curlh = lh;

        /* record every link's content-space y (even off-screen) so keyboard
         * link selection can scroll it into view */
        if (tk->style == STY_LINK && tk->link != NO_LINK && tk->link < LINK_MAX)
            b->linky[tk->link] = cy - (ct - b->scroll);
        if (t < TOK_MAX) b->toky[t] = cy - (ct - b->scroll);   /* per-token y for in-page find */

        if (cy >= ct && cy + lh <= cb + 2) {
            int selected = (tk->style == STY_LINK && tk->link == b->sel);
            /* in-page find: highlight every match; the current one stronger */
            int matched = (b->find_tok >= 0 && tk->type == TK_WORD && tok_matches(b, tk));
            int current = (t == b->find_tok);
            uint32_t fg = color_for(tk->style);
            if (tk->style != STY_LINK && (b->tokcolor[t] & 0x01000000))   /* <font color> override */
                fg = b->tokcolor[t] & 0xFFFFFF;
            uint32_t wbg = selected ? 0xFFE9A8 : (current ? 0x7FC0FF : (matched ? 0xCDE8FF : BG));
            int maxc = (cr - cx) / (GW * sc); if (maxc < 0) maxc = 0;
            int dl = tk->len > maxc ? maxc : tk->len;      /* clip to content width (no h-scroll) */
            int drawpx = dl * GW * sc;
            put_word(cx, cy, b->text + tk->off, dl, fg, wbg, sc);
            if (tk->style == STY_BOLD) {                   /* faux-bold: transparent 1px overstrike */
                char w[72]; int ln = dl > 71 ? 71 : dl;
                for (int i = 0; i < ln; i++) w[i] = b->text[tk->off + i];
                w[ln] = 0;
                fb_text(cx + 1, cy, w, fg, sc);
            }
            if (tk->style == STY_STRIKE) fb_fill_rect(cx, cy + 7, drawpx, 1, fg);   /* strike-line through the text */
            if (tk->style == STY_LINK) {
                fb_fill_rect(cx, cy + 15, drawpx, 1, fg);
                if (selected) box(cx - 1, cy - 1, drawpx + 2, lh, 0xC08000);  /* selection outline */
                if (tk->link != NO_LINK && b->nlrec < LREC_MAX)   /* clickable */
                    b->lrec[b->nlrec++] = (lrec_t){ (int16_t)(cx - x), (int16_t)(cy - y),
                                                    (int16_t)drawpx, (int16_t)lh, tk->link };
            }
        }
        cx += wpx + GW * sc;
    }

    int bottom = cy + curlh;
    b->content_h = bottom - (ct - b->scroll);
    }                                           /* end token-render branch */

    int maxscroll = b->content_h - b->view_h; if (maxscroll < 0) maxscroll = 0;
    if (b->scroll > maxscroll) b->scroll = maxscroll;
    if (b->scroll < 0) b->scroll = 0;

    if (b->content_h > b->view_h) {
        int track = cb - ct, sbx = x + w - 6;
        fb_fill_rect(sbx, ct, 4, track, 0xD8DCE4);
        int th = track * b->view_h / b->content_h; if (th < 16) th = 16;
        int ty = ct + (track - th) * b->scroll / (maxscroll ? maxscroll : 1);
        fb_fill_rect(sbx, ty, 4, th, 0x8893A4);
    }
}

/* Reconstruct the page as readable text (words + line breaks) and write it to
 * PAGE.TXT on the FAT32 disk — a tiny "reader mode" / offline save. */
static void browser_save(browser_t *b) {
    if (b->ntok == 0) { set_status(b, "nothing to save"); return; }
    int cap = TEXT_MAX + 4096;
    char *out = kmalloc(cap);
    if (!out) { set_status(b, "save: nomem"); return; }
    int p = 0;
    for (int t = 0; t < b->ntok && p < cap - 8; t++) {
        tok_t *tk = &b->toks[t];
        if (tk->type == TK_WORD) {
            for (int i = 0; i < tk->len && p < cap - 2; i++) out[p++] = b->text[tk->off + i];
            out[p++] = ' ';
        } else if (tk->type == TK_PARA) { out[p++] = '\n'; out[p++] = '\n'; }
        else if (tk->type == TK_HR)     { for (const char *s = "\n----\n"; *s; s++) out[p++] = *s; }
        else                            { out[p++] = '\n'; }   /* TK_BREAK */
    }
    long n = vfs_write("PAGE.TXT", out, (unsigned long)p);
    kfree(out);
    set_status(b, n < 0 ? "save failed" : "saved PAGE.TXT");
}

/* Append the current page's URL to the SITES bookmarks file (read on the home
 * page, M67). Closes the loop: browse -> 'a' -> it shows up as a bookmark. */
static void browser_bookmark(browser_t *b) {
    if (!startsw(b->cur, "http://") && !startsw(b->cur, "https://") && !startsw(b->cur, "file:")) {
        set_status(b, "not bookmarkable"); return;        /* don't bookmark "home"/empty */
    }
    static char sb[2048];
    long n = vfs_read("SITES", sb, sizeof(sb) - URL_MAX - 2);
    if (n < 0) n = 0;
    sb[n] = 0;
    /* skip if this exact URL is already a line in SITES */
    for (int i = 0; i <= (int)n; ) {
        int j = i; while (j < (int)n && sb[j] != '\n' && sb[j] != '\r') j++;
        char save = sb[j]; sb[j] = 0;
        if (streqs(sb + i, b->cur)) { sb[j] = save; set_status(b, "already saved"); return; }
        sb[j] = save; i = j + 1;
    }
    int p = (int)n;
    if (p > 0 && sb[p-1] != '\n') sb[p++] = '\n';          /* ensure newline-separated */
    for (int i = 0; b->cur[i] && p < (int)sizeof(sb) - 2; i++) sb[p++] = b->cur[i];
    sb[p++] = '\n';
    long w = vfs_write("SITES", sb, (unsigned long)p);
    set_status(b, w < 0 ? "bookmark failed" : "bookmarked");
}

/* Move the keyboard link selection by `dir` (+1 next / -1 prev), wrapping, then
 * scroll it into view and show its target href in the status line. This makes
 * the browser fully keyboard-drivable (and testable without a mouse). */
static void select_link(browser_t *b, int dir) {
    if (b->nlink <= 0) { set_status(b, "no links"); return; }
    int s = b->sel;
    if (s == NO_LINK || s < 0 || s >= b->nlink) s = (dir > 0) ? 0 : b->nlink - 1;
    else { s += dir; if (s < 0) s = b->nlink - 1; else if (s >= b->nlink) s = 0; }
    b->sel = s;
    if (s < LINK_MAX && b->linky[s] >= 0) {              /* only if laid out this page */
        b->scroll = b->linky[s] - 20; if (b->scroll < 0) b->scroll = 0;
    }
    char st[40]; int p = 0, hl = b->links[s].len, off = b->links[s].off;  /* show Enter target */
    for (int i = 0; i < hl && p < (int)sizeof(st) - 1; i++) st[p++] = b->hrefs[off + i];
    st[p] = 0;
    set_status(b, st);
}

/* ---- in-page find ---- */
static int tok_matches(browser_t *b, tok_t *tk) {       /* token text contains findq (ci)? */
    int qn = 0; while (b->findq[qn]) qn++;
    if (qn == 0 || tk->len < qn) return 0;
    for (int s = 0; s + qn <= tk->len; s++) {
        int j = 0;
        while (j < qn && lc(b->text[tk->off + s + j]) == lc(b->findq[j])) j++;
        if (j == qn) return 1;
    }
    return 0;
}
/* Find a matching word token starting at `start` and stepping by `dir` (+1 next
 * / -1 previous), wrapping, and scroll to it. */
static void do_find(browser_t *b, int start, int dir) {
    if (!b->findq[0]) { set_status(b, "find: type text"); return; }
    if (b->ntok <= 0) { b->find_tok = -1; set_status(b, "no text"); return; }
    for (int n = 0; n < b->ntok; n++) {
        int t = ((start + dir * n) % b->ntok + b->ntok) % b->ntok;
        if (b->toks[t].type == TK_WORD && tok_matches(b, &b->toks[t])) {
            b->find_tok = t;
            if (t < TOK_MAX) { b->scroll = b->toky[t] - 30; if (b->scroll < 0) b->scroll = 0; }
            char st[40]; int p = 0; const char *pre = "found: ";
            while (*pre && p < 39) st[p++] = *pre++;
            for (int i = 0; b->findq[i] && p < 39; i++) st[p++] = b->findq[i];
            st[p] = 0; set_status(b, st);
            return;
        }
    }
    b->find_tok = -1; set_status(b, "not found");
}
/* Show "find: <query>" in the status line while typing. */
static void find_prompt(browser_t *b) {
    char st[40]; int p = 0; const char *pre = "find: ";
    while (*pre && p < 39) st[p++] = *pre++;
    for (int i = 0; b->findq[i] && p < 39; i++) st[p++] = b->findq[i];
    st[p] = 0; set_status(b, st);
}

/* Does the address-bar text look like a web-search query rather than a URL?
 * A scheme (http/https/file), the home keyword, or a dotted host -> navigate;
 * a space or no dot at all (e.g. "operating systems", "weather") -> search. */
static int looks_like_search(const char *s) {
    if (startsw(s, "http://") || startsw(s, "https://") || startsw(s, "file:")) return 0;
    if (!s[0] || streqs(s, "home")) return 0;
    int hasdot = 0, hasspace = 0;
    for (int i = 0; s[i]; i++) { if (s[i] == '.') hasdot = 1; if (s[i] == ' ') hasspace = 1; }
    return hasspace || !hasdot;
}
/* Rewrite the address-bar query into a DuckDuckGo HTML search URL (in place). */
static void make_search_url(char *url) {
    char q[URL_MAX]; int p = 0;
    const char *pfx = "https://html.duckduckgo.com/html/?q=";
    for (int i = 0; pfx[i] && p < URL_MAX - 1; i++) q[p++] = pfx[i];
    p += url_encode(q + p, URL_MAX - 1 - p, url);
    q[p] = 0;
    copy_url(url, q);
}

void browser_key(browser_t *b, int c) {
    if (b->focus_id[0]) {                               /* typing into a focused <input> field */
        if (c == '\n' || c == '\r' || c == 27) {
            char fid[32]; { int k=0; while (b->focus_id[k] && k<31) { fid[k]=b->focus_id[k]; k++; } fid[k]=0; }   /* the field losing focus */
            b->focus_id[0] = 0;                          /* leave typing mode */
            if (c != 27) {                               /* Enter (not Esc): submit the form if it has a submit button */
                for (int i = 0; i < b->nlink; i++) {
                    const char *h = b->hrefs + b->links[i].off; int hl = b->links[i].len;
                    int sub = (hl > 6); if (sub) for (int k = 0; k < 7; k++) if (lc(h[k]) != "submit:"[k]) { sub = 0; break; }
                    if (sub) { browser_follow(b, i); return; }
                }
            }
            set_status(b, "");
            if (!fire_onchange(b, fid)) parse_html(b, b->raw + b->bodyoff, b->bodylen);   /* onchange fires on blur; it re-renders */
        }
        else if (c == 8 || c == 127) {                  /* backspace */
            const char *cur = in_get(b, b->focus_id);
            if (cur && cur[0]) { char t[96]; int n=0; while(cur[n]&&n<95){t[n]=cur[n];n++;} t[n-1]=0; in_set(b, b->focus_id, t);
                if (!fire_handler(b, b->focus_id, "oninput")) parse_html(b, b->raw + b->bodyoff, b->bodylen); }
        } else if (c >= 32 && c < 127) {                /* a printable char */
            const char *cur = in_get(b, b->focus_id); char t[96]; int n=0;
            if (cur) while (cur[n] && n<94) { t[n]=cur[n]; n++; }
            if (n < 94) { t[n++]=(char)c; t[n]=0; in_set(b, b->focus_id, t);
                if (!fire_handler(b, b->focus_id, "oninput")) parse_html(b, b->raw + b->bodyoff, b->bodylen); }   /* oninput fires per keystroke */
        }
        return;
    }
    if (b->finding) {                                   /* in-page find input mode */
        if (c == '\n' || c == '\r' || c == 0x12)         /* Enter / Down: next match */
            do_find(b, b->find_tok >= 0 ? b->find_tok + 1 : 0, +1);
        else if (c == 0x11)                              /* Up: previous match */
            do_find(b, b->find_tok >= 0 ? b->find_tok - 1 : 0, -1);
        else if (c == 27)            { b->finding = 0; b->find_tok = -1; set_status(b, ""); }
        else if (c == 8 || c == 127) { int n = (int)strlen(b->findq); if (n) b->findq[n-1] = 0; find_prompt(b); }
        else if (c >= 32 && c < 127) { int n = (int)strlen(b->findq); if (n < (int)sizeof(b->findq)-1) { b->findq[n]=(char)c; b->findq[n+1]=0; } find_prompt(b); }
        return;
    }
    if (b->editing) {
        if (c == '\n' || c == '\r') { b->editing = 0; if (looks_like_search(b->url)) make_search_url(b->url); browser_navigate(b); }
        else if (c == 27)            { b->editing = 0; }
        else if (c == 8 || c == 127) { b->edit_fresh = 0; int n = (int)strlen(b->url); if (n) b->url[n-1] = 0; }   /* editing the existing URL */
        else if (c >= 32 && c < 127) { if (b->edit_fresh) { b->url[0] = 0; b->edit_fresh = 0; }   /* first keystroke replaces the shown URL */
                                       int n = (int)strlen(b->url); if (n < URL_MAX-1) { b->url[n]=(char)c; b->url[n+1]=0; } }
        return;
    }
    if (c == 8 || c == 127) { browser_back(b); return; }   /* Backspace = Back */
    int page = b->view_h ? b->view_h * 3 / 4 : 200;
    switch (c) {
    case ' ': case 'f': case 0x16: b->scroll += page; break;   /* space/f/PgDn */
    case 'b':           case 0x15: b->scroll -= page; break;   /* b/PgUp        */
    case 'j': case 0x12: b->scroll += 40;  break;          /* j / down-arrow */
    case 'k': case 0x11: b->scroll -= 40;  break;          /* k / up-arrow   */
    case 'g':           b->scroll = 0;     break;   /* top */
    case 'G':           b->scroll = 1 << 24; break; /* bottom (render clamps to maxscroll) */
    case 'h':           copy_url(b->url, "home"); browser_navigate(b); break;  /* start page */
    case 'r':           browser_navigate(b); break;
    case 's':           browser_save(b);   break;   /* save page to PAGE.TXT */
    case 'u':           if (!b->img) { b->viewsource = !b->viewsource; b->scroll = 0;  /* toggle raw HTML */
                            set_status(b, b->viewsource ? "source" : ""); } break;
    case 'a':           browser_bookmark(b); break; /* add current URL to SITES */
    case '/': case 'e': b->editing = 1; b->edit_fresh = 1;    break;
    case '\\':          b->finding = 1; b->findq[0] = 0; b->find_tok = -1; set_status(b, "find: "); break;
    case '\t': case 'n': select_link(b, +1); break; /* next link (keyboard nav) */
    case 'p':            select_link(b, -1); break; /* previous link */
    case '\n': case '\r':                            /* follow the selected link */
        if (b->sel != NO_LINK) browser_follow(b, b->sel);
        break;
    }
    if (b->scroll < 0) b->scroll = 0;
}

int browser_click(browser_t *b, int rx, int ry, int w, int h) {
    (void)w; (void)h;
    if (ry < ADDR_H) {
        if (rx >= 6 && rx < 24) browser_back(b);          /* the Back button */
        else { b->editing = 1; b->edit_fresh = 1; }       /* edit the address */
        return 1;
    }
    b->editing = 0;
    for (int i = 0; i < b->nlrec; i++) {                 /* a link? follow it */
        lrec_t *L = &b->lrec[i];
        if (rx >= L->x && rx < L->x + L->w && ry >= L->y && ry < L->y + L->h) {
            browser_follow(b, L->link);
            return 1;
        }
    }
    return 1;
}

browser_t *browser_create(const char *url) {
    browser_init();                          /* ensure the fetch worker exists */
    browser_t *b = kzalloc(sizeof(browser_t));
    if (!b) return NULL;
    b->raw   = kmalloc(RAW_MAX);
    b->text  = kmalloc(TEXT_MAX);
    b->toks  = kmalloc(sizeof(tok_t) * TOK_MAX);
    b->hrefs = kmalloc(HREF_MAX);
    b->links = kmalloc(sizeof(href_t) * LINK_MAX);
    b->lrec  = kmalloc(sizeof(lrec_t) * LREC_MAX);
    b->scripts = kmalloc(SCRIPT_MAX);
    if (!url || !url[0]) url = "home";        /* open the start page by default */
    int i = 0; while (url[i] && i < URL_MAX-1) { b->url[i] = url[i]; i++; }
    b->url[i] = 0;
    if (b->raw && b->text && b->toks && b->hrefs && b->links && b->lrec && b->scripts)
        browser_navigate(b);
    else set_status(b, "nomem");
    return b;
}

void browser_destroy(browser_t *b) {
    if (!b) return;
    uint64_t f = irq_save();
    int defer;
    if (g_req == b) {            /* queued, worker hasn't started: cancel + we free */
        g_req = NULL; defer = 0;
    } else if (b->loading) {     /* worker is mid-fetch on b: it frees when done */
        b->closed = 1; defer = 1;
    } else {                     /* idle/done: we free */
        defer = 0;
    }
    irq_restore(f);
    if (!defer) free_buffers(b);
}

const char *browser_title(browser_t *b) {
    return (b && b->title[0]) ? b->title : "Browser";
}
