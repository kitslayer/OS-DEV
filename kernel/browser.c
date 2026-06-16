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
#include "svg.h"
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
#define REMOTE_IMG_MAX 3            /* remote <img> URLs prefetched per page (best-effort) */

enum { STY_NORMAL, STY_H1, STY_H2, STY_LINK, STY_BOLD, STY_EM, STY_CODE, STY_STRIKE, STY_MARK, STY_SUB, STY_SUP };
enum { TK_WORD, TK_BREAK, TK_PARA, TK_HR, TK_IMG };   /* TK_IMG: link field = image slot */

typedef struct { uint16_t off, len, link; uint8_t style, type; } tok_t;
typedef struct { uint16_t off, len; } href_t;            /* slice into hrefs[] */
typedef struct { int16_t x, y, w, h; uint16_t link; } lrec_t;  /* a clickable rect */
typedef struct { char tag[16]; char cls[32]; char id[32]; char attr[32]; } sel_t;  /* one simple CSS selector (tag/.class/#id/[attr]) */

#define CSS_MAX 24                  /* simple style rules captured from <style> blocks per page */
#define SC_MAX  16                  /* max nesting depth of active style scopes (color/weight) */

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
    int     host_match;                                  /* TLS hostname match: -2 n/a, 1 ok, 0 mismatch */
    char    cert_cn[48], cert_expiry[16];                /* leaf cert identity, for the 'i' cert-info display */
    int     zoom;                                        /* content zoom multiplier (1..4), persists across navigation */
    volatile int want;                                   /* load queued (worker busy) */
    char    cur[URL_MAX];                                /* currently shown URL */
    char    hist[16][URL_MAX]; int histn;                /* back stack          */
    int     is_back;                                     /* this nav is a Back  */
    int     redirects;                                   /* HTTP 3xx hop count  */
    int     listdepth;                                   /* nested <ul>/<ol> depth */
    char    listtype[8];                                 /* 'u' or 'o' per level */
    int     listnum[8];                                  /* <ol> item counter per level */
    char    listfmt[8];                                  /* <ol type>: '1'/'a'/'A'/'i'/'I' per level */
    int     tdcount;                                     /* cells emitted in the current <tr> */
    int     finding;                                     /* in-page find: typing a query */
    char    findq[40];                                   /* the find query */
    int     find_tok;                                    /* highlighted match token (-1 none) */
    int     toky[TOK_MAX];                               /* content-space y of every token (scroll-to) */
    char    anc_id[32][32]; uint16_t anc_tok[32]; int anc_n;   /* id -> token index, for #fragment scroll-to */
    uint8_t det_open[16];                                      /* <details> open state by index (0xFF=unseeded), persists across re-renders */
    uint8_t *img; int imgw, imgh;                        /* current full-page frame (RGBA), or NULL */
    uint8_t *framebuf; int nframes, curframe;            /* animated GIF: all frames + current */
    int      framedelay[64];                             /* per-frame delay (centiseconds) */
    uint64_t frametick;                                  /* tick at which curframe was shown */
    uint8_t *imgs[IMG_SLOTS]; int imgsw[IMG_SLOTS], imgsh[IMG_SLOTS]; int nimg;  /* inline images */
    char     rimg_src[REMOTE_IMG_MAX][96];   /* the RAW src string as it appears in the <img> (for matching) */
    uint8_t *rimg_data[REMOTE_IMG_MAX];      /* the fetched COMPRESSED image bytes (kmalloc'd), or NULL */
    int      rimg_len[REMOTE_IMG_MAX];       /* byte length of rimg_data[i] */
    int      n_rimg;                         /* how many remote images prefetched (0..REMOTE_IMG_MAX) */
    uint32_t curcolor;                                   /* <font color> in effect (0=none, else 0x01000000|rgb) */
    uint32_t tokcolor[TOK_MAX];                          /* per-token colour override */
    int      curul;                                      /* underline in effect (text-decoration:underline / <u>/<ins>); independent of style, so it composes */
    uint8_t  tokul[TOK_MAX];                             /* per-token underline flag */
    int      curtransform;                               /* text-transform in effect: 0=none, 1=uppercase, 2=lowercase (applied emit-time to b->text, so .textContent — read from b->raw — stays original) */
    uint32_t curbg;                                      /* CSS background-color in effect (0=none, else 0x01000000|rgb) */
    uint32_t tokbg[TOK_MAX];                             /* per-token background colour (drawn as an inline highlight behind the text) */
    int      curalign;                                   /* text-align in effect: 0=left, 1=center, 2=right */
    uint8_t  tokalign[TOK_MAX];                          /* per-token text-align (a line takes its first token's value) */
    int      curscale;                                   /* CSS font-size scale override in effect (0=none/use style default, else 2 or 3) */
    uint8_t  tokscale[TOK_MAX];                           /* per-token glyph-scale override (0=none; enlarges only — the bitmap font has no sub-1x) */
    int      curindent;                                  /* left-indent in px (from <blockquote> nesting), applied at every line start */
    uint8_t  tokindent[TOK_MAX];                          /* per-token left-indent in px, capped at 255 (a line takes its first token's value) */
    char    *scripts; int scriptlen;                     /* inline <script> text captured this parse */
    int     bodyoff, bodylen;                            /* current page's body region in raw (for click-time JS re-render) */
    char    ls_keys[16][32]; char ls_vals[16][160]; int ls_n;   /* per-page localStorage (survives per-run JS arena resets) */
    char    oc_tag[16]; int oc_depth, oc_link, oc_style;        /* active inline-onclick scope (0 depth = none) */
    struct { char tag[16]; int depth; uint32_t savecolor, savebg; int savestyle, setstyle, saveul, savetransform, savealign, savescale, hidden; } sc[SC_MAX];  /* nested style scopes (color/bg/font-weight/font-style/underline/transform/align/font-size/display:none), a stack so nested styled elements compose */
    int     sc_sp;                                              /* number of active style frames (0 = none) */
    int     n_hidden;                                          /* >0 while inside a display:none element: suppress all emission */
    sel_t   css_sel[CSS_MAX]; uint32_t css_color[CSS_MAX]; int16_t css_style[CSS_MAX]; uint8_t css_ul[CSS_MAX]; uint8_t css_transform[CSS_MAX]; uint32_t css_bg[CSS_MAX]; uint8_t css_align[CSS_MAX]; uint8_t css_size[CSS_MAX]; uint8_t css_disp[CSS_MAX]; int n_css;  /* <style> rules: selector -> color / text-style / underline / text-transform / background / text-align / font-size / display:none */
    char    in_id[8][32]; char in_val[8][96]; int in_n;         /* <input> field values, by id (the typed/scripted text) */
    char    in_name[8][32];                                     /* each field's name= attr (parallel to in_id), for GET submit */
    char    focus_id[32];                                       /* id of the focused input field (empty = none) */
    char    form_action[URL_MAX];                               /* current <form action>; empty = submit to the current page */
};

static void drop_image(browser_t *b);        /* fwd: free any decoded image */
static void drop_image_slots(browser_t *b);  /* fwd: free inline images */
static void drop_remote_imgs(browser_t *b);  /* fwd: free prefetched remote-image bytes */
static int  decode_local_to_slot(browser_t *b, const char *path);  /* fwd */
static int  decode_bytes_to_slot(browser_t *b, const uint8_t *data, int len);  /* fwd: decode an in-memory image blob into an inline slot */
static int  is_chunked(const char *raw, int hdr_end);   /* fwd: chunked transfer-encoding? */
static int  dechunk(char *body, int len);               /* fwd: de-chunk an HTTP body in place */

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
    case STY_MARK: return 0x101015;          /* <mark>: dark text on a yellow highlight (set in the draw) */
    default:       return 0x202024;
    }
}
static int lineh_for(int style) { return style == STY_H1 ? 34 : style == STY_H2 ? 24 : 18; }
static int scale_for(int style) { return style == STY_H1 ? 2 : 1; }

/* ---- token emission ---- */
static void emit_word(browser_t *b, int start, int style, int link) {
    int len = b->textlen - start;
    if (len <= 0 || b->ntok >= TOK_MAX || b->n_hidden > 0) return;   /* display:none -> emit nothing */
    b->tokcolor[b->ntok] = b->curcolor;                  /* <font color> override (0 = none) */
    b->tokul[b->ntok] = (uint8_t)b->curul;               /* underline (text-decoration / <u>) */
    b->tokbg[b->ntok] = b->curbg;                        /* background-color highlight (0 = none) */
    b->tokalign[b->ntok] = (uint8_t)b->curalign;         /* text-align (0=left/1=center/2=right) */
    b->tokscale[b->ntok] = (uint8_t)b->curscale;         /* font-size scale override (0=none) */
    b->tokindent[b->ntok] = (uint8_t)(b->curindent > 255 ? 255 : b->curindent);   /* left-indent px */
    b->toks[b->ntok++] = (tok_t){ (uint16_t)start, (uint16_t)len,
                                  (uint16_t)link, (uint8_t)style, TK_WORD };
}
static void emit_break(browser_t *b, int type) {
    if (b->ntok == 0 || b->n_hidden > 0) return;         /* no leading blank lines / display:none */
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
/* Format an <ol> item number n in the given type into out[<max]: '1' decimal,
 * 'a'/'A' alphabetic (a,b,…,z,aa,…), 'i'/'I' roman (1–3999). */
static void fmt_li_num(int n, char fmt, char *out, int max) {
    int p = 0; if (n < 1) n = 1; if (max < 2) { if (max > 0) out[0] = 0; return; }
    if (fmt == 'a' || fmt == 'A') {
        char base = (fmt == 'A') ? 'A' : 'a'; char tmp[8]; int k = 0;
        while (n > 0 && k < 7) { n--; tmp[k++] = (char)(base + n % 26); n /= 26; }
        while (k && p < max - 1) out[p++] = tmp[--k];
    } else if (fmt == 'i' || fmt == 'I') {
        static const int vals[] = {1000,900,500,400,100,90,50,40,10,9,5,4,1};
        static const char *syms[] = {"m","cm","d","cd","c","xc","l","xl","x","ix","v","iv","i"};
        if (n > 3999) n = 3999;
        for (int i = 0; i < 13 && p < max - 1; i++)
            while (n >= vals[i] && p < max - 1) { for (const char *s = syms[i]; *s && p < max - 1; s++) out[p++] = (fmt == 'I') ? (char)(*s - 32) : *s; n -= vals[i]; }
    } else {
        char tmp[12]; int k = 0; int t = n; while (t && k < 11) { tmp[k++] = (char)('0' + t % 10); t /= 10; }
        while (k && p < max - 1) out[p++] = tmp[--k];
    }
    out[p] = 0;
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
/* Store a "det:N" link so following it toggles <details> index N open/closed. */
static int add_det_link(browser_t *b, int idx) {
    char buf[8]; int p = 0; const char *pfx = "det:"; while (pfx[p]) { buf[p] = pfx[p]; p++; }
    if (idx >= 10) buf[p++] = (char)('0' + idx / 10);
    buf[p++] = (char)('0' + idx % 10); buf[p] = 0;
    if (b->nlink >= LINK_MAX || b->hreflen + p >= HREF_MAX) return NO_LINK;
    int off = b->hreflen;
    for (int i = 0; i < p; i++) b->hrefs[b->hreflen++] = buf[i];
    b->links[b->nlink] = (href_t){ (uint16_t)off, (uint16_t)p };
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
/* Store an "event:ID" link; activating it (browser_follow) calls js_fire_event(ID,
 * "click") to run a JS-assigned handler (el.onclick=fn / addEventListener). The
 * element gets one via a synthetic data-jsh attribute the handler-registration wrote. */
static int add_event_link(browser_t *b, const char *id, int idlen) {
    const char *pfx = "event:"; int pl = 6;
    if (idlen <= 0 || b->nlink >= LINK_MAX || b->hreflen + pl + idlen >= HREF_MAX) return NO_LINK;
    int off = b->hreflen;
    for (int i = 0; i < pl; i++) b->hrefs[b->hreflen++] = pfx[i];
    for (int i = 0; i < idlen; i++) b->hrefs[b->hreflen++] = id[i];
    b->links[b->nlink] = (href_t){ (uint16_t)off, (uint16_t)(pl + idlen) };
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
    if (vl >= 4 && lc(v[0])=='r' && lc(v[1])=='g' && lc(v[2])=='b') {   /* rgb(r,g,b) / rgba(r,g,b,a) — alpha ignored */
        int i = 3; if (i < vl && lc(v[i])=='a') i++;          /* skip the 'a' of rgba */
        while (i < vl && v[i] != '(') i++;
        if (i >= vl) return 0;
        i++;                                                  /* past '(' */
        int comp[3], nc = 0;
        while (i < vl && nc < 3) {
            while (i < vl && (v[i]==' '||v[i]==',')) i++;      /* skip separators */
            if (i >= vl || v[i] < '0' || v[i] > '9') break;   /* no number here (')' / '%'-only / junk) */
            int val = 0;
            while (i < vl && v[i] >= '0' && v[i] <= '9') { val = val*10 + (v[i]-'0'); if (val > 1000) val = 1000; i++; }
            if (i < vl && v[i]=='%') { val = val * 255 / 100; i++; }   /* percentage component */
            if (val > 255) val = 255;
            comp[nc++] = val;
            while (i < vl && v[i] != ',' && v[i] != ')') i++; /* to the next separator */
        }
        if (nc >= 3) return 0x01000000u | ((uint32_t)comp[0]<<16) | ((uint32_t)comp[1]<<8) | (uint32_t)comp[2];
        return 0;
    }
    static const struct { const char *n; uint32_t rgb; } named[] = {
        {"red",0xCC0000},{"green",0x008000},{"blue",0x0000CC},{"black",0x000000},
        {"gray",0x808080},{"grey",0x808080},{"silver",0xC0C0C0},{"orange",0xE07000},
        {"yellow",0xB8A000},{"purple",0x800080},{"navy",0x000080},{"teal",0x008080},
        {"maroon",0x800000},{"olive",0x808000},{"lime",0x00A000},{"cyan",0x008B8B},
        {"magenta",0xB000B0},{"brown",0x8B4513},{"pink",0xD06080},
        {"crimson",0xC0143C},{"gold",0xB8860B},{"indigo",0x4B0082},{"violet",0x8A2BE2},
        {"coral",0xD0522D},{"salmon",0xC0593B},{"turquoise",0x209888},{"tan",0x8B7355},
        {"darkred",0x8B0000},{"darkblue",0x00008B},{"darkgreen",0x006400},{"darkgray",0x595959},
        {"darkgrey",0x595959},{"slategray",0x5A6A78},{"slategrey",0x5A6A78},{"steelblue",0x3672A0},
    };
    char buf[16]; int p = 0;
    for (int i = 0; i < vl && p < 15; i++) buf[p++] = (char)lc(v[i]);
    buf[p] = 0;
    for (unsigned i = 0; i < sizeof(named)/sizeof(named[0]); i++)
        if (streqs(buf, named[i].n)) return 0x01000000u | named[i].rgb;
    return 0;
}
/* Extract the `color:` value from an inline style="..." string and parse it (a tiny
 * CSS subset). Skips `background-color:` by requiring a property boundary before `color`. */
static uint32_t parse_style_color(const char *s, int n) {
    for (int i = 0; i + 6 <= n; i++) {
        if (lc(s[i])=='c' && lc(s[i+1])=='o' && lc(s[i+2])=='l' && lc(s[i+3])=='o' && lc(s[i+4])=='r' && s[i+5]==':') {
            char before = (i > 0) ? s[i-1] : ' ';   /* must start a property, not be the tail of e.g. background-color */
            if (before==' ' || before==';' || before=='\t' || before=='\n' || before=='"' || before=='\'') {
                int k = i + 6; while (k < n && (s[k]==' '||s[k]=='\t')) k++;     /* skip ws after ':' */
                int vs = k; while (k < n && s[k] != ';' && s[k] != '}') k++;     /* value to ';' or end */
                int ve = k; while (ve > vs && (s[ve-1]==' '||s[ve-1]=='\t')) ve--;  /* trim trailing ws */
                return parse_color(s + vs, ve - vs);
            }
        }
    }
    return 0;
}
/* Find a lowercase CSS property `prop` (length plen) at a property boundary in an inline
 * style string and return its trimmed value span [*vs,*ve); 1 if found. Bounded read-only. */
static int style_prop(const char *s, int n, const char *prop, int plen, int *vs, int *ve) {
    for (int i = 0; i + plen + 1 <= n; i++) {                /* room for prop + ':' */
        int m = 1;
        for (int j = 0; j < plen; j++) if (lc(s[i+j]) != prop[j]) { m = 0; break; }
        if (!m || s[i+plen] != ':') continue;
        char before = (i > 0) ? s[i-1] : ' ';                /* must start a property (not a hyphen-suffix like -weight) */
        if (!(before==' '||before==';'||before=='\t'||before=='\n'||before=='"'||before=='\'')) continue;
        int k = i + plen + 1; while (k < n && (s[k]==' '||s[k]=='\t')) k++;   /* ws after ':' */
        int a = k; while (k < n && s[k] != ';' && s[k] != '}') k++;          /* value to ';' or end */
        int e = k; while (e > a && (s[e-1]==' '||s[e-1]=='\t')) e--;          /* trim trailing ws */
        *vs = a; *ve = e; return 1;
    }
    return 0;
}
/* Map an inline style's font-weight/font-style to the renderer's text-style enum:
 * font-weight bold/bolder/600..900 -> STY_BOLD; font-style italic/oblique -> STY_EM; else -1.
 * (The renderer's style is a single enum, so bold takes precedence when both are set.) */
static int parse_style_textstyle(const char *s, int n) {
    int vs, ve;
    if (style_prop(s, n, "font-weight", 11, &vs, &ve)) {
        const char *v = s + vs; int vl = ve - vs;
        if (attr_eq(v, vl, "bold") || attr_eq(v, vl, "bolder") ||
            (vl >= 3 && v[0] >= '6' && v[0] <= '9'))         /* 600/700/800/900 */
            return STY_BOLD;
    }
    if (style_prop(s, n, "font-style", 10, &vs, &ve)) {
        const char *v = s + vs; int vl = ve - vs;
        if (attr_eq(v, vl, "italic") || attr_eq(v, vl, "oblique")) return STY_EM;
    }
    return -1;
}
/* text-decoration / text-decoration-line: underlined if its value contains "underline". */
static int parse_style_underline(const char *s, int n) {
    int vs, ve;
    if (!style_prop(s, n, "text-decoration", 15, &vs, &ve) &&
        !style_prop(s, n, "text-decoration-line", 20, &vs, &ve)) return 0;
    for (int i = vs; i + 9 <= ve; i++)                     /* "underline" is 9 chars; i+8 < ve <= n */
        if (lc(s[i])=='u'&&lc(s[i+1])=='n'&&lc(s[i+2])=='d'&&lc(s[i+3])=='e'&&lc(s[i+4])=='r'&&
            lc(s[i+5])=='l'&&lc(s[i+6])=='i'&&lc(s[i+7])=='n'&&lc(s[i+8])=='e') return 1;
    return 0;
}
/* text-transform: 1 = uppercase, 2 = lowercase, 0 = none/unsupported (capitalize is left as-is). */
static int parse_style_transform(const char *s, int n) {
    int vs, ve;
    if (!style_prop(s, n, "text-transform", 14, &vs, &ve)) return 0;
    const char *v = s + vs; int vl = ve - vs;
    if (attr_eq(v, vl, "uppercase")) return 1;
    if (attr_eq(v, vl, "lowercase")) return 2;
    return 0;
}
/* background-color / background (shorthand): the colour drawn behind the text (0 = none).
 * `background` (10) matches only the shorthand (`background:` — the `:` boundary means it
 * never matches `background-color`), and parse_color reads the leading colour token, so a
 * `background: #fff url(..)` shorthand still yields #fff. */
static uint32_t parse_style_bg(const char *s, int n) {
    int vs, ve;
    if (style_prop(s, n, "background-color", 16, &vs, &ve) ||
        style_prop(s, n, "background", 10, &vs, &ve))
        return parse_color(s + vs, ve - vs);
    return 0;
}
/* text-align: 1 = center, 2 = right, 0 = left/justify/other (the default flow). */
static int parse_style_align(const char *s, int n) {
    int vs, ve;
    if (!style_prop(s, n, "text-align", 10, &vs, &ve)) return 0;
    const char *v = s + vs; int vl = ve - vs;
    if (attr_eq(v, vl, "center")) return 1;
    if (attr_eq(v, vl, "right"))  return 2;
    return 0;
}
/* font-size -> a glyph-scale bucket: 3 (≈ ≥28px / ≥175% / ≥1.8em), 2 (≈ ≥19px / ≥119% /
 * ≥1.2em), else 0 (no override). The bitmap font has no sub-1x, so this only enlarges;
 * small sizes just fall through to the default 1x. Reads the leading integer of the value. */
static int parse_style_fontsize(const char *s, int n) {
    int vs, ve;
    if (!style_prop(s, n, "font-size", 9, &vs, &ve)) return 0;
    const char *v = s + vs; int vl = ve - vs, i = 0, num = 0, seen = 0;
    while (i < vl && v[i] >= '0' && v[i] <= '9') { num = num*10 + (v[i]-'0'); i++; seen = 1; }
    if (!seen) {                                            /* keywords: large/x-large/… */
        if (attr_eq(v, vl, "large") || attr_eq(v, vl, "larger")) return 2;
        if (attr_eq(v, vl, "x-large") || attr_eq(v, vl, "xx-large")) return 3;
        return 0;
    }
    if (i < vl && v[i] == '.') { i++; while (i < vl && v[i] >= '0' && v[i] <= '9') i++; }  /* skip any fraction */
    /* unit: % / em / rem / px / pt (default ~px). em/rem are relative to 1; % to 100. */
    const char *u = v + i; int ul = vl - i;
    if (ul >= 1 && u[0] == '%')                                          return num >= 175 ? 3 : num >= 119 ? 2 : 0;
    if (ul >= 2 && (u[0]|32)=='e' && (u[1]|32)=='m')                     return num >= 2 ? 3 : num >= 1 ? 2 : 0;
    if (ul >= 3 && (u[0]|32)=='r' && (u[1]|32)=='e' && (u[2]|32)=='m')   return num >= 2 ? 3 : num >= 1 ? 2 : 0;
    return num >= 28 ? 3 : num >= 19 ? 2 : 0;                            /* px / pt / unitless */
}
/* display: returns 1 for "display:none" (the element + its content are hidden). */
static int parse_style_display(const char *s, int n) {
    int vs, ve;
    if (!style_prop(s, n, "display", 7, &vs, &ve)) return 0;
    return attr_eq(s + vs, ve - vs, "none");
}

/* void (self-closing) elements have no close tag, so they can't open an onclick scope */
static int is_void_tag(const char *t) {
    return tageq(t,"input")||tageq(t,"img")||tageq(t,"br")||tageq(t,"hr")||tageq(t,"meta")||
           tageq(t,"link")||tageq(t,"area")||tageq(t,"col")||tageq(t,"base")||tageq(t,"wbr")||
           tageq(t,"embed")||tageq(t,"source");
}
/* <style> support (defined after sel_parse): parse a <style> body into b->css_* rules,
 * and find the cascaded color/text-style for one element from those rules. */
static void capture_css(browser_t *b, const char *s, int n);
static int  css_match(browser_t *b, const char *tag, const char *attrs, int attrlen,
                      uint32_t *color, int *textstyle, int *underline, int *transform, uint32_t *bg,
                      int *align, int *size, int *hidden);
static void handle_tag(browser_t *b, const char *tag, int closing,
                       const char *attrs, int attrlen,
                       int *style, int *linkdepth, int *curlink) {
    /* inline onclick="CODE": make the element's content a clickable javascript: link,
     * scoped to the element (depth-counted so nested same-name tags don't end it early). */
    if (b->oc_depth > 0 && tageq(tag, b->oc_tag)) {
        if (closing) { if (--b->oc_depth == 0) { *curlink = b->oc_link; if (*style == STY_LINK) *style = b->oc_style; } }
        else b->oc_depth++;
    }
    /* style scope STACK (color / font-weight / font-style from inline style= and <style> rules).
     * A styled element pushes a frame; its matching close pops it (restoring colour + text-style),
     * so nested styled elements compose. Same-tag nesting is depth-counted per frame; the restore
     * mirrors the oc_* link scope (only if *style is still the one we set). */
    if (closing) {
        if (b->sc_sp > 0 && tageq(tag, b->sc[b->sc_sp-1].tag)) {
            if (--b->sc[b->sc_sp-1].depth == 0) {
                int sp = --b->sc_sp;
                b->curcolor = b->sc[sp].savecolor;
                b->curbg = b->sc[sp].savebg;
                b->curul = b->sc[sp].saveul;
                b->curtransform = b->sc[sp].savetransform;
                b->curalign = b->sc[sp].savealign;
                b->curscale = b->sc[sp].savescale;
                if (b->sc[sp].hidden && b->n_hidden > 0) b->n_hidden--;   /* leaving a display:none element */
                if (b->sc[sp].setstyle >= 0 && *style == b->sc[sp].setstyle) *style = b->sc[sp].savestyle;
            }
        }
    } else if (!is_void_tag(tag)) {
        uint32_t c = 0; int ts = -1, ul = 0, tr = 0; uint32_t bg = 0; int al = 0, fs = 0, hide = 0;
        if (b->n_css > 0) css_match(b, tag, attrs, attrlen, &c, &ts, &ul, &tr, &bg, &al, &fs, &hide);   /* <style> rules first (lower priority) */
        const char *st; int stl;
        if (find_attr(attrs, attrlen, "style", &st, &stl)) {           /* inline style overrides per-property (cascade) */
            uint32_t ic = parse_style_color(st, stl);  if (ic) c = ic;
            int its = parse_style_textstyle(st, stl);  if (its >= 0) ts = its;
            if (parse_style_underline(st, stl)) ul = 1;
            int itr = parse_style_transform(st, stl);  if (itr) tr = itr;   /* text-transform (inline only) */
            uint32_t ibg = parse_style_bg(st, stl);    if (ibg) bg = ibg;   /* background-color */
            int ial = parse_style_align(st, stl);      if (ial) al = ial;   /* text-align */
            int ifs = parse_style_fontsize(st, stl);   if (ifs) fs = ifs;   /* font-size (enlarge) */
            if (parse_style_display(st, stl)) hide = 1;                      /* display:none */
        }
        if (has_attr(attrs, attrlen, "hidden")) hide = 1;   /* the HTML5 `hidden` attribute */
        if (tageq(tag, "big")) { if (!fs) fs = 2; }         /* <big> -> 2x */
        if (tageq(tag, "font")) {                           /* <font size=N>: 5+ -> 2x, 6+/7 -> 3x */
            const char *sv; int svl;
            if (find_attr(attrs, attrlen, "size", &sv, &svl) && svl > 0) {
                int neg = (sv[0]=='+'||sv[0]=='-'), d = (svl>neg) ? sv[neg]-'0' : 0;
                if (sv[0] != '-' && d >= 6) fs = 3; else if (sv[0] != '-' && d >= 4) fs = 2;
            }
        }
        if (tageq(tag, "center")) al = 1;                   /* the legacy <center> tag */
        { const char *av; int avl;                          /* and the legacy align= attribute (div, p, headings, td) */
          if (find_attr(attrs, attrlen, "align", &av, &avl)) {
              if (attr_eq(av, avl, "center")) al = 1; else if (attr_eq(av, avl, "right")) al = 2; } }
        if (tageq(tag, "u") || tageq(tag, "ins")) ul = 1;   /* the <u>/<ins> tags also underline */
        int apply_ts = (ts >= 0 && *style == STY_NORMAL);   /* like <b>/<i>: only over normal-flow text */
        if (c || apply_ts || ul || tr || bg || al || fs || hide) {  /* styled/hidden element -> push a frame */
            if (b->sc_sp < SC_MAX) {
                int sp = b->sc_sp;
                b->sc[sp].hidden = hide; if (hide) b->n_hidden++;   /* enter a display:none subtree */
                b->sc[sp].savecolor = b->curcolor; if (c) b->curcolor = c;
                b->sc[sp].savebg = b->curbg; if (bg) b->curbg = bg;
                b->sc[sp].saveul = b->curul; if (ul) b->curul = 1;
                b->sc[sp].savetransform = b->curtransform; if (tr) b->curtransform = tr;
                b->sc[sp].savealign = b->curalign; if (al) b->curalign = al;
                b->sc[sp].savescale = b->curscale; if (fs) b->curscale = fs;
                b->sc[sp].savestyle = *style; b->sc[sp].setstyle = -1;
                if (apply_ts) { *style = ts; b->sc[sp].setstyle = ts; }
                int i = 0; while (tag[i] && i < 15) { b->sc[sp].tag[i] = tag[i]; i++; } b->sc[sp].tag[i] = 0;
                b->sc[sp].depth = 1;
                b->sc_sp++;
            }   /* stack full: skip (no scope) — graceful, never overflows */
        } else if (b->sc_sp > 0 && tageq(tag, b->sc[b->sc_sp-1].tag)) {
            b->sc[b->sc_sp-1].depth++;                       /* unstyled same-tag nesting of the top frame */
        }
    }
    if (!closing && b->oc_depth == 0 && !is_void_tag(tag)) {
        const char *oc; int ocl;
        int lk = NO_LINK;
        if (find_attr(attrs, attrlen, "onclick", &oc, &ocl)) {
            lk = add_onclick(b, oc, ocl);              /* inline handler: scope the element to a javascript: link */
        } else if (find_attr(attrs, attrlen, "data-jsh", &oc, &ocl)) {   /* el.onclick=fn marker: clickable -> event:ID link (id-keyed registry) */
            const char *idv; int idl;
            if (find_attr(attrs, attrlen, "id", &idv, &idl) && idl > 0) lk = add_event_link(b, idv, idl);
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
    if (!closing && b->anc_n < 32) {                     /* record id (or <a name>) -> token index for #fragment scroll-to */
        const char *idv; int idl;
        if ((find_attr(attrs, attrlen, "id", &idv, &idl) ||                     /* modern: any element's id */
             (tageq(tag, "a") && find_attr(attrs, attrlen, "name", &idv, &idl)))  /* legacy: <a name="..."> */
            && idl > 0 && idl < 32) {
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
    if (tageq(tag, "i") || tageq(tag, "em") || tageq(tag, "cite") ||      /* italic-by-convention */
        tageq(tag, "var") || tageq(tag, "dfn") || tageq(tag, "address")) {
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
    if (tageq(tag, "mark")) {                                             /* highlighted text */
        if (!closing) { if (*style == STY_NORMAL) *style = STY_MARK; }
        else if (*style == STY_MARK) *style = STY_NORMAL;
        return;
    }
    if (tageq(tag, "sub") || tageq(tag, "sup")) {                         /* sub/superscript (drawn at a y-offset) */
        int st = tageq(tag, "sub") ? STY_SUB : STY_SUP;
        if (!closing) { if (*style == STY_NORMAL) *style = st; }
        else if (*style == st) *style = STY_NORMAL;
        return;
    }
    if (tageq(tag, "font")) {                            /* <font color="..."> text colour */
        if (!closing) {
            const char *v; int vl;
            if (find_attr(attrs, attrlen, "color", &v, &vl)) { uint32_t c = parse_color(v, vl); if (c) b->curcolor = c; }
        } else b->curcolor = 0;
        return;
    }
    if (tageq(tag, "hr")) { if (b->ntok < TOK_MAX && b->n_hidden == 0) b->toks[b->ntok++] = (tok_t){0,0,NO_LINK,STY_NORMAL,TK_HR}; return; }
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
                    int slot = b->n_hidden ? -1 : decode_local_to_slot(b, path);   /* display:none -> don't show */
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
                if (!shown && !b->loading) {
                    /* A remote src the worker prefetched (matched by raw src string)
                     * decodes inline, like a local one; the compressed bytes were
                     * stashed in rimg_data[] pre-parse. Gated on !loading: the worker
                     * mutates rimg_* (drop+repopulate) only while loading==1, so reading
                     * them here on the WM thread when !loading is a DIRECT guard against
                     * a free/use race (not relying on the indirect "render triggers are
                     * disabled during load" invariant). */
                    for (int k = 0; k < b->n_rimg && !shown; k++) {
                        if (!b->rimg_data[k]) continue;
                        int m = 1;
                        for (int i = 0; i < srcl; i++) { if (b->rimg_src[k][i] != src[i]) { m = 0; break; } }
                        if (!m || b->rimg_src[k][srcl] != 0) continue;   /* exact length+bytes */
                        int slot = b->n_hidden ? -1 : decode_bytes_to_slot(b, b->rimg_data[k], b->rimg_len[k]);
                        if (slot >= 0 && b->ntok + 2 < TOK_MAX) {
                            int aw = attr_int(attrs, attrlen, "width");
                            int ah = attr_int(attrs, attrlen, "height");
                            emit_break(b, TK_BREAK);
                            b->toks[b->ntok++] = (tok_t){ (uint16_t)aw, (uint16_t)ah, (uint16_t)slot, STY_NORMAL, TK_IMG };
                            emit_break(b, TK_BREAK);
                            shown = 1;
                        }
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
            if (b->listdepth < 8) {
                b->listtype[b->listdepth] = tag[0];
                char fmt = '1'; const char *tv; int tvl;                       /* <ol type=…> marker style */
                if (tag[0] == 'o' && find_attr(attrs, attrlen, "type", &tv, &tvl) && tvl >= 1
                    && (tv[0]=='a'||tv[0]=='A'||tv[0]=='i'||tv[0]=='I'||tv[0]=='1')) fmt = tv[0];
                b->listfmt[b->listdepth] = fmt;
                int st = (tag[0] == 'o') ? attr_int(attrs, attrlen, "start") : 0;   /* <ol start=N> */
                b->listnum[b->listdepth] = st > 0 ? st - 1 : 0;
                b->listdepth++;
            }
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
                int n = ++b->listnum[b->listdepth - 1];        /* numbered item, formatted per <ol type> */
                char num[16]; fmt_li_num(n, b->listfmt[b->listdepth - 1], num, sizeof(num));
                for (int k = 0; num[k] && p < 22; k++) marker[p++] = num[k];
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
    if (tageq(tag, "blockquote")) {                      /* indent the quoted block (incl. wrapped lines) */
        emit_break(b, TK_PARA);
        if (!closing) { if (b->curindent < 200) b->curindent += 24; }
        else { b->curindent -= 24; if (b->curindent < 0) b->curindent = 0; }
        return;
    }
    if (tageq(tag, "div") || tageq(tag, "section") ||
        tageq(tag, "article") || tageq(tag, "header") || tageq(tag, "footer") ||
        tageq(tag, "nav") || tageq(tag, "pre") || tageq(tag, "dl") ||
        tageq(tag, "main"))
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

/* ---- <table> rendering: monospace column-aligned rows --------------------- *
 * A <table> is rendered self-contained: render_table consumes the whole
 * <table>..</table> region and emits cells padded (with spaces) to each column's
 * max width, so the fixed-width font lays them out as aligned columns. It does
 * its own bounded parsing of the region and never touches non-table content. */
#define TBL_COLS     16
#define TBL_CELL_MAX 64
/* Classify a tag at p: 1=<tr> 2=<td> 6=<th> 3=</tr> 4=</table> 5=</td>|</th> 0=other.
 * *after := just past '>'. All reads bounded by e. */
static int tbl_classify(const char *p, const char *e, const char **after) {
    const char *q = p + 1; int cl = 0;
    if (q < e && *q == '/') { cl = 1; q++; }
    char nm[8]; int nl = 0;
    while (q < e && nl < 7) { int ch = lc((unsigned char)*q); if (ch < 'a' || ch > 'z') break; nm[nl++] = (char)ch; q++; }
    const char *r = q; while (r < e && *r != '>') r++;
    *after = (r < e) ? r + 1 : e;
    if (nl == 2 && nm[0]=='t' && nm[1]=='r') return cl ? 3 : 1;
    if (nl == 2 && nm[0]=='t' && nm[1]=='d') return cl ? 5 : 2;
    if (nl == 2 && nm[0]=='t' && nm[1]=='h') return cl ? 5 : 6;
    if (nl == 5 && nm[0]=='t'&&nm[1]=='a'&&nm[2]=='b'&&nm[3]=='l'&&nm[4]=='e') return cl ? 4 : 0;
    return 0;
}
/* Extract a cell's visible text into out (capped): strip inline tags, decode entities,
 * fold non-ASCII, collapse runs of whitespace to one space. Returns the char length. */
static int cell_extract(const char *s, const char *e, char *out, int cap) {
    int n = 0, sp = 0;
    const char *p = s;
    while (p < e && n < cap - 1) {
        if (*p == '<') { while (p < e && *p != '>') p++; if (p < e) p++; continue; }
        char c;
        if (*p == '&') { char d; int adv = decode_entity(p, (int)(e - p), &d); if (adv) { c = d; p += adv; } else { c = *p; p++; } }
        else if ((unsigned char)*p >= 0x80) { unsigned cp; int adv = decode_utf8(p, (int)(e - p), &cp); c = uni_to_ascii(cp); p += (adv > 0 ? adv : 1); }
        else { c = *p; p++; }
        if (c==' '||c=='\t'||c=='\n'||c=='\r') { sp = 1; continue; }
        if (sp && n > 0 && n < cap - 1) out[n++] = ' ';
        sp = 0;
        if (n < cap - 1) out[n++] = c;
    }
    out[n] = 0;
    return n;
}
/* Render the table region [s, s+len). Returns chars consumed (to just past </table>). */
static int render_table(browser_t *b, const char *s, int len) {
    if (len <= 0) return 0;
    const char *e = s + len, *tend = e;
    int col_w[TBL_COLS]; for (int i = 0; i < TBL_COLS; i++) col_w[i] = 0;
    char out[TBL_CELL_MAX];

    /* pass 1: measure each column's max cell width */
    { const char *p = s; int col = 0, guard = 0;
      while (p < e && guard++ < 200000) {
        if (*p != '<') { p++; continue; }
        const char *after; int k = tbl_classify(p, e, &after);
        if (k == 4) { tend = after; break; }
        if (k == 1) { col = 0; p = after; continue; }
        if (k == 2 || k == 6) {
            const char *cs = after, *cend = cs;
            while (cend < e) { if (*cend == '<') { const char *aa; if (tbl_classify(cend, e, &aa)) break; } cend++; }
            int n = cell_extract(cs, cend, out, TBL_CELL_MAX);
            if (col < TBL_COLS && n > col_w[col]) col_w[col] = n;
            col++; p = cend; continue;
        }
        p = after;
      }
    }
    /* pass 2: emit each cell padded to its column width (fixed-width font => aligned) */
    emit_break(b, TK_PARA);
    { const char *p = s; int col = 0, guard = 0;
      while (p < e && guard++ < 200000) {
        if (*p != '<') { p++; continue; }
        const char *after; int k = tbl_classify(p, e, &after);
        if (k == 4) break;
        if (k == 1) { emit_break(b, TK_BREAK); col = 0; p = after; continue; }
        if (k == 2 || k == 6) {
            const char *cs = after, *cend = cs;
            while (cend < e) { if (*cend == '<') { const char *aa; if (tbl_classify(cend, e, &aa)) break; } cend++; }
            int n = cell_extract(cs, cend, out, TBL_CELL_MAX);
            int w = (col < TBL_COLS) ? col_w[col] : n;
            char padded[TBL_CELL_MAX + 2]; int m = 0;
            for (int i = 0; i < n && m < TBL_CELL_MAX; i++) padded[m++] = out[i];
            while (m < w && m < TBL_CELL_MAX) padded[m++] = ' ';
            padded[m] = 0;
            emit_literal(b, padded, (k == 6) ? STY_BOLD : STY_NORMAL);
            col++; p = cend; continue;
        }
        p = after;
      }
    }
    emit_break(b, TK_PARA);
    return (tend < e) ? (int)(tend - s) : len;
}

static void parse_html(browser_t *b, const char *body, int len) {
    drop_image(b);                                       /* a page replaces any prior image */
    drop_image_slots(b);                                 /* and its inline images */
    b->textlen = b->ntok = b->hreflen = b->nlink = 0;
    b->scriptlen = 0;                                    /* recaptured fresh each parse */
    b->oc_depth = 0;                                     /* no inline-onclick scope open yet */
    b->sc_sp = 0;                                        /* no style scopes open yet */
    b->n_hidden = 0;                                     /* nothing hidden yet */
    b->n_css = 0;                                        /* <style> rules captured fresh each parse */
    b->form_action[0] = 0;                               /* no <form> action open yet */
    b->anc_n = 0;                                        /* fresh #fragment anchor table */
    b->sel = NO_LINK;                                    /* no link selected on a fresh page */
    b->find_tok = -1;                                    /* clear any find highlight */
    b->curcolor = 0;                                     /* default text colour */
    b->curbg = 0;                                        /* no background-color in effect */
    b->curalign = 0;                                     /* default left alignment */
    b->curscale = 0;                                     /* no font-size override in effect */
    b->curindent = 0;                                    /* no blockquote indent in effect */
    b->curul = 0;                                        /* no underline in effect */
    b->curtransform = 0;                                 /* no text-transform in effect */
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
    int st_start = -1;                                   /* offset where current <style> body began */
    int det_n = 0, det_depth = 0, det_hide = 0, in_summary = 0, det_cur = 0;   /* <details>: index / nesting / suppress-depth / in-<summary> / current idx */
    int sum_link = NO_LINK, sum_style = STY_NORMAL;      /* saved link/style around a <summary> */

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
            else if (tageq(tag, "style")) {
                if (!closing) { instyle = 1; st_start = j + 1; }          /* body starts after '>' */
                else { if (instyle && st_start >= 0 && i > st_start) capture_css(b, body + st_start, i - st_start);
                       instyle = 0; st_start = -1; }
            }
            else if (tageq(tag, "svg")) insvg = !closing;        /* inline SVG: skip its guts */
            else if (tageq(tag, "title") && !insvg) intitle = !closing;  /* (svg <title> mustn't hijack) */
            else if (tageq(tag, "head")) inhead = !closing;
            else if (tageq(tag, "body")) inhead = intitle = 0;   /* visible content */
            else if (tageq(tag, "pre")) {                        /* preformatted block */
                inpre = !closing;
                emit_break(b, TK_PARA);                          /* pre starts/ends on its own line */
            }
            else if (tageq(tag, "details")) {                    /* collapsible: hide the body when closed */
                if (!closing) {
                    int idx = det_n < 16 ? det_n : 15; if (det_n < 16) det_n++;   /* cap idx so det_open[idx] stays in bounds */
                    if (b->det_open[idx] == 0xFF) b->det_open[idx] = has_attr(body + astart, j - astart, "open") ? 1 : 0;
                    det_depth++; det_cur = idx;
                    if (!b->det_open[idx] && !det_hide) det_hide = det_depth;     /* start hiding this closed details' body */
                } else { if (det_depth == det_hide) det_hide = 0; if (det_depth > 0) det_depth--; }
                emit_break(b, TK_PARA);
            }
            else if (tageq(tag, "summary")) {                    /* the always-shown, clickable toggle */
                if (!closing && det_depth > 0) {
                    in_summary = 1; sum_link = curlink; sum_style = style;
                    int lk = add_det_link(b, det_cur);
                    if (lk != NO_LINK) { curlink = lk; if (style == STY_NORMAL) style = STY_LINK;
                        emit_literal_link(b, b->det_open[det_cur] ? "[-] " : "[+] ", lk); }
                } else if (closing && in_summary) { in_summary = 0; curlink = sum_link; style = sum_style; }
            }
            else if (tageq(tag, "table") && !closing &&
                     !inscript && !instyle && !intitle && !inhead && !insvg && !(det_hide && !in_summary)) {
                /* render the whole <table>..</table> region as aligned columns, then skip past it */
                int consumed = render_table(b, body + j + 1, len - (j + 1));
                i = j + consumed;                         /* loop ++ lands just past </table> */
                continue;
            }
            else if (!inscript && !instyle && !intitle && !inhead && !insvg && !(det_hide && !in_summary))
                handle_tag(b, tag, closing, body + astart, j - astart,
                           &style, &linkdepth, &curlink);

            i = j;                                        /* loop ++ steps past '>' */
            continue;
        }
        if (inscript || instyle || insvg) continue;  /* never render script/style/svg */
        if (det_hide && !in_summary) continue;        /* inside a collapsed <details>, outside its <summary> */
        if (b->n_hidden > 0) continue;                /* inside a display:none element (tags still tracked above) */

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
        if (b->curtransform) {                    /* CSS text-transform: case-fold the RENDERED char (goes into b->text only; .textContent reads b->raw, so it stays original) */
            if (b->curtransform == 1 && c >= 'a' && c <= 'z') c -= 32;
            else if (b->curtransform == 2 && c >= 'A' && c <= 'Z') c += 32;
        }
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
/* ---- querySelector(All): a tiny CSS-selector matcher over the page source ----
 * Supports simple selectors: a tag name, ".class", "#id", and compounds like
 * "div.note" / "p#x". Matches are reported as the byte offset of each opening
 * '<' in b->raw, which dom_find_at() then resolves to an inner-text span (the
 * same byte coordinates the id-keyed splice code already uses) — so id-less
 * matches become addressable WITHOUT changing the id path. */
#define QSA_MAX 256
static int sel_parse(const char *s, sel_t *o) {
    o->tag[0]=o->cls[0]=o->id[0]=o->attr[0]=0;
    int i=0, k=0;
    while (s[i] && dom_alnum(s[i]) && k<15) { o->tag[k++]=(char)lc(s[i]); i++; }   /* leading tag name (lowercased) */
    o->tag[k]=0;
    while (s[i]) {
        if (s[i]=='.')      { i++; k=0; while (s[i] && (dom_alnum(s[i])||s[i]=='-'||s[i]=='_') && k<31) o->cls[k++]=s[i++]; o->cls[k]=0; }
        else if (s[i]=='#') { i++; k=0; while (s[i] && (dom_alnum(s[i])||s[i]=='-'||s[i]=='_') && k<31) o->id[k++]=s[i++];  o->id[k]=0;  }
        else if (s[i]=='[') { i++; k=0; while (s[i] && s[i]!=']' && s[i]!='=' && k<31) o->attr[k++]=(char)lc(s[i++]); o->attr[k]=0;   /* [attr] presence (value, if any, ignored) */
                              while (s[i] && s[i]!=']') i++; if (s[i]==']') i++; }
        else return 0;   /* an unsupported combinator/char -> fail closed (no match) */
    }
    return (o->tag[0]||o->cls[0]||o->id[0]||o->attr[0]);
}
/* Word-boundary class match within a class="..." value (space-separated tokens). */
static int class_has(const char *v, int vl, const char *cls) {
    int cl=0; while (cls[cl]) cl++; if (cl==0) return 0;
    for (int i=0; i+cl<=vl; i++) {
        if (i>0 && v[i-1]!=' ' && v[i-1]!='\t') continue;
        int m=0; while (m<cl && v[i+m]==cls[m]) m++;
        if (m==cl && (i+cl==vl || v[i+cl]==' ' || v[i+cl]=='\t')) return 1;
    }
    return 0;
}
/* Parse a <style> body into simple `selector { color / font-weight / font-style }` rules
 * (b->css_*). A selector that isn't a single simple selector (descendant, comma-grouped,
 * @-rule) fails sel_parse and is skipped. Bounded read-only over s[0..n); caps at CSS_MAX. */
static void capture_css(browser_t *b, const char *s, int n) {
    int i = 0;
    while (i < n && b->n_css < CSS_MAX) {
        while (i < n && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r'||s[i]==';'||s[i]=='}')) i++;  /* ws / stray ; } */
        if (i >= n) break;
        int ss = i; while (i < n && s[i] != '{' && s[i] != '}') i++;   /* selector text up to '{' */
        if (i >= n || s[i] != '{') continue;         /* '}' (e.g. @-rule close) or end before '{' */
        int se = i; i++;                             /* selector is [ss,se); step past '{' */
        int ds = i; while (i < n && s[i] != '}') i++;   /* declaration block [ds,de) */
        int de = i; if (i < n) i++;                  /* step past '}' */
        while (ss < se && (s[ss]==' '||s[ss]=='\t'||s[ss]=='\n'||s[ss]=='\r')) ss++;   /* trim selector */
        while (se > ss && (s[se-1]==' '||s[se-1]=='\t'||s[se-1]=='\n'||s[se-1]=='\r')) se--;
        int sl = se - ss; if (sl <= 0 || sl >= 40) continue;          /* empty or too long -> skip */
        char selbuf[40]; for (int k = 0; k < sl; k++) selbuf[k] = s[ss+k]; selbuf[sl] = 0;
        sel_t sel; if (!sel_parse(selbuf, &sel)) continue;            /* unsupported selector -> skip */
        uint32_t col = parse_style_color(s + ds, de - ds);           /* reuse the reviewed value parsers */
        int tsv = parse_style_textstyle(s + ds, de - ds);
        int ulv = parse_style_underline(s + ds, de - ds);
        int trv = parse_style_transform(s + ds, de - ds);
        uint32_t bgv = parse_style_bg(s + ds, de - ds);
        int alv = parse_style_align(s + ds, de - ds);
        int szv = parse_style_fontsize(s + ds, de - ds);
        int dnv = parse_style_display(s + ds, de - ds);
        if (col || tsv >= 0 || ulv || trv || bgv || alv || szv || dnv) {  /* keep rules that set something we render */
            b->css_sel[b->n_css] = sel;
            b->css_color[b->n_css] = col;
            b->css_style[b->n_css] = (int16_t)tsv;
            b->css_ul[b->n_css] = (uint8_t)ulv;
            b->css_transform[b->n_css] = (uint8_t)trv;
            b->css_bg[b->n_css] = bgv;
            b->css_align[b->n_css] = (uint8_t)alv;
            b->css_size[b->n_css] = (uint8_t)szv;
            b->css_disp[b->n_css] = (uint8_t)dnv;
            b->n_css++;
        }
    }
}
/* Cascade the captured <style> rules onto one element: each matching rule (tag / .class /
 * #id / [attr]) sets *color / *textstyle, later rules winning per property (source order).
 * Returns 1 if any rule matched. Matching mirrors sel_match_all's per-element checks. */
static int css_match(browser_t *b, const char *tag, const char *attrs, int attrlen,
                     uint32_t *color, int *textstyle, int *underline, int *transform, uint32_t *bg,
                     int *align, int *size, int *hidden) {
    int hit = 0;
    for (int r = 0; r < b->n_css; r++) {
        const sel_t *s = &b->css_sel[r];
        if (s->tag[0] && !tageq(tag, s->tag)) continue;
        if (s->cls[0]) { const char *v; int vl; if (!find_attr(attrs, attrlen, "class", &v, &vl) || !class_has(v, vl, s->cls)) continue; }
        if (s->id[0])  { const char *v; int vl; if (!find_attr(attrs, attrlen, "id", &v, &vl)    || !attr_eq(v, vl, s->id))     continue; }
        if (s->attr[0] && !has_attr(attrs, attrlen, s->attr)) continue;
        if (b->css_color[r]) *color = b->css_color[r];
        if (b->css_style[r] >= 0) *textstyle = b->css_style[r];
        if (b->css_ul[r]) *underline = 1;
        if (b->css_transform[r]) *transform = b->css_transform[r];
        if (b->css_bg[r]) *bg = b->css_bg[r];
        if (b->css_align[r]) *align = b->css_align[r];
        if (b->css_size[r]) *size = b->css_size[r];
        if (b->css_disp[r]) *hidden = 1;
        hit = 1;
    }
    return hit;
}
/* Scan the body region for elements matching `sel`; fill offs[] with the byte
 * offset of each matching opening '<'. Returns the count (bounded by max). */
static int sel_match_all(browser_t *b, const sel_t *sel, int *offs, int max) {
    const char *body = b->raw; int lo = b->bodyoff, hi = b->bodyoff + b->bodylen;
    int n = 0;
    for (int i = lo; i < hi && n < max; i++) {
        if (body[i] != '<') continue;
        int j = i + 1;
        if (j>=hi || body[j]=='/' || body[j]=='!' || body[j]=='?') continue;   /* closing tag / comment / decl */
        char tag[16]; int tl = 0;
        while (j<hi && tl<15) { char c=body[j]; if(c=='>'||c==' '||c=='/'||c=='\t'||c=='\n') break; tag[tl++]=(char)lc(c); j++; }
        tag[tl]=0; if (tl==0) continue;
        int astart = j; char q=0;                                   /* quote-aware scan to '>' (mirrors parse_html) */
        while (j<hi) { char c=body[j]; if(q){if(c==q)q=0;} else if(c=='"'||c=='\'')q=c; else if(c=='>')break; j++; }
        int alen = j - astart;
        if (tageq(tag,"script") || tageq(tag,"style")) {   /* raw-text elements: skip their content so tag-like TEXT inside (e.g. "<li class=\"x\">" in a script string) isn't matched */
            int e = j + 1, found = 0;
            while (e + 2 + tl <= hi) {
                if (body[e]=='<' && body[e+1]=='/') { int m=0; while (m<tl && lc(body[e+2+m])==tag[m]) m++; if (m==tl) { found=1; break; } }
                e++;
            }
            i = found ? e : hi;   /* resume at the </tag> (for-loop i++ steps past its '<'); to end if unclosed */
            continue;
        }
        if (sel->tag[0] && !tageq(tag, sel->tag)) { i=j; continue; }
        if (sel->cls[0]) { const char *v; int vl; if(!find_attr(body+astart,alen,"class",&v,&vl) || !class_has(v,vl,sel->cls)) { i=j; continue; } }
        if (sel->id[0])  { const char *v; int vl; if(!find_attr(body+astart,alen,"id",&v,&vl)    || !attr_eq(v,vl,sel->id))     { i=j; continue; } }
        if (sel->attr[0] && !has_attr(body+astart,alen,sel->attr)) { i=j; continue; }   /* [attr] presence (has_attr matches valued + boolean attrs) */
        offs[n++] = i;          /* matched: record the '<' offset */
        i = j;                  /* advance past this opening tag (children are still scanned) */
    }
    return n;
}
/* Position variant of dom_find: the element whose opening '<' is at byte `off`.
 * Validates `off` is a real in-body opening tag (so a stale offset fails closed),
 * then returns INNER's [*is,*ie) via the same depth-count as dom_find. */
static int dom_find_at(browser_t *b, int off, int *is, int *ie) {
    const char *r = b->raw; int lo = b->bodyoff, hi = b->bodyoff + b->bodylen;
    if (off < lo || off >= hi - 1 || r[off] != '<' || !dom_alnum(r[off+1])) return 0;   /* `off >= hi-1` (not `off+1 >= hi`) so off==INT_MAX can't overflow past the guard */
    char tag[16]; int tn = 0, ne = off + 1;
    while (ne < hi && dom_alnum(r[ne]) && tn < 15) tag[tn++] = r[ne++];
    if (tn == 0) return 0;
    int gt = off; while (gt < hi && r[gt] != '>') gt++;
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
/* Read a position-addressed element's textContent/innerHTML (html 0/1). The
 * .value path (html 2) is keyed by id, so a position handle can't address it. */
static int browser_dom_get_at(int off, char *out, int max, int html) {
    browser_t *b = g_ls_b; if (max) out[0] = 0; if (!b) return 0;
    if (html == 2) return 0;
    int is, ie; if (!dom_find_at(b, off, &is, &ie)) return 0;
    int len = ie - is; if (len > max - 1) len = max - 1; if (len < 0) len = 0;
    memcpy(out, b->raw + is, len); out[len] = 0; return 1;
}
/* Position variant of dom_attr_region: the opening-tag attr span of the element at `off`. */
static int dom_attr_region_at(browser_t *b, int off, int *as, int *ae) {
    const char *r = b->raw; int lo = b->bodyoff, hi = b->bodyoff + b->bodylen;
    if (off < lo || off >= hi - 1 || r[off] != '<' || !dom_alnum(r[off+1])) return 0;   /* `off >= hi-1` (not `off+1 >= hi`) so off==INT_MAX can't overflow past the guard */
    int ne = off + 1, tn = 0;
    while (ne < hi && dom_alnum(r[ne]) && tn < 15) { ne++; tn++; }   /* skip the tag name */
    if (tn == 0) return 0;
    int gt = off; while (gt < hi && r[gt] != '>') gt++;
    if (gt >= hi) return 0;
    *as = ne; *ae = gt; return 1;
}
/* getAttribute on a position-addressed element (read-only; mirrors browser_dom_getattr). */
static int browser_dom_getattr_at(int off, const char *attr, char *out, int max) {
    browser_t *b = g_ls_b; if (!b || max <= 0) return 0; out[0] = 0;
    int as, ae; if (!dom_attr_region_at(b, off, &as, &ae)) return 0;
    const char *v; int vl;
    if (!find_attr(b->raw + as, ae - as, attr, &v, &vl)) return 0;
    int n = vl; if (n > max - 1) n = max - 1; if (n < 0) n = 0;
    memcpy(out, v, n); out[n] = 0; return 1;
}
/* document.querySelector(All): parse the selector, scan, fill offs[] with match offsets. */
static int browser_dom_query(const char *sel, int *offs, int max) {
    browser_t *b = g_ls_b; if (!b) return 0;
    sel_t s; if (!sel_parse(sel, &s)) return 0;
    int m = (max < QSA_MAX) ? max : QSA_MAX;
    return sel_match_all(b, &s, offs, m);
}
static int dom_attr_region(browser_t *b, const char *id, int *as, int *ae);   /* fwd (defined below, near browser_dom_getattr) */
/* element.matches(sel): does the element whose opening '<' is at byte `off` match
 * the selector? Reuses the (reviewed) matcher — run it and test membership. */
static int browser_dom_matches_at(int off, const char *sel) {
    browser_t *b = g_ls_b; if (!b) return 0;
    sel_t s; if (!sel_parse(sel, &s)) return 0;
    int offs[QSA_MAX]; int n = sel_match_all(b, &s, offs, QSA_MAX);
    for (int i = 0; i < n; i++) if (offs[i] == off) return 1;
    return 0;
}
/* matches() for an id handle: locate the element's opening '<', then reuse the above. */
static int browser_dom_matches(const char *id, const char *sel) {
    browser_t *b = g_ls_b; if (!b) return 0;
    int as, ae; if (!dom_attr_region(b, id, &as, &ae)) return 0;   /* finds the element by id */
    const char *r = b->raw; int lo = b->bodyoff, off = as;
    while (off > lo && r[off] != '<') off--;
    if (r[off] != '<') return 0;
    return browser_dom_matches_at(off, sel);
}
/* element.closest(sel): the innermost element matching `sel` that is `off`'s self
 * or an ancestor — returns its byte offset, or -1. Forward approach (no backward
 * nesting scan): among the selector's matches, pick the largest offset that is
 * `off` itself or whose inner span [is,ie) ENCLOSES off (so it's an ancestor). */
static int browser_dom_closest_at(int off, const char *sel) {
    browser_t *b = g_ls_b; if (!b) return -1;
    sel_t s; if (!sel_parse(sel, &s)) return -1;
    int offs[QSA_MAX]; int n = sel_match_all(b, &s, offs, QSA_MAX);
    int best = -1;
    for (int i = 0; i < n; i++) {
        int o = offs[i];
        if (o == off) { if (o > best) best = o; }                          /* the element itself matches */
        else if (o < off) { int is, ie; if (dom_find_at(b, o, &is, &ie) && off >= is && off < ie && o > best) best = o; }   /* o encloses off -> ancestor */
    }
    return best;
}
static int browser_dom_closest(const char *id, const char *sel) {
    browser_t *b = g_ls_b; if (!b) return -1;
    int as, ae; if (!dom_attr_region(b, id, &as, &ae)) return -1;
    const char *r = b->raw; int lo = b->bodyoff, off = as;
    while (off > lo && r[off] != '<') off--;
    if (r[off] != '<') return -1;
    return browser_dom_closest_at(off, sel);
}
/* element.children: the byte offsets of the DIRECT child elements of the element
 * at `off`. Scans the inner span for top-level open tags, skipping each child's
 * own subtree (via dom_find_at) so descendants aren't counted. */
static int browser_dom_children_at(int off, int *offs, int max) {
    browser_t *b = g_ls_b; if (!b) return 0;
    int is, ie; if (!dom_find_at(b, off, &is, &ie)) return 0;   /* parent's inner span */
    const char *body = b->raw; int n = 0;
    for (int i = is; i < ie && n < max; ) {
        if (body[i] != '<') { i++; continue; }
        int j = i + 1;
        if (j >= ie || body[j]=='/' || body[j]=='!' || body[j]=='?') { i++; continue; }   /* not an opening tag */
        offs[n++] = i;                                  /* a direct child */
        int cis, cie, e;
        if (dom_find_at(b, i, &cis, &cie)) { e = cie; while (e < ie && body[e] != '>') e++; }   /* skip the child's whole element */
        else { e = i; while (e < ie && body[e] != '>') e++; }                                   /* void/self-closing: just past its '>' */
        i = (e < ie) ? e + 1 : ie;
    }
    return n;
}
static int browser_dom_children(const char *id, int *offs, int max) {
    browser_t *b = g_ls_b; if (!b) return 0;
    int as, ae; if (!dom_attr_region(b, id, &as, &ae)) return 0;
    const char *r = b->raw; int lo = b->bodyoff, off = as;
    while (off > lo && r[off] != '<') off--;
    if (r[off] != '<') return 0;
    return browser_dom_children_at(off, offs, max);
}
/* element.parentElement: the byte offset of the innermost element that ENCLOSES
 * the element at `off` (its immediate parent), or -1. Forward scan: the largest
 * offset < off whose inner span [is,ie) contains off. */
static int browser_dom_parent_at(int off) {
    browser_t *b = g_ls_b; if (!b) return -1;
    const char *body = b->raw; int lo = b->bodyoff, hi = b->bodyoff + b->bodylen;
    int best = -1;
    for (int i = lo; i < off && i < hi; i++) {
        if (body[i] != '<') continue;
        int j = i + 1;
        if (j >= hi || !dom_alnum(body[j])) continue;   /* an opening tag */
        int is, ie; if (dom_find_at(b, i, &is, &ie) && off >= is && off < ie && i > best) best = i;   /* i encloses off */
    }
    return best;
}
static int browser_dom_parent(const char *id) {
    browser_t *b = g_ls_b; if (!b) return -1;
    int as, ae; if (!dom_attr_region(b, id, &as, &ae)) return -1;
    const char *r = b->raw; int lo = b->bodyoff, off = as;
    while (off > lo && r[off] != '<') off--;
    if (r[off] != '<') return -1;
    return browser_dom_parent_at(off);
}
/* element.nextElementSibling / previousElementSibling: among the element's
 * siblings (the top-level children of its parent, or top-level body elements),
 * the nearest one after (*next) / before (*prev) it. One scan of the sibling range. */
static void browser_dom_siblings_at(int off, int *prev, int *next) {
    *prev = -1; *next = -1;
    browser_t *b = g_ls_b; if (!b) return;
    int lo, hi, parent = browser_dom_parent_at(off);
    if (parent >= 0) { int is, ie; if (!dom_find_at(b, parent, &is, &ie)) return; lo = is; hi = ie; }
    else { lo = b->bodyoff; hi = b->bodyoff + b->bodylen; }   /* top-level: siblings are body-level elements */
    const char *body = b->raw;
    for (int i = lo; i < hi; ) {
        if (body[i] != '<') { i++; continue; }
        int j = i + 1;
        if (j >= hi || body[j]=='/' || body[j]=='!' || body[j]=='?' || !dom_alnum(body[j])) { i++; continue; }
        if (i < off) { if (i > *prev) *prev = i; }              /* a sibling before off */
        else if (i > off && *next < 0) *next = i;               /* the first sibling after off */
        int cis, cie, e;
        if (dom_find_at(b, i, &cis, &cie)) { e = cie; while (e < hi && body[e] != '>') e++; }
        else { e = i; while (e < hi && body[e] != '>') e++; }
        i = (e < hi) ? e + 1 : hi;                              /* skip this sibling's whole element */
    }
}
static int browser_dom_sibling_at(int off, int dir) {   /* dir<0 = previous, else = next; returns offset or -1 */
    int prev, next; browser_dom_siblings_at(off, &prev, &next);
    return dir < 0 ? prev : next;
}
static int browser_dom_sibling(const char *id, int dir) {
    browser_t *b = g_ls_b; if (!b) return -1;
    int as, ae; if (!dom_attr_region(b, id, &as, &ae)) return -1;
    const char *r = b->raw; int lo = b->bodyoff, off = as;
    while (off > lo && r[off] != '<') off--;
    if (r[off] != '<') return -1;
    return browser_dom_sibling_at(off, dir);
}
/* element.tagName / nodeName: the element's tag name, UPPERCASED (per the DOM). */
static int browser_dom_tag_at(int off, char *out, int max) {
    browser_t *b = g_ls_b; if (max <= 0) return 0; out[0] = 0; if (!b) return 0;
    const char *r = b->raw; int hi = b->bodyoff + b->bodylen;
    if (off < b->bodyoff || off+1 >= hi || r[off] != '<' || !dom_alnum(r[off+1])) return 0;
    int p = off+1, o = 0;
    while (p < hi && dom_alnum(r[p]) && o < max-1) { char c = r[p++]; out[o++] = (c >= 'a' && c <= 'z') ? c - 32 : c; }
    out[o] = 0; return o > 0;
}
static int browser_dom_tag(const char *id, char *out, int max) {
    browser_t *b = g_ls_b; if (max <= 0) return 0; out[0] = 0; if (!b) return 0;
    int as, ae; if (!dom_attr_region(b, id, &as, &ae)) return 0;
    const char *r = b->raw; int lo = b->bodyoff, off = as;
    while (off > lo && r[off] != '<') off--;
    if (r[off] != '<') return 0;
    return browser_dom_tag_at(off, out, max);
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
    if (html == 3) {   /* element.remove(): splice the whole <tag id>…</tag> out of b->raw (full-span variant of the set below) */
        int is, ie; if (!dom_find(b, id, &is, &ie)) return;
        const char *r = b->raw; int lo = b->bodyoff;
        int ts = is - 1; while (ts > lo && r[ts] != '<') ts--; if (r[ts] != '<') return;   /* opening '<' */
        int bodyend = b->bodyoff + b->bodylen;
        int ce = ie; while (ce < bodyend && r[ce] != '>') ce++; if (ce >= bodyend) return; ce++;   /* past the closing '>' */
        int delta = -(ce - ts);                                   /* removing [ts, ce) */
        int active = (g_sw_raw == b->raw);
        int live_end = (active && g_sw_pos > bodyend) ? g_sw_pos : bodyend;
        memmove(b->raw + ts, b->raw + ce, live_end - ce);         /* shift the tail down over the removed element */
        b->bodylen += delta;
        if (active) { if (g_sw_pos > ts) g_sw_pos += delta; if (g_sw_base > ts) g_sw_base += delta; }   /* sync document.write cursors; pivot on ts (removed-span start) so both shift together */
        b->raw[live_end + delta] = 0;
        parse_html(b, b->raw + b->bodyoff, b->bodylen);
        return;
    }
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
/* Position-keyed write: textContent/innerHTML (html 0/1) and remove (html 3) on
 * the element whose opening '<' is at byte `off`. Duplicates browser_dom_set's
 * splice with dom_find_at in place of dom_find (the id path stays untouched).
 * NOTE: after a splice + re-render, the offsets of LATER elements shift, so a
 * second position handle into the same page is stale; dom_find_at re-validates
 * '<' at off, so a stale write fails closed (no-op) or edits a still-valid '<' —
 * always within bounds (memory-safe). Single-match writes are exact. */
static void browser_dom_set_at(int off, const char *value, int html) {
    browser_t *b = g_ls_b; if (!b) return;
    if (html == 2) return;   /* .value is keyed by id; not position-addressable */
    if (html == 3) {         /* remove(): off IS the opening '<'; splice [off, past-close) */
        int is, ie; if (!dom_find_at(b, off, &is, &ie)) return;
        const char *r = b->raw; int bodyend = b->bodyoff + b->bodylen;
        int ts = off;
        int ce = ie; while (ce < bodyend && r[ce] != '>') ce++; if (ce >= bodyend) return; ce++;
        int delta = -(ce - ts);
        int active = (g_sw_raw == b->raw);
        int live_end = (active && g_sw_pos > bodyend) ? g_sw_pos : bodyend;
        memmove(b->raw + ts, b->raw + ce, live_end - ce);
        b->bodylen += delta;
        if (active) { if (g_sw_pos > ts) g_sw_pos += delta; if (g_sw_base > ts) g_sw_base += delta; }
        b->raw[live_end + delta] = 0;
        parse_html(b, b->raw + b->bodyoff, b->bodylen);
        return;
    }
    static char esc[8192];
    if (!html) {   /* textContent: HTML-escape (innerHTML inserts raw) — same as browser_dom_set */
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
    int is, ie; if (!dom_find_at(b, off, &is, &ie)) return;
    int vlen = 0; while (value[vlen]) vlen++;
    int delta = vlen - (ie - is);
    int active = (g_sw_raw == b->raw);
    int bodyend = b->bodyoff + b->bodylen;
    int live_end = (active && g_sw_pos > bodyend) ? g_sw_pos : bodyend;
    if (live_end + delta >= RAW_MAX - 1 || live_end + delta < b->bodyoff) return;
    memmove(b->raw + ie + delta, b->raw + ie, live_end - ie);
    memcpy(b->raw + is, value, vlen);
    b->bodylen += delta;
    if (active) { if (g_sw_pos > ie) g_sw_pos += delta; if (g_sw_base > ie) g_sw_base += delta; }
    b->raw[live_end + delta] = 0;
    parse_html(b, b->raw + b->bodyoff, b->bodylen);
}
/* Position-keyed setAttribute: duplicates browser_dom_setattr with dom_attr_region_at. */
static void browser_dom_setattr_at(int off, const char *attr, const char *val) {
    browser_t *b = g_ls_b; if (!b || !attr[0]) return;
    int as, ae; if (!dom_attr_region_at(b, off, &as, &ae)) return;
    char vbuf[256]; int vlen = 0;
    for (int i = 0; val[i] && vlen < (int)sizeof(vbuf) - 1; i++) { char c = val[i]; if (c != '"' && c != '\'') vbuf[vlen++] = c; }
    vbuf[vlen] = 0;
    const char *fv; int fvl;
    int rs, rend; const char *repl; int rlen; char ins[384];
    if (find_attr(b->raw + as, ae - as, attr, &fv, &fvl)) {
        rs = (int)(fv - b->raw); rend = rs + fvl; repl = vbuf; rlen = vlen;
    } else {
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
    if (live_end + delta >= RAW_MAX - 1 || live_end + delta < b->bodyoff) return;
    memmove(b->raw + rend + delta, b->raw + rend, live_end - rend);
    memcpy(b->raw + rs, repl, rlen);
    b->bodylen += delta;
    if (active) { if (g_sw_pos > rend) g_sw_pos += delta; if (g_sw_base > rend) g_sw_base += delta; }
    b->raw[live_end + delta] = 0;
    parse_html(b, b->raw + b->bodyoff, b->bodylen);
}
/* Find the full " attr[=value]" span (incl one leading space) within the opening-tag
 * attr region [as,ae), for removeAttribute. Returns 1 + [*rs,*rend). */
static int attr_span(browser_t *b, int as, int ae, const char *attr, int *rs, int *rend) {
    const char *a = b->raw; int nl=0; while(attr[nl]) nl++; if(!nl) return 0;
    for (int i = as; i + nl <= ae; i++) {
        if (i>as && a[i-1]!=' ' && a[i-1]!='\t') continue;                 /* attr-name boundary */
        int m=0; while (m<nl && lc(a[i+m])==lc(attr[m])) m++;
        if (m!=nl) continue;
        int k=i+nl;
        if (k<ae && a[k]!=' ' && a[k]!='\t' && a[k]!='=' && a[k]!='>') continue;   /* complete token, not a prefix */
        int start=i; while (start>as && (a[start-1]==' '||a[start-1]=='\t')) start--;   /* eat one+ leading space */
        int e=k; while (e<ae && (a[e]==' '||a[e]=='\t')) e++;              /* optional =value */
        if (e<ae && a[e]=='=') { e++; while (e<ae && (a[e]==' '||a[e]=='\t')) e++;
            if (e<ae && (a[e]=='"'||a[e]=='\'')) { char q=a[e]; e++; while (e<ae && a[e]!=q) e++; if(e<ae) e++; }   /* quoted value */
            else { while (e<ae && a[e]!=' ' && a[e]!='\t') e++; } }                                                /* unquoted value */
        *rs = start; *rend = e; return 1;
    }
    return 0;
}
/* removeAttribute: splice " attr[=value]" out of the opening tag + re-render.
 * Mirrors browser_dom_set's remove-path splice (bounds-checked). */
static void browser_dom_rmattr(const char *id, const char *attr) {
    browser_t *b=g_ls_b; if(!b||!attr[0]) return;
    int as,ae; if(!dom_attr_region(b,id,&as,&ae)) return;
    int rs,rend; if(!attr_span(b,as,ae,attr,&rs,&rend)) return;
    int delta=-(rend-rs);
    int active=(g_sw_raw==b->raw); int bodyend=b->bodyoff+b->bodylen;
    int live_end=(active && g_sw_pos>bodyend)?g_sw_pos:bodyend;
    memmove(b->raw+rs, b->raw+rend, live_end-rend);
    b->bodylen+=delta;
    if(active){ if(g_sw_pos>rend)g_sw_pos+=delta; if(g_sw_base>rend)g_sw_base+=delta; }
    b->raw[live_end+delta]=0;
    parse_html(b,b->raw+b->bodyoff,b->bodylen);
}
static void browser_dom_rmattr_at(int off, const char *attr) {   /* position-handle variant */
    browser_t *b=g_ls_b; if(!b||!attr[0]) return;
    int as,ae; if(!dom_attr_region_at(b,off,&as,&ae)) return;
    int rs,rend; if(!attr_span(b,as,ae,attr,&rs,&rend)) return;
    int delta=-(rend-rs);
    int active=(g_sw_raw==b->raw); int bodyend=b->bodyoff+b->bodylen;
    int live_end=(active && g_sw_pos>bodyend)?g_sw_pos:bodyend;
    memmove(b->raw+rs, b->raw+rend, live_end-rend);
    b->bodylen+=delta;
    if(active){ if(g_sw_pos>rend)g_sw_pos+=delta; if(g_sw_base>rend)g_sw_base+=delta; }
    b->raw[live_end+delta]=0;
    parse_html(b,b->raw+b->bodyoff,b->bodylen);
}
static void js_bind_storage(browser_t *b){ g_ls_b=b; js_set_storage(browser_ls_get, browser_ls_set); js_set_dom(browser_dom_get, browser_dom_set); js_set_dom_attr(browser_dom_getattr, browser_dom_setattr); js_set_dom_pos(browser_dom_get_at, browser_dom_set_at, browser_dom_getattr_at, browser_dom_setattr_at, browser_dom_query); js_set_dom_match(browser_dom_matches, browser_dom_matches_at, browser_dom_closest, browser_dom_closest_at); js_set_dom_rmattr(browser_dom_rmattr, browser_dom_rmattr_at); js_set_dom_children(browser_dom_children, browser_dom_children_at, browser_dom_parent, browser_dom_parent_at, browser_dom_sibling, browser_dom_sibling_at); js_set_dom_tag(browser_dom_tag, browser_dom_tag_at); js_set_location(b->url); }
static void run_page_scripts(browser_t *b, int bodyoff, int bodylen) {
    static char jsout[2048];
    int appendpos = bodyoff + bodylen;                   /* splice point in b->raw */
    if (appendpos >= RAW_MAX - 1) return;                /* no room to write */
    g_sw_raw = b->raw; g_sw_pos = appendpos; g_sw_base = appendpos; g_sw_max = RAW_MAX;
    js_bind_storage(b);
    js_page_load(b->scripts, jsout, sizeof(jsout), script_write_cb);   /* persists the page's global env for later events */
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
    js_page_event(code, jsout, sizeof(jsout), script_write_cb);   /* runs in the persistent page env (sees load-script globals) */
    int written = g_sw_pos - g_sw_base;                  /* g_sw_base may have shifted if a DOM write moved the buffer */
    g_sw_raw = 0;
    if (jsout[0]) kprintf("[js] %s\n", jsout);
    if (written > 0) { b->bodylen += written; parse_html(b, b->raw + b->bodyoff, b->bodylen); }
    /* a DOM mutation or document.write re-render clears the selection; restore it so
     * pressing Enter again re-runs the same link (e.g. clicking a counter repeatedly). */
    if (saved_sel != NO_LINK && saved_sel < b->nlink) b->sel = saved_sel;
}
/* Activate an `event:ID` link: fire the JS-assigned handler registered for that
 * element id. Same document.write splice + re-render + selection-restore wrapper
 * as run_js_handler, but dispatches through js_fire_event (the persistent env). */
static void run_js_event(browser_t *b, const char *id, const char *type) {
    static char jsout[2048];
    int saved_sel = b->sel;
    int appendpos = b->bodyoff + b->bodylen;
    if (appendpos >= RAW_MAX - 1) return;
    g_sw_raw = b->raw; g_sw_pos = appendpos; g_sw_base = appendpos; g_sw_max = RAW_MAX;
    js_bind_storage(b);
    js_fire_event(id, type, jsout, sizeof(jsout), script_write_cb);
    int written = g_sw_pos - g_sw_base;
    g_sw_raw = 0;
    if (jsout[0]) kprintf("[js] %s\n", jsout);
    if (written > 0) { b->bodylen += written; parse_html(b, b->raw + b->bodyoff, b->bodylen); }
    if (saved_sel != NO_LINK && saved_sel < b->nlink) b->sel = saved_sel;
}
/* If the element has the named inline handler (onchange/oninput/…), run it.
 * Returns 1 if it ran (run_js_handler re-renders), so the caller can skip its own. */
static int fire_handler(browser_t *b, const char *id, const char *attr) {
    int as, ae;
    if (dom_attr_region(b, id, &as, &ae)) {
        const char *oc; int ocl;
        if (find_attr(b->raw + as, ae - as, attr, &oc, &ocl) && ocl > 0) {   /* inline onchange="CODE" */
            char code[1024]; int n = ocl; if (n > (int)sizeof(code) - 1) n = (int)sizeof(code) - 1;
            for (int i = 0; i < n; i++) code[i] = oc[i];   /* copy BEFORE run_js_handler mutates b->raw */
            code[n] = 0;
            run_js_handler(b, code);
            return 1;
        }
    }
    /* no inline attribute: try a JS-assigned handler (el.onchange=fn / addEventListener),
     * keyed by id with the bare event type ("on" stripped). Rides this same input path. */
    {
        static char jsout[2048];
        int saved_sel = b->sel;                          /* preserve selection across the re-render (parity with run_js_handler) */
        int appendpos = b->bodyoff + b->bodylen;
        if (appendpos >= RAW_MAX - 1) return 0;
        g_sw_raw = b->raw; g_sw_pos = appendpos; g_sw_base = appendpos; g_sw_max = RAW_MAX;
        js_bind_storage(b);
        int ran = js_fire_event(id, attr + 2, jsout, sizeof(jsout), script_write_cb);
        int written = g_sw_pos - g_sw_base;
        g_sw_raw = 0;
        if (jsout[0]) kprintf("[js] %s\n", jsout);
        if (written > 0) b->bodylen += written;
        if (ran || written > 0) parse_html(b, b->raw + b->bodyoff, b->bodylen);   /* re-render to show the change + handler effects */
        if (saved_sel != NO_LINK && saved_sel < b->nlink) b->sel = saved_sel;
        return ran;
    }
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
    drop_remote_imgs(b);                           /* prefetched remote-image bytes */
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

/* Resolve a raw <img src> (as it appears in the HTML) into a full absolute URL,
 * the SAME way goto_href resolves a link: an absolute http(s):// src is kept;
 * a protocol-relative //host/path, root-relative /path, or dir-relative src is
 * resolved against `base` (the page URL), keeping the page's scheme. file:/data:
 * srcs are rejected (returns 0). Writes a NUL-terminated URL into out[<outsz];
 * returns 1 on success, 0 if it can't be resolved or won't fit. */
static int resolve_img_url(const char *base, const char *src, int srcl, char *out, int outsz) {
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

/* Best-effort: scan the fetched HTML in b->raw for up to REMOTE_IMG_MAX remote
 * <img src=...> URLs, fetch each over HTTP/HTTPS, and stash the COMPRESSED bytes
 * in b->rimg_data[]/rimg_len[] keyed by the RAW src string (b->rimg_src[]). The
 * parse later matches an <img>'s raw src against rimg_src[] and decodes inline.
 *
 * STRICTLY best-effort: any failure (alloc, fetch<=0, src too long, no src)
 * simply skips that image. Runs on the worker (it does the blocking tls_get/
 * http_get). Re-checks b->closed between fetches so a closed window bails fast. */
static void collect_remote_imgs(browser_t *b) {
    b->n_rimg = 0;
    if (!b->raw || b->rawlen <= 0) return;
    const char *h = b->raw;
    int len = b->rawlen;
    for (int i = 0; i + 4 < len && b->n_rimg < REMOTE_IMG_MAX; i++) {
        if (b->closed) return;                            /* window closed mid-scan */
        if (h[i] != '<') continue;
        if (lc(h[i+1]) != 'i' || lc(h[i+2]) != 'm' || lc(h[i+3]) != 'g') continue;
        if (h[i+4] != ' ' && h[i+4] != '\t' && h[i+4] != '\n' && h[i+4] != '\r' && h[i+4] != '/' && h[i+4] != '>') continue;
        /* find the end of this tag's attributes (the unquoted '>'), quote-aware
         * exactly like the main tokenizer so we delimit the same <img> it does */
        int as = i + 4, ae = as;
        { char q = 0;
          while (ae < len) { char ac = h[ae];
              if (q) { if (ac == q) q = 0; }
              else if (ac=='"' || ac=='\'') q = ac;
              else if (ac=='>') break;
              ae++; } }
        if (ae >= len) break;                             /* unterminated tag -> stop */
        const char *src; int srcl;
        if (find_attr(h + as, ae - as, "src", &src, &srcl) && srcl > 0 && srcl <= 95) {
            /* our decoders handle PNG/GIF/JPEG/SVG — skip a src whose extension is a
             * format we still can't decode (WebP/AVIF/ICO) so we don't waste a fetch +
             * pre-paint latency on an image that would just fall back to a link.
             * Extension-less srcs are still tried (may be a real image). */
            int pe = srcl; for (int x = 0; x < srcl; x++) if (src[x] == '?') { pe = x; break; }
            int unsup = 0;
            if (pe >= 4) { const char *e = src + pe - 4;
                if (e[0]=='.' && lc(e[1])=='i'&&lc(e[2])=='c'&&lc(e[3])=='o') unsup = 1; }     /* .ico */
            if (!unsup && pe >= 5) { const char *e = src + pe - 5;
                if (e[0]=='.' && lc(e[1])=='w'&&lc(e[2])=='e'&&lc(e[3])=='b'&&lc(e[4])=='p') unsup = 1;   /* .webp */
                if (e[0]=='.' && lc(e[1])=='a'&&lc(e[2])=='v'&&lc(e[3])=='i'&&lc(e[4])=='f') unsup = 1; } /* .avif */
            if (unsup) { i = ae; continue; }
            /* skip a src already queued (a page may repeat the same image) */
            int dup = 0;
            for (int k = 0; k < b->n_rimg; k++) {
                int m = 1;
                for (int j = 0; j < srcl; j++) { if (b->rimg_src[k][j] != src[j]) { m = 0; break; } }
                if (m && b->rimg_src[k][srcl] == 0) { dup = 1; break; }
            }
            if (!dup) {
                char url[URL_MAX];
                if (resolve_img_url(b->url, src, srcl, url, sizeof(url))) {
                    int k = b->n_rimg;                    /* candidate slot */
                    char host[96];
                    const char *path = url_split(url, host, sizeof(host));
                    int https = startsw(url, "https://");
                    uint8_t *scratch = kmalloc(IMG_READ_MAX);
                    if (scratch) {
                        int n = https
                            ? tls_get(host, path, scratch, IMG_READ_MAX - 1, (uint32_t)timer_ticks())
                            : http_get(host, path, (char *)scratch, IMG_READ_MAX - 1);
                        if (n > 0) {
                            /* tls_get/http_get returns the whole HTTP response; strip the
                             * headers (and de-chunk) to get the raw image bytes — exactly
                             * like the page path in browser_poll. Storing the response with
                             * its headers would put "HTTP/1.1 200..." before the image
                             * signature and the decoder would reject it. */
                            int bo = 0;
                            for (int x = 0; x + 3 < n; x++)
                                if (scratch[x]=='\r'&&scratch[x+1]=='\n'&&scratch[x+2]=='\r'&&scratch[x+3]=='\n') { bo = x + 4; break; }
                            int blen = n - bo;
                            if (bo > 0 && is_chunked((const char *)scratch, bo))
                                blen = dechunk((char *)scratch + bo, blen);
                            if (blen > 0) {
                                uint8_t *data = kmalloc((unsigned long)blen);
                                if (data) {
                                    memcpy(data, scratch + bo, (unsigned long)blen);
                                    /* commit this slot only once everything succeeded */
                                    for (int j = 0; j < srcl; j++) b->rimg_src[k][j] = src[j];
                                    b->rimg_src[k][srcl] = 0;
                                    b->rimg_data[k] = data;
                                    b->rimg_len[k]  = blen;
                                    b->n_rimg++;
                                }
                            }
                        }
                        kfree(scratch);
                    }
                }
            }
        }
        i = ae;                                           /* skip past this tag */
    }
}

/* Worker task: fetch b->url into b->raw. The close/finish decision is made under
 * a lock so EXACTLY ONE of the worker and browser_destroy frees b (review C1). */
static void worker_fetch(browser_t *b) {
    /* Free the PREVIOUS page's prefetched remote images here, on the worker
     * thread — this runs while loading==1, and the WM reads rimg_* only when
     * !loading (the <img> branch is gated on it), so the free/repopulate here
     * can't race a WM read. collect_remote_imgs below repopulates them. */
    drop_remote_imgs(b);
    char host[96];
    const char *path = url_split(b->url, host, sizeof(host));
    int n; int https = startsw(b->url, "https://");
    if (!b->raw)    n = -1;
    else if (https) n = tls_get(host, path, (uint8_t *)b->raw, RAW_MAX - 1, (uint32_t)timer_ticks());
    else            n = http_get(host, path, b->raw, RAW_MAX - 1);
    b->cert_status = https ? tls_cert_status() : -2;   /* surface TLS cert result in the UI */
    b->chain_ok = https ? tls_chain_anchored() : 0;
    b->host_match = https ? tls_host_match() : -2;
    b->cert_cn[0] = b->cert_expiry[0] = 0;
    if (https && n > 0) {                              /* snapshot the leaf identity for the 'i' display */
        const char *cn = tls_leaf_cn(), *ex = tls_leaf_expiry();
        int k = 0; while (cn[k] && k < (int)sizeof(b->cert_cn)-1) { b->cert_cn[k] = cn[k]; k++; } b->cert_cn[k] = 0;
        k = 0; while (ex[k] && k < (int)sizeof(b->cert_expiry)-1) { b->cert_expiry[k] = ex[k]; k++; } b->cert_expiry[k] = 0;
    }
    b->http_n = n;
    if (n > 0) { b->rawlen = n; b->raw[n] = 0; }
    /* Best-effort inline-image prefetch (worker-side; does its own blocking
     * fetches). MUST NOT prevent the render handoff below: collect_remote_imgs
     * never aborts the page and bails fast if the window closed. Only runs on a
     * real HTML fetch; a closed window skips it entirely (n_rimg already 0 from
     * the drop_remote_imgs above). */
    if (n > 0 && !b->closed) collect_remote_imgs(b);
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
    HAPP("<html><head><title>OS-DEV Start</title>"
         "<style> h1 { color: #2C66D6 }  dd { color: #555555 }  "
         ".new { color: #006400; font-weight: bold } </style></head><body>"
         "<h1>OS-DEV Browser</h1>"
         "<p class=\"new\">New this build: a small CSS engine &mdash; this very page is styled by it "
         "&mdash; plus twenty-four apps including a piano, a music jukebox, a maze, a Mandelbrot explorer, an ASCII paint, a text adventure, a typing test, Simon, tic-tac-toe against an unbeatable AI, and Blackjack.</p>"
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
         "<dt><a href=\"file:help.htm\">file:help.htm</a></dt><dd>a built-in <b>help/manual</b> &mdash; apps, shell commands, and keyboard shortcuts (styled by the CSS engine)</dd>"
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

/* Free the prefetched COMPRESSED remote-image bytes and clear the match table.
 * Called on navigation (before fetching the next page) and on teardown. NOT
 * called from parse_html, so the bytes survive every re-render of one page. */
static void drop_remote_imgs(browser_t *b) {
    for (int k = 0; k < REMOTE_IMG_MAX; k++) {
        if (b->rimg_data[k]) { kfree(b->rimg_data[k]); b->rimg_data[k] = 0; }
        b->rimg_len[k] = 0;
        b->rimg_src[k][0] = 0;
    }
    b->n_rimg = 0;
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
    /* SVG (text XML): detect "<svg" within the first chunk, then rasterize. svg_decode
     * caps W,H<=512, so a 1 MB worst-case buffer holds any output; shrink to exact after. */
    { int issvg = 0, lim = len < 512 ? len : 512;
      for (int i = 0; i + 4 <= lim; i++)
          if (data[i]=='<' && (data[i+1]|32)=='s' && (data[i+2]|32)=='v' && (data[i+3]|32)=='g') { issvg = 1; break; }
      if (issvg) {
          long ocap = 512L*512*4, scap = 256*1024;
          uint8_t *rgba = kmalloc((unsigned long)ocap);
          uint8_t *scr  = kmalloc((unsigned long)scap);
          if (!rgba || !scr) { if (rgba) kfree(rgba); if (scr) kfree(scr); return 0; }
          int w = 0, h = 0;
          int r = svg_decode(data, len, rgba, (int)ocap, scr, (int)scap, &w, &h);
          kfree(scr);
          if (r != 0 || w <= 0 || h <= 0) { kfree(rgba); return 0; }
          long exact = (long)w * h * 4;            /* shrink the 1 MB worst-case buffer to exactly W*H*4 */
          uint8_t *fit = kmalloc((unsigned long)exact);
          if (fit) { memcpy(fit, rgba, (unsigned long)exact); kfree(rgba); *ow = w; *oh = h; return fit; }
          *ow = w; *oh = h; return rgba;           /* shrink alloc failed: the big buffer is still valid */
      }
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

/* Decode an in-memory image blob (PNG/GIF/JPEG) into the next inline-image
 * slot. Returns the slot index, or -1 (caller falls back to a clickable link).
 * Shared by the local-file and remote-image paths. */
static int decode_bytes_to_slot(browser_t *b, const uint8_t *data, int len) {
    if (b->nimg >= IMG_SLOTS) return -1;
    if (!data || len <= 0) return -1;
    int ow, oh;
    uint8_t *rgba = decode_image(data, len, &ow, &oh);
    if (!rgba) return -1;
    int s = b->nimg++;
    b->imgs[s] = rgba; b->imgsw[s] = ow; b->imgsh[s] = oh;
    return s;
}

/* Read a local file and decode it into the next inline-image slot. Returns the
 * slot index, or -1 (caller then falls back to a clickable link). */
static int decode_local_to_slot(browser_t *b, const char *path) {
    if (b->nimg >= IMG_SLOTS) return -1;
    uint8_t *buf = kmalloc(IMG_READ_MAX);
    if (!buf) return -1;
    long n = vfs_read(path, buf, IMG_READ_MAX);
    if (n <= 0) { kfree(buf); return -1; }
    int s = decode_bytes_to_slot(b, buf, (int)n);
    kfree(buf);
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
    js_page_reset();                  /* drop the previous page's persistent JS globals */
    memset(b->det_open, 0xFF, sizeof(b->det_open));   /* <details> states unseeded until first render */

    if (streqs(b->url, "home") || !b->url[0]) {       /* built-in start page, no net */
        if (b->loading) { set_status(b, "busy, retry"); return; }
        drop_remote_imgs(b);          /* no worker here: clear any prior page's remote-image table */
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
        drop_remote_imgs(b);          /* no worker here: clear any prior page's remote-image table */
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
    js_page_reset();                           /* this branch skips browser_navigate; drop the previous page's JS env so a script-less backed-to page doesn't reuse it */
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
    int isdet = (len > 4); if (isdet) for (int k = 0; k < 4; k++) if (lc(hp[k]) != "det:"[k]) { isdet = 0; break; }
    if (isdet) {                                         /* toggle a <details> open/closed */
        int idx = 0; for (int k = 4; k < len; k++) if (hp[k] >= '0' && hp[k] <= '9') idx = idx * 10 + (hp[k] - '0');
        if (idx >= 0 && idx < 16) b->det_open[idx] = b->det_open[idx] ? 0 : 1;   /* flip (seeded by the render) */
        parse_html(b, b->raw + b->bodyoff, b->bodylen);
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
    int isev = (len > 6); if (isev) for (int k = 0; k < 6; k++) if (lc(hp[k]) != "event:"[k]) { isev = 0; break; }
    if (isev) {                                          /* event:ID -> fire the JS-assigned click handler for element ID */
        char eid[40]; int n = len - 6; if (n > 39) n = 39;
        for (int i = 0; i < n; i++) eid[i] = hp[6 + i];
        eid[n] = 0;
        run_js_event(b, eid, "click");
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

        int zm = b->zoom > 0 ? b->zoom : 1;                 /* content zoom (1..4) */
        int tsc = (t < TOK_MAX) ? b->tokscale[t] : 0;       /* CSS font-size override (0 = use style default) */
        int sc = (tsc ? tsc : scale_for(tk->style)) * zm;
        int lh = (tsc ? (16 * tsc + 2) : lineh_for(tk->style)) * zm;
        int wpx = tk->len * GW * sc; if (wpx > cr - cl) wpx = cr - cl;
        if (cx + wpx > cr && cx > cl) { cy += curlh; cx = cl; curlh = 18; }
        if (lh > curlh) curlh = lh;
        /* at a line start (cx==cl): apply the line's left-indent (<blockquote>) and, for
         * center/right text-align, look ahead over the words that fit on the line
         * (replicating the wrap test) and shift cx. indent 0 + align 0 (the defaults) ->
         * cx stays cl, so ordinary pages are byte-for-byte unchanged; everything is bounded
         * by ntok and cx only ever moves within [cl, cr). A line takes its first token's value. */
        if (cx == cl) {
            int indent = (t < TOK_MAX) ? b->tokindent[t] : 0;
            if (indent > (cr - cl) - GW * 4) indent = (cr - cl) - GW * 4;   /* leave room for text */
            if (indent < 0) indent = 0;
            int ls = cl + indent;
            int al = (t < TOK_MAX) ? b->tokalign[t] : 0;
            if (al) {
                int avail = cr - ls, probe = ls, endx = ls;
                for (int u = t; u < b->ntok; u++) {
                    tok_t *pk = &b->toks[u];
                    if (pk->type != TK_WORD) break;          /* break/para/hr/img end the line */
                    int pts = (u < TOK_MAX) ? b->tokscale[u] : 0;
                    int ps = (pts ? pts : scale_for(pk->style)) * zm;
                    int pw = pk->len * GW * ps; if (pw > avail) pw = avail;
                    if (probe + pw > cr && probe > ls) break;  /* would wrap -> line ends here */
                    endx = probe + pw; probe = endx + GW * ps;
                }
                int off = (al == 1) ? (avail - (endx - ls)) / 2 : (avail - (endx - ls));
                if (off < 0) off = 0;
                cx = ls + off;
            } else cx = ls;
        }

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
            /* content background: explicit CSS background-color wins over the <mark> default */
            uint32_t cbg = (b->tokbg[t] & 0x01000000) ? (b->tokbg[t] & 0xFFFFFF)
                         : (tk->style == STY_MARK ? 0xFFF080 : BG);
            uint32_t wbg = selected ? 0xFFE9A8 : (current ? 0x7FC0FF : (matched ? 0xCDE8FF : cbg));
            int maxc = (cr - cx) / (GW * sc); if (maxc < 0) maxc = 0;
            int dl = tk->len > maxc ? maxc : tk->len;      /* clip to content width (no h-scroll) */
            int drawpx = dl * GW * sc;
            int yo = tk->style == STY_SUB ? 5 : tk->style == STY_SUP ? -4 : 0;   /* sub/superscript vertical shift */
            put_word(cx, cy + yo, b->text + tk->off, dl, fg, wbg, sc);
            if (tk->style == STY_BOLD) {                   /* faux-bold: transparent 1px overstrike */
                char w[72]; int ln = dl > 71 ? 71 : dl;
                for (int i = 0; i < ln; i++) w[i] = b->text[tk->off + i];
                w[ln] = 0;
                fb_text(cx + 1, cy, w, fg, sc);
            }
            if (tk->style == STY_STRIKE) fb_fill_rect(cx, cy + 7, drawpx, 1, fg);   /* strike-line through the text */
            if (b->tokul[t] && tk->style != STY_LINK) fb_fill_rect(cx, cy + 15, drawpx, 1, fg);   /* underline (text-decoration / <u>); links already underline */
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
    case '+': case '=': if (b->zoom < 4) b->zoom++; set_status(b, b->zoom>1?"zoom in":""); break;   /* content zoom */
    case '-': case '_': if (b->zoom > 1) b->zoom--; set_status(b, b->zoom>1?"zoom out":"1x"); break;
    case '0':           b->zoom = 1; set_status(b, "1x"); break;   /* reset zoom */
    case 'h':           copy_url(b->url, "home"); browser_navigate(b); break;  /* start page */
    case 'r':           browser_navigate(b); break;
    case 's':           browser_save(b);   break;   /* save page to PAGE.TXT */
    case 'u':           if (!b->img) { b->viewsource = !b->viewsource; b->scroll = 0;  /* toggle raw HTML */
                            set_status(b, b->viewsource ? "source" : ""); } break;
    case 'i': {         /* cert info: identity + validity of the current HTTPS page */
        if (b->cert_status == -2) { set_status(b, "not HTTPS"); break; }
        char s[40]; int p = 0;
        for (const char *cn = b->cert_cn[0] ? b->cert_cn : "?"; *cn && p < 18; cn++) s[p++] = *cn;
        const char *tail = " exp";
        for (const char *t = tail; *t && p < (int)sizeof s - 1; t++) s[p++] = *t;
        for (int k = 0; k < 6 && b->cert_expiry[k] && p < (int)sizeof s - 1; k++) s[p++] = b->cert_expiry[k];  /* YYMMDD */
        /* a loaded HTTPS page already passed hostname + validity enforcement;
         * "anchored" also means the chain reached a trusted root in our store. */
        const char *vfy = b->chain_ok ? " anchored" : (b->host_match == 1 ? " verified" : " ?");
        for (const char *v = vfy; *v && p < (int)sizeof s - 1; v++) s[p++] = *v;
        s[p] = 0; set_status(b, s);
        break; }
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
    b->zoom  = 1;                            /* default 1x (zoom persists across pages) */
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
