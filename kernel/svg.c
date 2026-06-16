/*
 * svg.c — a minimal-but-useful, integer-only SVG rasterizer.
 *
 * Decodes a useful subset of SVG (untrusted XML fetched from the web) into an
 * RGBA bitmap so the browser can render simple icons/logos inline. It is
 * freestanding: no libc beyond string.h, NO floating point — all geometry is
 * done in 16.16 signed fixed-point (`fx`). The caller passes the output bitmap
 * and a scratch buffer; svg.c allocates nothing.
 *
 * Pipeline:
 *   1. Find the <svg ...> tag; read width/height (px stripped) and/or viewBox.
 *      The canvas is W*H pixels; viewBox coords are mapped to pixels with a
 *      fixed-point scale (x and y scaled independently — aspect ratio ignored).
 *   2. Walk the XML element-by-element (a tiny, bounded tag scanner — this is
 *      NOT a real XML parser; it only needs to find shape tags + their attrs).
 *      For each supported shape (rect/circle/ellipse/line/polyline/polygon/path)
 *      build a list of subpath polygons in *device* fixed-point coords, then
 *      scanline-fill (even-odd) the fill colour and optionally stroke the edges.
 *
 * Supported: <rect> (incl. rx/ry rounded corners), <circle>, <ellipse>, <line>,
 *   <polyline>, <polygon>, <path d=...> (M/m L/l H/h V/v C/c Q/q Z/z; A treated
 *   as a line to its endpoint). Presentation attrs fill/stroke/stroke-width and a
 *   style="fill:..;stroke:.." are honoured. Colours: #rgb, #rrggbb, rgb(r,g,b),
 *   a small named-colour set, none, currentColor(->black). Default fill = black.
 *
 * <use href="#id" x= y=> (also xlink:href) instantiates a previously-defined
 *   element — a shape, or a <symbol>/<g> of shapes (usually in <defs>) — at the
 *   given offset; recursion is depth-capped so self/cyclic refs terminate.
 * Ignored (skipped gracefully): <image>/filters, CSS classes, transforms beyond
 *   viewBox (per-element + <g> transforms ARE honoured), fill-rule overrides.
 *
 * BOUNDS-SAFETY (untrusted input, no stack guard page):
 *   - Every scan loop is bounded by `len`; the tag scanner always advances.
 *   - W,H are capped (<= SVG_MAX_DIM) and W*H*4 must fit out_cap, so the bitmap
 *     index W*H can't overflow and every pixel write is range-checked anyway.
 *   - The point list, subpath count and path command count all have fixed caps
 *     and are bounds-checked on every push (overflow -> the shape is truncated,
 *     never an OOB).
 *   - Bezier subdivision is a fixed, small number of steps (no recursion).
 *   - Fixed-point coords are clamped to a safe range before use so the 16.16
 *     scale/multiply can't overflow 32 bits into a wild bitmap index.
 */
#include "svg.h"
#include "string.h"
#include "font.h"      /* font_glyphs[128][16] for <text> rendering */

/* ---- caps (documented) ------------------------------------------------- */
#define SVG_MAX_DIM    512      /* W,H each <= this */
#define SVG_MAX_PTS    8192     /* total polygon vertices buffered per shape */
#define SVG_MAX_SUBS   1024     /* subpaths per shape */
#define SVG_MAX_CMDS   16384    /* path 'd' commands processed (bounds the work) */
#define SVG_BEZ_STEPS  16       /* line segments per cubic/quadratic bezier */
#define SVG_MAX_TAGS   65536    /* hard ceiling on tags scanned (anti-hang) */
#define SVG_MAX_GROUPS 16       /* transform-stack depth for nested <g> (capped) */
#define SVG_MAX_GRADS  8        /* gradient definitions collected (capped) */
#define SVG_MAX_STOPS  8        /* color stops per gradient (capped) */
#define SVG_MAX_USE    4        /* <use> recursion depth cap (self/cyclic -> terminates) */

/* ---- 16.16 signed fixed-point ------------------------------------------ */
typedef int32_t fx;            /* value = real * 65536 */
#define FX_SHIFT 16
#define FX_ONE   (1 << FX_SHIFT)
/* Clamp the *integer* part of a fixed-point coord to keep |fx| < 2^30, so
 * fx*fx and scale math stay inside int64 and never wrap into a bad index. */
#define FX_CLAMP_INT 16383
static fx fx_from_int(int v) {
    if (v >  FX_CLAMP_INT) v =  FX_CLAMP_INT;
    if (v < -FX_CLAMP_INT) v = -FX_CLAMP_INT;
    return (fx)(v * FX_ONE);            /* multiply: v may be negative (<< is UB) */
}
static fx fx_clamp(fx v) {
    fx hi = (fx)FX_CLAMP_INT << FX_SHIFT;
    if (v >  hi) v =  hi;
    if (v < -hi) v = -hi;
    return v;
}
/* a*b with a 64-bit intermediate, result back in 16.16 (inputs pre-clamped). */
static fx fx_mul(fx a, fx b) { return (fx)(((int64_t)a * b) >> FX_SHIFT); }
/* Clamped add and reflection (2*a-b), done in int64 then clamped, so the path
 * accumulator arithmetic can never overflow the 32-bit fx type. */
static int64_t clamp64(int64_t v, int64_t hi) {   /* clamp v to [-hi, hi] */
    if (v >  hi) v =  hi;
    if (v < -hi) v = -hi;
    return v;
}
static fx fx_radd(fx a, fx b) {
    return (fx)clamp64((int64_t)a + b, (int64_t)FX_CLAMP_INT << FX_SHIFT);
}
static fx fx_reflect(fx a, fx b) {              /* 2*a - b, clamped */
    return (fx)clamp64((int64_t)2*a - b, (int64_t)FX_CLAMP_INT << FX_SHIFT);
}

/* fwd decls: the tiny char helpers are defined just below but are used by the
 * transform parser that sits above them. */
static int is_space(int c);
static int is_digit(int c);
static int lc(int c);
static int hexv(int c);

/* a*b in 16.16 with the result CLAMPED to the safe fx range. Unlike fx_mul (which
 * assumes pre-clamped inputs whose product fits), this is for transform matrices
 * whose entries can be large scales/translations — a huge product saturates to the
 * clamped extreme (an off-canvas coord, harmless) rather than wrapping a 32-bit fx. */
static fx fx_mulc(fx a, fx b) {
    return (fx)clamp64(((int64_t)a * b) >> FX_SHIFT, (int64_t)FX_CLAMP_INT << FX_SHIFT);
}

/* ---- sine/cosine from a small fixed-point quarter-wave table (no FPU) ---- *
 * SIN256[k] = round(sin(k*pi/128)*256) for k=0..64 (a quarter wave); the other
 * three quadrants are reflections. Used by <circle>/<ellipse> and rotate(). */
static const int SIN256[65] = {
    0,6,13,19,25,31,38,44,50,56,62,68,74,80,86,92,98,104,109,115,121,126,
    132,137,142,147,152,157,162,167,172,177,181,185,190,194,198,202,206,
    209,213,216,220,223,226,229,231,234,237,239,241,243,245,247,248,250,
    251,252,253,254,255,255,256,256,256 };
/* sin/cos of an angle in DEGREES (fx). Sub-degree precision is dropped (fine for
 * icon rendering). Outputs are fx in [-FX_ONE, FX_ONE]. */
static void sincos_deg(fx deg, fx *sinout, fx *cosout) {
    int d = (int)(deg >> FX_SHIFT);          /* integer degrees */
    d %= 360; if (d < 0) d += 360;
    int ang = (d * 256) / 360;               /* 0..255 ~ 0..2pi */
    int q = ang & 0xFF, s, co;
    if (q < 64)       { s =  SIN256[q];      co =  SIN256[64-q]; }
    else if (q < 128) { s =  SIN256[128-q];  co = -SIN256[q-64]; }
    else if (q < 192) { s = -SIN256[q-128];  co = -SIN256[192-q]; }
    else              { s = -SIN256[256-q];  co =  SIN256[q-192]; }
    *sinout = (fx)(s  * (FX_ONE/256));
    *cosout = (fx)(co * (FX_ONE/256));
}

/* ---- 2x3 affine transform matrices (16.16) ----------------------------- *
 * m[0..5] = a,b,c,d,e,f mapping (x,y) -> (a*x + c*y + e,  b*x + d*y + f). All
 * products use the clamped fx_mulc so a large scale/translate saturates rather
 * than overflowing a 32-bit fx into a wild coordinate/index. */
static void set_identity(fx *m) { m[0]=FX_ONE; m[1]=0; m[2]=0; m[3]=FX_ONE; m[4]=0; m[5]=0; }
/* out = A * B (B is applied to the point first). out may alias A or B. */
static void mat_mul(fx *out, const fx *A, const fx *B) {
    fx r[6];
    r[0] = fx_radd(fx_mulc(A[0],B[0]), fx_mulc(A[2],B[1]));
    r[1] = fx_radd(fx_mulc(A[1],B[0]), fx_mulc(A[3],B[1]));
    r[2] = fx_radd(fx_mulc(A[0],B[2]), fx_mulc(A[2],B[3]));
    r[3] = fx_radd(fx_mulc(A[1],B[2]), fx_mulc(A[3],B[3]));
    r[4] = fx_radd(fx_radd(fx_mulc(A[0],B[4]), fx_mulc(A[2],B[5])), A[4]);
    r[5] = fx_radd(fx_radd(fx_mulc(A[1],B[4]), fx_mulc(A[3],B[5])), A[5]);
    for (int i = 0; i < 6; i++) out[i] = r[i];
}
/* Compare nl chars at p to keyword k (case-insensitive); k must end exactly at nl. */
static int kw_eq(const char *p, int n, const char *k) {
    int i = 0;
    for (; i < n; i++) { if (!k[i] || lc((unsigned char)p[i]) != lc((unsigned char)k[i])) return 0; }
    return k[i] == 0;
}
/* Parse an SVG transform list ("translate(..) rotate(..) scale(..) matrix(..)
 * skewX/Y(..)") into *out, composed left-to-right (so a point is transformed by
 * the rightmost function first). A bounded scan; unknown functions are skipped. */
static fx parse_num(const char **pp, const char *end);   /* fwd (defined below) */
static void parse_transform(const char *str, int slen, fx *out) {
    set_identity(out);
    const char *p = str, *e = str + slen;
    int guard = 0;
    while (p < e && guard++ < 64) {
        while (p < e && (is_space((unsigned char)*p) || *p==',')) p++;
        if (p >= e) break;
        const char *ns = p;
        while (p < e && lc((unsigned char)*p) >= 'a' && lc((unsigned char)*p) <= 'z') p++;
        int nl = (int)(p - ns);
        while (p < e && is_space((unsigned char)*p)) p++;
        if (p >= e || *p != '(') { if (p < e) p++; continue; }
        p++;
        fx a[6]; int na = 0;
        while (p < e && *p != ')' && na < 6) {
            while (p < e && (is_space((unsigned char)*p) || *p==',')) p++;
            if (p >= e || *p == ')') break;
            const char *bf = p;
            a[na] = parse_num(&p, e);
            if (p == bf) { p++; continue; }       /* no progress -> advance, don't hang */
            na++;
        }
        while (p < e && *p != ')') p++;
        if (p < e) p++;                            /* consume ')' */

        fx m[6]; set_identity(m);
        if (nl == 9 && kw_eq(ns, nl, "translate")) {
            if (na >= 1) m[4] = a[0];
            if (na >= 2) m[5] = a[1];
        } else if (nl == 5 && kw_eq(ns, nl, "scale")) {
            if (na >= 1) { m[0] = a[0]; m[3] = (na >= 2) ? a[1] : a[0]; }
        } else if (nl == 6 && kw_eq(ns, nl, "matrix")) {
            if (na >= 6) { m[0]=a[0]; m[1]=a[1]; m[2]=a[2]; m[3]=a[3]; m[4]=a[4]; m[5]=a[5]; }
        } else if (nl == 6 && kw_eq(ns, nl, "rotate")) {
            if (na >= 1) {
                fx sn, cs; sincos_deg(a[0], &sn, &cs);
                fx R[6]; R[0]=cs; R[1]=sn; R[2]=-sn; R[3]=cs; R[4]=0; R[5]=0;
                if (na >= 3) {                     /* rotate(deg, cx, cy) about a point */
                    fx T1[6]; set_identity(T1); T1[4]=a[1];  T1[5]=a[2];
                    fx T2[6]; set_identity(T2); T2[4]=-a[1]; T2[5]=-a[2];
                    mat_mul(R, T1, R);             /* T(c) * R         */
                    mat_mul(R, R, T2);             /* T(c) * R * T(-c) */
                }
                for (int i = 0; i < 6; i++) m[i] = R[i];
            }
        } else if (nl == 5 && (kw_eq(ns, nl, "skewx") || kw_eq(ns, nl, "skewy"))) {
            if (na >= 1) {
                fx sn, cs; sincos_deg(a[0], &sn, &cs);
                /* tan = sin/cos in 16.16; multiply (not <<) — sn may be negative
                 * and a left shift of a negative value is undefined behaviour. */
                fx tn = cs ? (fx)clamp64(((int64_t)sn * FX_ONE) / cs,
                                         (int64_t)FX_CLAMP_INT << FX_SHIFT) : 0;
                if (lc((unsigned char)ns[4]) == 'x') m[2] = tn; else m[1] = tn;
            }
        } else {
            continue;                              /* unknown transform function */
        }
        mat_mul(out, out, m);                      /* compose left-to-right */
    }
}

/* ---- tiny char helpers (no ctype) -------------------------------------- */
static int is_space(int c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
static int is_digit(int c) { return c>='0'&&c<='9'; }
static int lc(int c) { return (c>='A'&&c<='Z') ? c+32 : c; }
static int hexv(int c) {
    if (c>='0'&&c<='9') return c-'0';
    c = lc(c);
    if (c>='a'&&c<='f') return c-'a'+10;
    return -1;
}

/* ---- number parsing (text -> 16.16 fixed-point) ------------------------ *
 * Accepts optional sign, an integer part, a fractional part, and a decimal
 * exponent (e/E). Bounded by `end`. Advances *pp past the number. The magnitude
 * is clamped so the result is always a valid, safe fx. No float anywhere. */
static fx parse_num(const char **pp, const char *end) {
    const char *p = *pp;
    while (p < end && (is_space(*p) || *p==',')) p++;
    int neg = 0;
    if (p < end && (*p=='+'||*p=='-')) { neg = (*p=='-'); p++; }

    int64_t ip = 0;            /* integer part */
    int saw = 0;
    while (p < end && is_digit(*p)) {
        if (ip < 100000000) ip = ip*10 + (*p-'0');   /* clamp; FX_CLAMP_INT bounds it anyway */
        p++; saw = 1;
    }
    int64_t frac = 0, fdiv = 1;  /* fractional part as frac/fdiv, capped to 6 digits */
    if (p < end && *p=='.') {
        p++;
        int fd = 0;
        while (p < end && is_digit(*p)) {
            if (fd < 6) { frac = frac*10 + (*p-'0'); fdiv *= 10; fd++; }
            p++; saw = 1;
        }
    }
    /* decimal exponent: shift the implied decimal point (small range only). */
    int expo = 0;
    if (saw && p < end && (*p=='e'||*p=='E')) {
        const char *q = p+1; int en = 0, ev = 0, eseen = 0;
        if (q < end && (*q=='+'||*q=='-')) { en = (*q=='-'); q++; }
        while (q < end && is_digit(*q)) { if (ev < 30) ev = ev*10 + (*q-'0'); q++; eseen = 1; }
        if (eseen) { expo = en ? -ev : ev; p = q; }
    }
    *pp = p;
    if (!saw) return 0;

    /* Build value*65536 in int64 then clamp. value = (ip + frac/fdiv) * 10^expo. */
    int64_t num = ip * fdiv + frac;          /* numerator over fdiv */
    int64_t den = fdiv;
    while (expo > 0 && num < (int64_t)1<<40) { num *= 10; expo--; }
    while (expo > 0) { expo--; if (den > 1) den /= 10; else num = (num > (1<<30) ? (1<<30) : num*10); }
    while (expo < 0 && den < (int64_t)1<<40) { den *= 10; expo++; }
    while (expo < 0) { expo++; num /= 10; if (num==0) break; }

    /* Cap num so `num << FX_SHIFT` can never overflow int64 (1<<46 << 16 = 1<<62).
     * The value clamps to +-FX_CLAMP_INT<<16 below anyway, so a saturated num
     * (huge magnitude attribute) just maps to the clamped extreme — never UB. */
    if (num > ((int64_t)1 << 46)) num = (int64_t)1 << 46;
    if (den < 1) den = 1;
    int64_t v = (num << FX_SHIFT) / den;
    if (neg) v = -v;
    int64_t hi = (int64_t)FX_CLAMP_INT << FX_SHIFT;   /* positive: -(x<<n) on a negative x is UB */
    if (v >  hi) v =  hi;
    if (v < -hi) v = -hi;
    return (fx)v;
}

/* ---- attribute extraction --------------------------------------------- *
 * Within the tag body [s,e), find attr `name` and copy its quoted value into
 * `buf` (NUL-terminated, dest-length capped). Returns 1 if found. A deliberately
 * simple matcher: it scans for the name preceded by a separator and followed by
 * '='. Good enough for shape attributes; never reads outside [s,e). */
static int get_attr(const char *s, const char *e, const char *name,
                    char *buf, int bufcap) {
    int nl = (int)strlen(name);
    for (const char *p = s; p + nl < e; p++) {
        /* name must start at a word boundary (start-of-body or after space/;/") */
        if (p != s) { char pc = p[-1]; if (!is_space(pc) && pc!=';' && pc!='"' && pc!='\'') continue; }
        int match = 1;
        for (int i = 0; i < nl; i++) if (lc((unsigned char)p[i]) != lc((unsigned char)name[i])) { match = 0; break; }
        if (!match) continue;
        const char *q = p + nl;
        while (q < e && is_space(*q)) q++;
        if (q >= e || *q != '=') continue;
        q++;
        while (q < e && is_space(*q)) q++;
        char quote = 0;
        if (q < e && (*q=='"' || *q=='\'')) { quote = *q; q++; }
        int n = 0;
        while (q < e && n < bufcap-1) {
            char c = *q;
            if (quote) { if (c == quote) break; }
            else if (is_space(c) || c=='>' || c=='/') break;
            buf[n++] = c; q++;
        }
        buf[n] = 0;
        return 1;
    }
    return 0;
}

/* Find a `key:value` inside a style="..." string (value -> buf). 1 if found. */
static int get_style(const char *style, const char *key, char *buf, int bufcap) {
    if (!style || !*style) return 0;
    int kl = (int)strlen(key);
    const char *e = style + strlen(style);
    for (const char *p = style; p + kl <= e; p++) {
        if (p != style) { char pc = p[-1]; if (!is_space(pc) && pc!=';') continue; }
        int match = 1;
        for (int i = 0; i < kl; i++) if (lc((unsigned char)p[i]) != lc((unsigned char)key[i])) { match = 0; break; }
        if (!match) continue;
        const char *q = p + kl;
        while (q < e && is_space(*q)) q++;
        if (q >= e || *q != ':') continue;
        q++;
        while (q < e && is_space(*q)) q++;
        int n = 0;
        while (q < e && *q != ';' && n < bufcap-1) buf[n++] = *q++;
        while (n > 0 && is_space((unsigned char)buf[n-1])) n--;   /* rtrim */
        buf[n] = 0;
        return 1;
    }
    return 0;
}

/* ---- colour parsing ---------------------------------------------------- */
typedef struct { uint8_t r, g, b, a; int set; int grad; } color_t;   /* set=0 -> "none"/unset; grad>0 -> gradient index+1 */

/* ---- gradients --------------------------------------------------------- *
 * A small fixed table of linear/radial gradients collected from the SVG (a
 * <linearGradient>/<radialGradient> with <stop> children). Coordinates are kept
 * in their declared form: for the default objectBoundingBox units they are 0..1
 * fractions of the filled shape's bounding box; for userSpaceOnUse they are user
 * coordinates (mapped to device like any point). */
typedef struct { fx off; uint8_t r, g, b, a; } gstop_t;   /* offset 0..1 (fx) + colour */
typedef struct {
    char    id[40];
    int     radial;        /* 0 = linear, 1 = radial */
    int     userspace;     /* 0 = objectBoundingBox (default), 1 = userSpaceOnUse */
    fx      x1, y1, x2, y2; /* linear axis; radial: (x1,y1)=center, x2=radius */
    gstop_t stops[SVG_MAX_STOPS];
    int     nstops;
} grad_t;

struct named { const char *name; uint8_t r, g, b; };
static const struct named NAMED[] = {
    {"black",0,0,0}, {"white",255,255,255}, {"red",255,0,0}, {"green",0,128,0},
    {"lime",0,255,0}, {"blue",0,0,255}, {"yellow",255,255,0}, {"cyan",0,255,255},
    {"aqua",0,255,255}, {"magenta",255,0,255}, {"fuchsia",255,0,255},
    {"gray",128,128,128}, {"grey",128,128,128}, {"silver",192,192,192},
    {"maroon",128,0,0}, {"olive",128,128,0}, {"navy",0,0,128}, {"teal",0,128,128},
    {"purple",128,0,128}, {"orange",255,165,0}, {"pink",255,192,203},
    {"brown",165,42,42}, {"gold",255,215,0}, {"darkgray",169,169,169},
    {"darkgrey",169,169,169}, {"lightgray",211,211,211}, {"lightgrey",211,211,211},
    {"transparent",0,0,0},
};

/* Parse a colour spec into *c. `dflt_set` chooses what an empty/garbage string
 * means (default fill is opaque black; default stroke is "unset"). */
static void parse_color(const char *s, color_t *c, int dflt_set) {
    c->r = c->g = c->b = 0; c->a = 255; c->set = dflt_set; c->grad = 0;
    if (!s || !*s) return;
    while (is_space((unsigned char)*s)) s++;
    int n = (int)strlen(s);

    if (s[0] == '#') {
        const char *h = s + 1;
        int hl = 0; while (h[hl] && hexv((unsigned char)h[hl]) >= 0) hl++;
        if (hl >= 6) {
            int r = hexv(h[0])*16+hexv(h[1]), g = hexv(h[2])*16+hexv(h[3]), b = hexv(h[4])*16+hexv(h[5]);
            c->r=(uint8_t)r; c->g=(uint8_t)g; c->b=(uint8_t)b; c->a=255; c->set=1;
        } else if (hl >= 3) {
            int r = hexv(h[0]), g = hexv(h[1]), b = hexv(h[2]);
            c->r=(uint8_t)(r*17); c->g=(uint8_t)(g*17); c->b=(uint8_t)(b*17); c->a=255; c->set=1;
        }
        return;
    }
    if ((n>=4) && (lc(s[0])=='r'&&lc(s[1])=='g'&&lc(s[2])=='b')) {   /* rgb(...) / rgba(...) */
        const char *p = s + 3;
        const char *e = s + n;
        while (p < e && *p != '(') p++;
        if (p < e) p++;
        fx rr = parse_num(&p, e), gg = parse_num(&p, e), bb = parse_num(&p, e);
        int ri = rr >> FX_SHIFT, gi = gg >> FX_SHIFT, bi = bb >> FX_SHIFT;
        if (ri<0) ri=0;
        if (ri>255) ri=255;
        if (gi<0) gi=0;
        if (gi>255) gi=255;
        if (bi<0) bi=0;
        if (bi>255) bi=255;
        c->r=(uint8_t)ri; c->g=(uint8_t)gi; c->b=(uint8_t)bi; c->a=255; c->set=1;
        return;
    }
    if (lc(s[0])=='n'&&lc(s[1])=='o'&&lc(s[2])=='n'&&lc(s[3])=='e') { c->set = 0; return; }   /* none */
    /* currentColor -> black (we have no inherited colour context) */
    if (lc(s[0])=='c'&&lc(s[1])=='u') { c->r=c->g=c->b=0; c->a=255; c->set=1; return; }

    for (unsigned i = 0; i < sizeof(NAMED)/sizeof(NAMED[0]); i++) {
        const char *nm = NAMED[i].name; int j = 0;
        while (nm[j] && lc((unsigned char)s[j]) == lc((unsigned char)nm[j])) j++;
        if (nm[j] == 0 && (s[j] == 0)) {
            c->r=NAMED[i].r; c->g=NAMED[i].g; c->b=NAMED[i].b;
            c->a = (i + 1 == sizeof(NAMED)/sizeof(NAMED[0])) ? 0 : 255;  /* "transparent" */
            c->set = 1;
            return;
        }
    }
    /* gradient/pattern url(...) or anything unknown -> mid gray (so it shows up) */
    if (lc(s[0])=='u'&&lc(s[1])=='r'&&lc(s[2])=='l') { c->r=c->g=c->b=128; c->a=255; c->set=1; }
}

/* ---- the render context ------------------------------------------------ */
typedef struct {
    uint8_t *out;            /* W*H RGBA */
    int W, H;
    fx sx, sy;               /* viewBox -> pixel scale (16.16) */
    fx ox, oy;               /* viewBox min (subtracted before scaling), 16.16 */
    /* point list for the shape currently being built (device fixed-point) */
    fx  *px;                 /* [SVG_MAX_PTS] x coords (16.16, device space) */
    fx  *py;                 /* [SVG_MAX_PTS] y coords */
    int  npt;
    int  sub_start[SVG_MAX_SUBS + 1];  /* index where each subpath begins */
    int  nsub;
    /* current transform matrix (user coords are run through this BEFORE the
     * viewBox->pixel map) + a stack saved/restored across nested <g> groups. */
    fx   m[6];
    fx   gstack[SVG_MAX_GROUPS][6];
    int  gdepth;
    /* current INHERITED paint (seeded by <svg>, overridden by <g>, used as the
     * default when a shape omits fill/stroke/stroke-width) + its group stack. */
    color_t in_fill, in_stroke;
    fx      in_swid;
    color_t fillstk[SVG_MAX_GROUPS], strokestk[SVG_MAX_GROUPS];
    fx      swidstk[SVG_MAX_GROUPS];
    /* inherited group opacity (0..255 multiplier), accumulated down <g> nesting. */
    int     in_alpha;
    int     alphastk[SVG_MAX_GROUPS];
    /* gradient definitions, collected in a pre-pass and referenced by fill=url(#id). */
    grad_t  grads[SVG_MAX_GRADS];
    int     ngrad;
    /* full document bounds (set in svg_decode) so <use href="#id"> can locate a
     * referenced definition anywhere in the markup via find_def(). */
    const char *doc, *docend;
} ctx_t;

/* Map a user-space coord (16.16) to device (pixel) fixed-point. The subtraction
 * is done in int64 + clamped before fx_mul so neither (ux-ox) nor the scale
 * multiply can overflow the 32-bit fx type. */
static fx map_x(ctx_t *c, fx ux) {
    int64_t d = clamp64((int64_t)ux - c->ox, (int64_t)FX_CLAMP_INT << FX_SHIFT);
    return fx_mul((fx)d, c->sx);
}
static fx map_y(ctx_t *c, fx uy) {
    int64_t d = clamp64((int64_t)uy - c->oy, (int64_t)FX_CLAMP_INT << FX_SHIFT);
    return fx_mul((fx)d, c->sy);
}
/* Apply the current transform matrix to a user-space point (clamped). */
static fx ctm_x(ctx_t *c, fx ux, fx uy) {
    int64_t v = (int64_t)fx_mulc(c->m[0], ux) + fx_mulc(c->m[2], uy) + c->m[4];
    return (fx)clamp64(v, (int64_t)FX_CLAMP_INT << FX_SHIFT);
}
static fx ctm_y(ctx_t *c, fx ux, fx uy) {
    int64_t v = (int64_t)fx_mulc(c->m[1], ux) + fx_mulc(c->m[3], uy) + c->m[5];
    return (fx)clamp64(v, (int64_t)FX_CLAMP_INT << FX_SHIFT);
}

/* Append a device-space point to the current subpath (bounds-checked). */
static void push_pt(ctx_t *c, fx dx, fx dy) {
    if (c->npt >= SVG_MAX_PTS) return;     /* cap: silently truncate the shape */
    c->px[c->npt] = fx_clamp(dx);
    c->py[c->npt] = fx_clamp(dy);
    c->npt++;
}
/* Append a user-space point: run it through the current transform matrix, then
 * map viewBox->device. (Identity CTM => byte-for-byte the old map-only path.) */
static void push_user(ctx_t *c, fx ux, fx uy) {
    fx tx = ctm_x(c, ux, uy), ty = ctm_y(c, ux, uy);
    push_pt(c, map_x(c, tx), map_y(c, ty));
}

/* Begin a new subpath at the current point count. */
static void begin_sub(ctx_t *c) {
    if (c->nsub >= SVG_MAX_SUBS) return;
    c->sub_start[c->nsub++] = c->npt;
}
static void reset_shape(ctx_t *c) { c->npt = 0; c->nsub = 0; begin_sub(c); }
static void end_shape(ctx_t *c) {
    if (c->nsub <= SVG_MAX_SUBS) c->sub_start[c->nsub] = c->npt;   /* sentinel end */
}

/* ---- compositing ------------------------------------------------------- */
static void blend_px(ctx_t *c, int x, int y, color_t col, int a) {
    if (x < 0 || y < 0 || x >= c->W || y >= c->H) return;   /* hard bitmap clamp */
    if (a <= 0) return;
    if (a > 255) a = 255;
    uint8_t *o = c->out + ((long)y * c->W + x) * 4;
    int dr=o[0], dg=o[1], db=o[2], da=o[3];
    /* src-over with straight alpha (integer): out = src*a + dst*(255-a)/255 */
    int na = a + da*(255-a)/255;
    int nr = (col.r*a + dr*da*(255-a)/255) / (na ? na : 1);
    int ng = (col.g*a + dg*da*(255-a)/255) / (na ? na : 1);
    int nb = (col.b*a + db*da*(255-a)/255) / (na ? na : 1);
    if (nr>255) nr=255;
    if (ng>255) ng=255;
    if (nb>255) nb=255;
    if (na>255) na=255;
    o[0]=(uint8_t)nr; o[1]=(uint8_t)ng; o[2]=(uint8_t)nb; o[3]=(uint8_t)na;
}

/* ---- gradient evaluation ----------------------------------------------- */
/* integer sqrt of a non-negative int64 (Newton; bounded iterations, no FPU). */
static int64_t isqrt64(int64_t v) {
    if (v <= 0) return 0;
    int64_t x = v, nx = (x + 1) / 2;
    for (int it = 0; it < 100 && nx < x; it++) { x = nx; nx = (x + v / x) / 2; }
    return x;
}
/* lerp two 0..255 channels by f (16.16, 0..1), clamped to 0..255. */
static uint8_t lerp8(int a, int b, fx f) {
    int v = a + (int)(((int64_t)(b - a) * f) >> FX_SHIFT);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}
/* Colour of gradient `gi` at device pixel (px,py); the filled shape's device-space
 * bounding box [bx0,by0]-[bx1,by1] maps objectBoundingBox (0..1) coords. Returns the
 * interpolated stop colour+alpha. All fixed-point with int64 intermediates; the
 * projection scale is reduced by 2^16 before dividing so num*FX_ONE can't overflow. */
static color_t grad_color_at(ctx_t *c, int gi, fx bx0, fx by0, fx bx1, fx by1, int px, int py) {
    color_t out; out.r = out.g = out.b = 0; out.a = 255; out.set = 1; out.grad = 0;
    /* defensive: the sole caller passes gi = col.grad-1 in [0,ngrad), but re-check
     * so the grads[] index is provably in range even if a future caller slips up. */
    if (gi < 0 || gi >= c->ngrad || gi >= SVG_MAX_GRADS) return out;
    grad_t *g = &c->grads[gi];
    if (g->nstops == 0) return out;

    int64_t Px = ((int64_t)px << FX_SHIFT) + FX_ONE / 2;   /* pixel centre, device fx */
    int64_t Py = ((int64_t)py << FX_SHIFT) + FX_ONE / 2;

    /* resolve gradient geometry to device fx */
    int64_t ax, ay, bxx, byy, cxx, cyy, rr;
    if (!g->userspace) {                       /* objectBoundingBox: 0..1 of the bbox */
        int64_t bw = (int64_t)bx1 - bx0, bh = (int64_t)by1 - by0;
        ax  = (int64_t)bx0 + fx_mulc(g->x1, (fx)clamp64(bw, (int64_t)FX_CLAMP_INT << FX_SHIFT));
        ay  = (int64_t)by0 + fx_mulc(g->y1, (fx)clamp64(bh, (int64_t)FX_CLAMP_INT << FX_SHIFT));
        bxx = (int64_t)bx0 + fx_mulc(g->x2, (fx)clamp64(bw, (int64_t)FX_CLAMP_INT << FX_SHIFT));
        byy = (int64_t)by0 + fx_mulc(g->y2, (fx)clamp64(bh, (int64_t)FX_CLAMP_INT << FX_SHIFT));
        cxx = ax; cyy = ay;
        rr  = fx_mulc(g->x2, (fx)clamp64((bw + bh) / 2, (int64_t)FX_CLAMP_INT << FX_SHIFT));
    } else {                                   /* userSpaceOnUse: user coords -> device */
        ax  = map_x(c, g->x1); ay  = map_y(c, g->y1);
        bxx = map_x(c, g->x2); byy = map_y(c, g->y2);
        cxx = ax; cyy = ay;
        rr  = fx_mulc(g->x2, c->sx);
    }
    /* Clamp the resolved geometry to a generous device range (>> any real <=512px
     * canvas, so visible gradients are unaffected) BEFORE the dx/dy squaring below:
     * unclamped, a malicious huge-coordinate SVG could make dx*dx overflow int64.
     * 1<<28 keeps dx in +-2^29 so dx*dx + dy*dy stays well within int64. */
    {
        const int64_t GC = (int64_t)1 << 28;
        ax = clamp64(ax, GC); ay = clamp64(ay, GC);
        bxx = clamp64(bxx, GC); byy = clamp64(byy, GC);
        cxx = clamp64(cxx, GC); cyy = clamp64(cyy, GC);
    }

    fx t;                                       /* position along the gradient, 0..1 (fx) */
    if (!g->radial) {
        int64_t dx = bxx - ax, dy = byy - ay;
        int64_t lensq = (dx * dx + dy * dy) >> FX_SHIFT;   /* reduce 2^32 scale by 2^16 */
        if (lensq <= 0) t = 0;
        else {
            int64_t num = ((Px - ax) * dx + (Py - ay) * dy) >> FX_SHIFT;
            int64_t tt = (num * FX_ONE) / lensq;
            t = (fx)clamp64(tt, (int64_t)FX_ONE);
        }
    } else {
        int64_t dx = Px - cxx, dy = Py - cyy;
        int64_t dist = isqrt64(dx * dx + dy * dy);          /* device fx distance */
        if (rr <= 0) t = FX_ONE;
        else { int64_t tt = (dist * FX_ONE) / rr; t = (fx)clamp64(tt, (int64_t)FX_ONE); }
    }
    if (t < 0) t = 0;

    /* find the stop pair bracketing t and interpolate (stops assumed in offset order). */
    gstop_t *st = g->stops; int n = g->nstops;
    if (t <= st[0].off)   { out.r=st[0].r;   out.g=st[0].g;   out.b=st[0].b;   out.a=st[0].a;   return out; }
    if (t >= st[n-1].off) { out.r=st[n-1].r; out.g=st[n-1].g; out.b=st[n-1].b; out.a=st[n-1].a; return out; }
    for (int i = 0; i + 1 < n; i++) {
        if (t >= st[i].off && t <= st[i+1].off) {
            fx span = st[i+1].off - st[i].off;
            fx f = span > 0 ? (fx)(((int64_t)(t - st[i].off) * FX_ONE) / span) : 0;
            out.r = lerp8(st[i].r, st[i+1].r, f);
            out.g = lerp8(st[i].g, st[i+1].g, f);
            out.b = lerp8(st[i].b, st[i+1].b, f);
            out.a = lerp8(st[i].a, st[i+1].a, f);
            return out;
        }
    }
    out.r=st[n-1].r; out.g=st[n-1].g; out.b=st[n-1].b; out.a=st[n-1].a;   /* fallthrough */
    return out;
}

/* ---- scanline polygon fill (even-odd) ---------------------------------- *
 * Fills all current subpaths together (even-odd rule, so holes work). For each
 * scanline center y+0.5 we gather edge crossings (fixed-point x), insertion-sort
 * them, and fill between pairs. Everything is bounded by W/H and the point cap. */
#define MAX_XS 4096
static void insort(fx *a, int n) {                 /* tiny insertion sort */
    for (int i = 1; i < n; i++) { fx v = a[i]; int j = i-1;
        while (j >= 0 && a[j] > v) { a[j+1] = a[j]; j--; } a[j+1] = v; }
}
static void fill_poly(ctx_t *c, color_t col) {
    if (!col.set || c->npt < 3) return;
    /* device-space bounding box: vertical extent -> scanlines; full box -> gradient mapping */
    fx ymin = c->py[0], ymax = c->py[0], xmin = c->px[0], xmax = c->px[0];
    for (int i = 1; i < c->npt; i++) {
        if (c->py[i]<ymin) ymin=c->py[i]; if (c->py[i]>ymax) ymax=c->py[i];
        if (c->px[i]<xmin) xmin=c->px[i]; if (c->px[i]>xmax) xmax=c->px[i];
    }
    int y0 = ymin >> FX_SHIFT, y1 = (ymax >> FX_SHIFT) + 1;
    if (y0 < 0) y0 = 0;
    if (y1 > c->H) y1 = c->H;

    for (int y = y0; y < y1; y++) {
        fx yc = ((fx)y << FX_SHIFT) + (FX_ONE/2);    /* scanline center */
        fx xs[MAX_XS]; int nx = 0;
        for (int s = 0; s < c->nsub && s < SVG_MAX_SUBS; s++) {
            int a = c->sub_start[s], b = c->sub_start[s+1];
            if (b - a < 2) continue;
            for (int i = a; i < b; i++) {
                int j = (i+1 < b) ? i+1 : a;          /* edge (i -> j), auto-closed */
                fx ay = c->py[i], by = c->py[j], ax = c->px[i], bx = c->px[j];
                if (ay == by) continue;               /* horizontal edge: ignore */
                fx lo = ay < by ? ay : by, hi = ay < by ? by : ay;
                if (yc < lo || yc >= hi) continue;    /* half-open span -> no double count */
                /* x = ax + (bx-ax) * (yc-ay)/(by-ay), all 16.16 via int64.
                 * Multiply (not <<) — (yc-ay) is signed and a left shift of a
                 * negative value is undefined behaviour. */
                int64_t t = ((int64_t)(yc - ay) * FX_ONE) / (by - ay);      /* 16.16 */
                int64_t x = (int64_t)ax + (((int64_t)(bx - ax) * t) >> FX_SHIFT);
                if (nx < MAX_XS) xs[nx++] = (fx)x;
            }
        }
        if (nx < 2) continue;
        insort(xs, nx);
        for (int k = 0; k + 1 < nx; k += 2) {         /* even-odd: fill pairs */
            int xa = (xs[k]   + (FX_ONE/2)) >> FX_SHIFT;    /* round to pixel centers */
            int xb = (xs[k+1] + (FX_ONE/2)) >> FX_SHIFT;
            if (xa < 0) xa = 0;
            if (xb > c->W) xb = c->W;
            if (col.grad) {                            /* gradient fill: per-pixel colour */
                int gi = col.grad - 1;
                for (int x = xa; x < xb; x++) {
                    color_t gc = grad_color_at(c, gi, xmin, ymin, xmax, ymax, x, y);
                    blend_px(c, x, y, gc, gc.a * (int)col.a / 255);
                }
            } else {                                   /* solid fill (unchanged path) */
                for (int x = xa; x < xb; x++) blend_px(c, x, y, col, col.a);
            }
        }
    }
}

/* ---- stroke: thin line along every edge (Bresenham, half-width box) ----- */
static void plot_disc(ctx_t *c, int cx, int cy, int r, color_t col) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r) blend_px(c, cx+dx, cy+dy, col, col.a);
}
static void stroke_line(ctx_t *c, fx x0f, fx y0f, fx x1f, fx y1f, int r, color_t col) {
    int x0 = x0f>>FX_SHIFT, y0 = y0f>>FX_SHIFT, x1 = x1f>>FX_SHIFT, y1 = y1f>>FX_SHIFT;
    int dx = x1-x0, dy = y1-y0;
    int adx = dx<0?-dx:dx, ady = dy<0?-dy:dy;
    int sx = dx<0?-1:1, sy = dy<0?-1:1;
    int err = adx - ady;
    int guard = c->W + c->H + 4;                  /* anti-hang: line can't be longer */
    for (int n = 0; n <= adx + ady + 1 && n < guard*2; n++) {
        plot_disc(c, x0, y0, r, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2*err;
        if (e2 > -ady) { err -= ady; x0 += sx; }
        if (e2 <  adx) { err += adx; y0 += sy; }
    }
}
static void stroke_poly(ctx_t *c, color_t col, int closed, fx swid) {
    if (!col.set || c->npt < 2) return;
    int r = (swid >> (FX_SHIFT+1));               /* half stroke-width in px */
    if (r < 0) r = 0;
    if (r > 16) r = 16;                           /* cap stroke radius */
    for (int s = 0; s < c->nsub && s < SVG_MAX_SUBS; s++) {
        int a = c->sub_start[s], b = c->sub_start[s+1];
        for (int i = a; i + 1 < b; i++)
            stroke_line(c, c->px[i], c->py[i], c->px[i+1], c->py[i+1], r, col);
        if (closed && b - a >= 2)
            stroke_line(c, c->px[b-1], c->py[b-1], c->px[a], c->py[a], r, col);
    }
}

/* ---- bezier flattening (fixed-point, fixed step count) ------------------ */
/* Cubic: append SVG_BEZ_STEPS points from p1 to p3 via control p1c,p2c.
 * All inputs are USER-space fx; output points are pushed (mapped to device). */
static void flatten_cubic(ctx_t *c, fx x0, fx y0, fx x1, fx y1,
                          fx x2, fx y2, fx x3, fx y3) {
    for (int i = 1; i <= SVG_BEZ_STEPS; i++) {
        fx t = ((fx)i << FX_SHIFT) / SVG_BEZ_STEPS;     /* 0..1 in 16.16 */
        fx mt = FX_ONE - t;
        fx t2 = fx_mul(t,t),  t3 = fx_mul(t2,t);
        fx m2 = fx_mul(mt,mt), m3 = fx_mul(m2,mt);
        fx c0 = m3, c1 = fx_mul(fx_from_int(3), fx_mul(m2,t)),
           c2 = fx_mul(fx_from_int(3), fx_mul(mt,t2)), c3 = t3;
        fx x = fx_mul(c0,x0)+fx_mul(c1,x1)+fx_mul(c2,x2)+fx_mul(c3,x3);
        fx y = fx_mul(c0,y0)+fx_mul(c1,y1)+fx_mul(c2,y2)+fx_mul(c3,y3);
        push_user(c, x, y);
    }
}
static void flatten_quad(ctx_t *c, fx x0, fx y0, fx x1, fx y1, fx x2, fx y2) {
    for (int i = 1; i <= SVG_BEZ_STEPS; i++) {
        fx t = ((fx)i << FX_SHIFT) / SVG_BEZ_STEPS;
        fx mt = FX_ONE - t;
        fx m2 = fx_mul(mt,mt), t2 = fx_mul(t,t), c1 = fx_mul(fx_from_int(2), fx_mul(mt,t));
        fx x = fx_mul(m2,x0)+fx_mul(c1,x1)+fx_mul(t2,x2);
        fx y = fx_mul(m2,y0)+fx_mul(c1,y1)+fx_mul(t2,y2);
        push_user(c, x, y);
    }
}

/* ---- shape builders (fill the point list, then the caller rasterizes) --- */
static void build_rect(ctx_t *c, const char *s, const char *e) {
    char v[48]; fx x=0,y=0,w=0,h=0,rx=0,ry=0;
    if (get_attr(s,e,"x",v,sizeof v))      { const char*p=v; x = parse_num(&p, v+strlen(v)); }
    if (get_attr(s,e,"y",v,sizeof v))      { const char*p=v; y = parse_num(&p, v+strlen(v)); }
    if (get_attr(s,e,"width",v,sizeof v))  { const char*p=v; w = parse_num(&p, v+strlen(v)); }
    if (get_attr(s,e,"height",v,sizeof v)) { const char*p=v; h = parse_num(&p, v+strlen(v)); }
    if (w <= 0 || h <= 0) { reset_shape(c); return; }
    int has_rx = get_attr(s,e,"rx",v,sizeof v); if (has_rx) { const char*p=v; rx = parse_num(&p, v+strlen(v)); }
    int has_ry = get_attr(s,e,"ry",v,sizeof v); if (has_ry) { const char*p=v; ry = parse_num(&p, v+strlen(v)); }
    if (has_rx && !has_ry) ry = rx;
    if (has_ry && !has_rx) rx = ry;
    if (rx > w/2) rx = w/2;
    if (ry > h/2) ry = h/2;
    /* all additions via fx_radd (clamped int64) — x/y/w/h are each <= +-16383,
     * so x+w could otherwise reach the int32 edge. */
    fx l=fx_clamp(x), t=fx_clamp(y), rgt=fx_radd(x,w), bot=fx_radd(y,h);
    reset_shape(c);
    if (rx <= 0 || ry <= 0) {                      /* plain rectangle */
        push_user(c, l,   t);
        push_user(c, rgt, t);
        push_user(c, rgt, bot);
        push_user(c, l,   bot);
        return;
    }
    /* rounded rect: 4 quarter-arc corners approximated with bezier steps */
    flatten_quad(c, fx_radd(l,rx), t,  l, t,  l, fx_radd(t,ry));  /* approximate corner */
    push_user(c, l, fx_radd(bot,-ry));
    flatten_quad(c, l, bot,  l, bot,  fx_radd(l,rx), bot);
    push_user(c, fx_radd(rgt,-rx), bot);
    flatten_quad(c, rgt, bot,  rgt, bot,  rgt, fx_radd(bot,-ry));
    push_user(c, rgt, fx_radd(t,ry));
    flatten_quad(c, rgt, t,  rgt, t,  fx_radd(rgt,-rx), t);
    push_user(c, fx_radd(l,rx), t);
}
static void build_ellipse(ctx_t *c, fx cx, fx cy, fx rx, fx ry) {
    if (rx <= 0 || ry <= 0) { reset_shape(c); return; }
    reset_shape(c);
    int steps = 48;
    /* direct parametric circle/ellipse via the file-scope SIN256 quarter table. */
    for (int i = 0; i <= steps; i++) {
        int ang = (i * 256) / steps;        /* 0..256 == 0..2pi */
        int s, co;
        /* sine via quadrant reflection of the quarter table (index 0..64). */
        int q = ang & 0xFF;
        if (q < 64)       { s =  SIN256[q];        co =  SIN256[64-q]; }
        else if (q < 128) { s =  SIN256[128-q];    co = -SIN256[q-64]; }
        else if (q < 192) { s = -SIN256[q-128];    co = -SIN256[192-q]; }
        else              { s = -SIN256[256-q];    co =  SIN256[q-192]; }
        /* co/s are signed (sine table) — multiply, never left-shift (UB).
         * fx_radd keeps cx+offset inside the safe int32 fx range. */
        fx ux = fx_radd(cx, fx_mul(rx, (fx)(co * (FX_ONE/256))));
        fx uy = fx_radd(cy, fx_mul(ry, (fx)(s  * (FX_ONE/256))));
        push_user(c, ux, uy);
    }
}

/* ---- polyline / polygon points="x,y x,y ..." --------------------------- */
static void build_points(ctx_t *c, const char *s, const char *e) {
    char v[1024];
    if (!get_attr(s,e,"points",v,sizeof v)) { reset_shape(c); return; }
    reset_shape(c);
    const char *p = v, *pe = v + strlen(v);
    int guard = 0;
    while (p < pe && guard++ < SVG_MAX_PTS) {
        while (p < pe && (is_space(*p) || *p==',')) p++;
        if (p >= pe) break;
        const char *before = p;
        fx px = parse_num(&p, pe);
        fx py = parse_num(&p, pe);
        if (p == before) break;             /* no progress -> stop */
        push_user(c, px, py);
    }
}

/* ---- <path d="..."> ---------------------------------------------------- *
 * A bounded command parser. Tracks current point (cx,cy) and the subpath start
 * (for Z) in user space. M starts a new subpath; Z closes it. Beziers are
 * flattened. 'A' (arc) is approximated as a straight line to its endpoint. */
static void build_path(ctx_t *c, const char *s, const char *e) {
    char d[8192];
    if (!get_attr(s,e,"d",d,sizeof d)) { reset_shape(c); return; }
    reset_shape(c);
    const char *p = d, *pe = d + strlen(d);
    fx cx = 0, cy = 0;        /* current point (user space) */
    fx startx = 0, starty = 0;
    fx px2 = 0, py2 = 0;      /* last control point (for smooth S/T — approximated) */
    (void)px2; (void)py2;
    char cmd = 0;
    int first_move = 1;
    int cmds = 0;

    while (p < pe && cmds < SVG_MAX_CMDS) {
        while (p < pe && (is_space(*p) || *p==',')) p++;
        if (p >= pe) break;
        char ch = *p;
        if ((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')) { cmd = ch; p++; }
        else if (cmd == 0) { p++; continue; }     /* number with no command yet */
        int rel = (cmd >= 'a');
        char C = (char)(cmd & ~0x20);             /* uppercase form */
        cmds++;
        const char *before = p;

        /* All coords clamped via fx_radd / fx_clamp so the int32 fx arithmetic
         * (relative adds, the 2*cx control-point reflection) can never overflow
         * — fx values stay within +-FX_CLAMP_INT<<16, well inside int32. */
        if (C == 'M') {
            fx nx = parse_num(&p,pe), ny = parse_num(&p,pe);
            if (rel) { nx = fx_radd(nx,cx); ny = fx_radd(ny,cy); }
            cx = fx_clamp(nx); cy = fx_clamp(ny); startx = cx; starty = cy;
            px2 = cx; py2 = cy;
            if (first_move) { reset_shape(c); first_move = 0; }
            else begin_sub(c);
            push_user(c, cx, cy);
            cmd = (char)(rel ? 'l' : 'L');        /* subsequent coords are implicit L */
        } else if (C == 'L') {
            fx nx = parse_num(&p,pe), ny = parse_num(&p,pe);
            if (rel) { nx = fx_radd(nx,cx); ny = fx_radd(ny,cy); }
            cx = fx_clamp(nx); cy = fx_clamp(ny); px2=cx; py2=cy; push_user(c, cx, cy);
        } else if (C == 'H') {
            fx nx = parse_num(&p,pe); if (rel) nx = fx_radd(nx,cx); cx = fx_clamp(nx); px2=cx; py2=cy; push_user(c, cx, cy);
        } else if (C == 'V') {
            fx ny = parse_num(&p,pe); if (rel) ny = fx_radd(ny,cy); cy = fx_clamp(ny); px2=cx; py2=cy; push_user(c, cx, cy);
        } else if (C == 'C') {
            fx x1=parse_num(&p,pe), y1=parse_num(&p,pe), x2=parse_num(&p,pe),
               y2=parse_num(&p,pe), x3=parse_num(&p,pe), y3=parse_num(&p,pe);
            if (rel) { x1=fx_radd(x1,cx);y1=fx_radd(y1,cy); x2=fx_radd(x2,cx);y2=fx_radd(y2,cy); x3=fx_radd(x3,cx);y3=fx_radd(y3,cy); }
            flatten_cubic(c, cx,cy, x1,y1, x2,y2, x3,y3);
            px2=fx_clamp(x2); py2=fx_clamp(y2); cx=fx_clamp(x3); cy=fx_clamp(y3);
        } else if (C == 'S') {                    /* smooth cubic: reflect prev ctrl */
            fx x2=parse_num(&p,pe), y2=parse_num(&p,pe), x3=parse_num(&p,pe), y3=parse_num(&p,pe);
            if (rel) { x2=fx_radd(x2,cx);y2=fx_radd(y2,cy); x3=fx_radd(x3,cx);y3=fx_radd(y3,cy); }
            fx x1 = fx_reflect(cx, px2), y1 = fx_reflect(cy, py2);
            flatten_cubic(c, cx,cy, x1,y1, x2,y2, x3,y3);
            px2=fx_clamp(x2); py2=fx_clamp(y2); cx=fx_clamp(x3); cy=fx_clamp(y3);
        } else if (C == 'Q') {
            fx x1=parse_num(&p,pe), y1=parse_num(&p,pe), x2=parse_num(&p,pe), y2=parse_num(&p,pe);
            if (rel) { x1=fx_radd(x1,cx);y1=fx_radd(y1,cy); x2=fx_radd(x2,cx);y2=fx_radd(y2,cy); }
            flatten_quad(c, cx,cy, x1,y1, x2,y2);
            px2=fx_clamp(x1); py2=fx_clamp(y1); cx=fx_clamp(x2); cy=fx_clamp(y2);
        } else if (C == 'T') {                    /* smooth quad */
            fx x2=parse_num(&p,pe), y2=parse_num(&p,pe);
            if (rel) { x2=fx_radd(x2,cx); y2=fx_radd(y2,cy); }
            fx x1 = fx_reflect(cx, px2), y1 = fx_reflect(cy, py2);
            flatten_quad(c, cx,cy, x1,y1, x2,y2);
            px2=fx_clamp(x1); py2=fx_clamp(y1); cx=fx_clamp(x2); cy=fx_clamp(y2);
        } else if (C == 'A') {                    /* arc -> line to endpoint (approx) */
            parse_num(&p,pe); parse_num(&p,pe); parse_num(&p,pe);   /* rx ry x-rot */
            parse_num(&p,pe); parse_num(&p,pe);                     /* large-arc sweep */
            fx nx = parse_num(&p,pe), ny = parse_num(&p,pe);
            if (rel) { nx = fx_radd(nx,cx); ny = fx_radd(ny,cy); }
            cx = fx_clamp(nx); cy = fx_clamp(ny); px2=cx; py2=cy; push_user(c, cx, cy);
        } else if (C == 'Z') {
            cx = startx; cy = starty;
            begin_sub(c);                          /* a Z ends the subpath; next M/cmd opens one */
            push_user(c, cx, cy);
        } else {
            /* unknown command letter — skip it (already consumed) */
        }
        if (p == before && C != 'Z') break;        /* anti-hang: no progress -> stop */
    }
}

/* case-insensitive whole-string compare (small helper for the "inherit" keyword). */
static int ieq(const char *a, const char *b) {
    while (*a && *b) { if (lc((unsigned char)*a) != lc((unsigned char)*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

/* Parse a gradient coordinate value -> 16.16. A bare number is taken as-is; a
 * percentage ("50%") becomes the 0..1 fraction (0.5). */
static fx parse_coord(const char *v) {
    const char *p = v, *e = v + strlen(v);
    fx n = parse_num(&p, e);
    if (p < e && *p == '%') n = (fx)((int64_t)n / 100);
    return n;
}

/* Find a collected gradient by id (id[0..idlen) vs the NUL-terminated stored id). */
static int find_grad(ctx_t *c, const char *id, int idlen) {
    for (int i = 0; i < c->ngrad && i < SVG_MAX_GRADS; i++) {
        const char *gi = c->grads[i].id; int k = 0;
        while (k < idlen && gi[k] && gi[k] == id[k]) k++;
        if (k == idlen && gi[k] == 0) return i;
    }
    return -1;
}

/* Read an opacity-style property (`opacity`/`fill-opacity`/`stroke-opacity`) from
 * a shape's style= or attr and return it as a 0..255 multiplier (255 if absent).
 * Accepts a 0..1 fraction ("0.5") or a percentage ("50%"); clamped. */
static int alpha_attr(const char *s, const char *e, const char *key) {
    char style[1024]; style[0] = 0;
    get_attr(s, e, "style", style, sizeof style);
    char v[64];
    if (get_style(style, key, v, sizeof v) || get_attr(s, e, key, v, sizeof v)) {
        const char *p = v, *ve = v + strlen(v);
        fx n = parse_num(&p, ve);
        long a = (p < ve && *p == '%') ? (long)(n >> FX_SHIFT) * 255 / 100
                                       : ((long)n * 255) >> FX_SHIFT;
        if (a < 0) a = 0;
        if (a > 255) a = 255;
        return (int)a;
    }
    return 255;
}

/* ---- presentation attrs (fill / stroke / stroke-width + opacity) -------- *
 * Resolves each property: a shape's own `style=` overrides its attr, which
 * overrides the INHERITED value (c->in_*). The SVG initial values (opaque-black
 * fill, unset stroke, stroke-width 1) live in c->in_* and are unchanged unless a
 * <svg>/<g> ancestor set them — so an SVG with no inherited paint renders exactly
 * as before. "inherit" explicitly takes the inherited value. When `apply_opacity`
 * is set (shapes, not the <g> inherited-paint update) the resolved fill/stroke
 * alpha is scaled by the inherited group opacity (c->in_alpha) and this element's
 * opacity/fill-opacity/stroke-opacity. */
static void read_paint(ctx_t *c, const char *s, const char *e,
                       color_t *fill, color_t *stroke, fx *swid, int apply_opacity) {
    char style[1024]; style[0] = 0;
    get_attr(s, e, "style", style, sizeof style);
    char v[256];

    if (get_style(style, "fill", v, sizeof v) || get_attr(s, e, "fill", v, sizeof v)) {
        if (ieq(v, "inherit")) *fill = c->in_fill;
        else {
            parse_color(v, fill, 1);                 /* sets grad=0; url() -> gray fallback */
            if (lc((unsigned char)v[0])=='u' && lc((unsigned char)v[1])=='r' && lc((unsigned char)v[2])=='l') {
                const char *h = v; while (*h && *h != '#') h++;   /* fill:url(#id) -> a gradient? */
                if (*h == '#') { h++; const char *ids = h;
                    while (*h && *h != ')' && !is_space((unsigned char)*h)) h++;
                    int gi = find_grad(c, ids, (int)(h - ids));
                    if (gi >= 0) { fill->grad = gi + 1; fill->set = 1; }
                }
            }
        }
    } else *fill = c->in_fill;

    if (get_style(style, "stroke", v, sizeof v) || get_attr(s, e, "stroke", v, sizeof v)) {
        if (ieq(v, "inherit")) *stroke = c->in_stroke;
        else parse_color(v, stroke, 0);
    } else *stroke = c->in_stroke;

    if (get_style(style, "stroke-width", v, sizeof v) || get_attr(s, e, "stroke-width", v, sizeof v)) {
        if (ieq(v, "inherit")) *swid = c->in_swid;
        else { const char *p = v; *swid = parse_num(&p, v + strlen(v)); }
    } else *swid = c->in_swid;
    /* scale stroke width into device px (use sx; aspect ignored) — done by caller via swid*sx */

    if (apply_opacity) {
        int base = c->in_alpha;                                /* inherited group opacity */
        int ea  = alpha_attr(s, e, "opacity");                 /* this element's opacity   */
        int faf = alpha_attr(s, e, "fill-opacity");
        int saf = alpha_attr(s, e, "stroke-opacity");
        /* product of four 0..255 factors (<= 255^4 ~ 4.2e9) / 255^3 -> back to 0..255;
         * int64 intermediate so it can't overflow regardless of the platform `long`. */
        fill->a   = (uint8_t)((int64_t)fill->a   * base * ea * faf / ((int64_t)255*255*255));
        stroke->a = (uint8_t)((int64_t)stroke->a * base * ea * saf / ((int64_t)255*255*255));
    }
}

/* Rasterize the current point list with the given paint. `closed`=1 for filled
 * shapes (rect/circle/polygon/path) — fill then stroke; 0 for line/polyline. */
static void raster_shape(ctx_t *c, color_t fill, color_t stroke, fx swid_dev, int closed) {
    end_shape(c);
    if (closed) fill_poly(c, fill);
    stroke_poly(c, stroke, closed, swid_dev);
}

/* ---- <text>: render a string with the kernel 8x16 bitmap font --------------- *
 * The text anchor (ux,uy is the BASELINE) is run through the CTM + viewBox map; each
 * glyph is then drawn axis-aligned, scaled so its height is the font-size in device
 * pixels (8x16 glyph -> width = height/2). Translate/scale positioning works; rotation
 * of the glyphs themselves is not applied (rare). Everything is bounded + clamped:
 * blend_px range-checks every pixel, and the char count / glyph height are capped. */
#define SVG_TEXT_MAX  512    /* chars rendered per <text> */
#define SVG_TEXT_HMAX 128    /* device glyph height cap (bounds the per-glyph work) */
static void draw_text(ctx_t *c, fx ux, fx uy, fx fsize, const char *s, int n, color_t col) {
    if (!col.set || n <= 0) return;
    if (n > SVG_TEXT_MAX) n = SVG_TEXT_MAX;
    int x0  = map_x(c, ctm_x(c, ux, uy)) >> FX_SHIFT;          /* device anchor (baseline) */
    int by  = map_y(c, ctm_y(c, ux, uy)) >> FX_SHIFT;
    int H = (int)(fx_mulc(fsize, c->sy) >> FX_SHIFT);          /* glyph height in device px */
    if (H < 6) H = 6;
    if (H > SVG_TEXT_HMAX) H = SVG_TEXT_HMAX;
    int W = H / 2; if (W < 3) W = 3;                           /* 8x16 -> advance = height/2 */
    int top = by - H;                                         /* baseline -> top of the glyph cell */
    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch >= 128) ch = '?';
        int cx = x0 + i * W;
        for (int gy = 0; gy < 16; gy++) {
            unsigned char row = font_glyphs[ch][gy];
            if (!row) continue;
            int py0 = top + gy * H / 16, py1 = top + (gy + 1) * H / 16;
            for (int gx = 0; gx < 8; gx++) {
                if (!(row & (0x80 >> gx))) continue;
                int px0 = cx + gx * W / 8, px1 = cx + (gx + 1) * W / 8;
                for (int py = py0; py < py1; py++)
                    for (int px = px0; px < px1; px++) blend_px(c, px, py, col, col.a);
            }
        }
    }
}

/* ---- the <svg> tag: dimensions + viewBox ------------------------------- */
/* Returns 0 and fills the ctx scale/canvas, or -1. */
static int read_svg_tag(ctx_t *c, const char *s, const char *e, int out_cap) {
    char v[128];
    fx wpx = 0, hpx = 0;
    int have_w = 0, have_h = 0;
    if (get_attr(s,e,"width",v,sizeof v))  { const char*p=v; wpx = parse_num(&p, v+strlen(v)); have_w = (wpx > 0); }
    if (get_attr(s,e,"height",v,sizeof v)) { const char*p=v; hpx = parse_num(&p, v+strlen(v)); have_h = (hpx > 0); }

    fx vbx=0, vby=0, vbw=0, vbh=0; int have_vb = 0;
    if (get_attr(s,e,"viewBox",v,sizeof v) || get_attr(s,e,"viewbox",v,sizeof v)) {
        const char *p = v, *pe = v + strlen(v);
        vbx = parse_num(&p,pe); vby = parse_num(&p,pe);
        vbw = parse_num(&p,pe); vbh = parse_num(&p,pe);
        have_vb = (vbw > 0 && vbh > 0);
    }

    int W, H;
    if (have_w && have_h)      { W = wpx >> FX_SHIFT; H = hpx >> FX_SHIFT; }
    else if (have_vb)          { W = vbw >> FX_SHIFT; H = vbh >> FX_SHIFT;
                                 if (have_w) W = wpx >> FX_SHIFT;
                                 if (have_h) H = hpx >> FX_SHIFT; }
    else return -1;            /* no way to size the canvas */

    if (W <= 0 || H <= 0) return -1;
    if (W > SVG_MAX_DIM) W = SVG_MAX_DIM;
    if (H > SVG_MAX_DIM) H = SVG_MAX_DIM;
    if ((long)W * H * 4 > out_cap) return -1;

    c->W = W; c->H = H;
    if (have_vb) {
        /* scale viewBox -> pixels (independent x/y), 16.16 */
        c->ox = vbx; c->oy = vby;
        c->sx = (fx)(((int64_t)W << (FX_SHIFT*2)) / (vbw ? vbw : FX_ONE));
        c->sy = (fx)(((int64_t)H << (FX_SHIFT*2)) / (vbh ? vbh : FX_ONE));
    } else {
        c->ox = 0; c->oy = 0; c->sx = FX_ONE; c->sy = FX_ONE;   /* 1 user unit = 1px */
    }
    return 0;
}

/* ---- gradient collection pre-pass -------------------------------------- *
 * Scans the whole SVG (including inside <defs>) for <linearGradient>/<radialGradient>
 * elements and their <stop> children, filling c->grads[]. Runs before the render
 * walk so forward references (fill=url(#id) before the gradient is declared) resolve.
 * Bounded by SVG_MAX_TAGS; never reads past `end`; every table write is capped. */
static void collect_gradients(ctx_t *c, const uint8_t *data, int len) {
    const char *p = (const char *)data, *end = p + len;
    int tags = 0, cur = -1;     /* cur = index of the gradient currently collecting stops */
    while (p < end && tags++ < SVG_MAX_TAGS) {
        if (*p != '<') { p++; continue; }
        const char *tag = p + 1;
        if (tag >= end) break;
        if (*tag == '!' || *tag == '?') {           /* comment / PI: skip to '>' */
            const char *q = tag;
            if (tag + 3 < end && tag[0]=='!' && tag[1]=='-' && tag[2]=='-') {
                q = tag + 3;
                while (q + 2 < end && !(q[0]=='-'&&q[1]=='-'&&q[2]=='>')) q++;
                p = (q + 2 < end) ? q + 3 : end;
            } else { while (q < end && *q != '>') q++; p = (q < end) ? q + 1 : end; }
            continue;
        }
        int closing = 0; const char *name = tag;
        if (*name == '/') { closing = 1; name++; }
        const char *ne = name;
        while (ne < end && !is_space((unsigned char)*ne) && *ne != '>' && *ne != '/') ne++;
        int nlen = (int)(ne - name);
        const char *tend = ne;
        while (tend < end && *tend != '>') tend++;
        const char *body_e = tend;
        const char *next = (tend < end) ? tend + 1 : end;

        int is_lin = (nlen == 14 && kw_eq(name, nlen, "lineargradient"));
        int is_rad = (nlen == 14 && kw_eq(name, nlen, "radialgradient"));
        int is_stop = (nlen == 4 && kw_eq(name, nlen, "stop"));

        if ((is_lin || is_rad) && !closing) {
            cur = -1;
            if (c->ngrad < SVG_MAX_GRADS) {
                grad_t *g = &c->grads[c->ngrad];
                memset(g, 0, sizeof *g);
                g->radial = is_rad;
                char v[64];
                get_attr(name, body_e, "id", g->id, sizeof g->id);
                if (get_attr(name, body_e, "gradientUnits", v, sizeof v) && ieq(v, "userSpaceOnUse"))
                    g->userspace = 1;
                if (is_rad) { g->x1 = FX_ONE/2; g->y1 = FX_ONE/2; g->x2 = FX_ONE/2; }  /* cx,cy,r */
                else        { g->x1 = 0; g->y1 = 0; g->x2 = FX_ONE; g->y2 = 0; }        /* default axis */
                if (is_rad) {
                    if (get_attr(name, body_e, "cx", v, sizeof v)) g->x1 = parse_coord(v);
                    if (get_attr(name, body_e, "cy", v, sizeof v)) g->y1 = parse_coord(v);
                    if (get_attr(name, body_e, "r",  v, sizeof v)) g->x2 = parse_coord(v);
                } else {
                    if (get_attr(name, body_e, "x1", v, sizeof v)) g->x1 = parse_coord(v);
                    if (get_attr(name, body_e, "y1", v, sizeof v)) g->y1 = parse_coord(v);
                    if (get_attr(name, body_e, "x2", v, sizeof v)) g->x2 = parse_coord(v);
                    if (get_attr(name, body_e, "y2", v, sizeof v)) g->y2 = parse_coord(v);
                }
                g->nstops = 0;
                cur = c->ngrad;
                c->ngrad++;
                if (body_e > name && body_e[-1] == '/') cur = -1;   /* self-closing: no stops */
            }
        } else if ((is_lin || is_rad) && closing) {
            cur = -1;
        } else if (is_stop && !closing && cur >= 0 && cur < SVG_MAX_GRADS) {
            grad_t *g = &c->grads[cur];
            if (g->nstops < SVG_MAX_STOPS) {
                char style[256]; style[0] = 0; get_attr(name, body_e, "style", style, sizeof style);
                char v[64];
                fx off = 0;
                if (get_attr(name, body_e, "offset", v, sizeof v)) off = parse_coord(v);
                if (off < 0) off = 0;
                if (off > FX_ONE) off = FX_ONE;
                color_t sc;
                if (get_style(style, "stop-color", v, sizeof v) || get_attr(name, body_e, "stop-color", v, sizeof v))
                    parse_color(v, &sc, 1);
                else parse_color("", &sc, 1);          /* default black */
                int sa = 255;
                if (get_style(style, "stop-opacity", v, sizeof v) || get_attr(name, body_e, "stop-opacity", v, sizeof v)) {
                    const char *op = v; fx on = parse_num(&op, v + strlen(v));
                    long aa = ((long)on * 255) >> FX_SHIFT;
                    if (aa < 0) aa = 0; if (aa > 255) aa = 255; sa = (int)aa;
                }
                gstop_t *st = &g->stops[g->nstops++];
                st->off = off; st->r = sc.r; st->g = sc.g; st->b = sc.b;
                st->a = (uint8_t)(sc.a * sa / 255);
            }
        }
        p = next;
    }
}

/* ---- <use href="#id"> support: find a referenced definition ------------- *
 * Scan [doc,docend) for the FIRST element carrying id="..." (or id='...') equal
 * to id[0..idlen). On a match set *dstart to that element's '<' and *dend to one
 * past the end of the element: for a self-closing/void element that's just past
 * its '>'; for a container (e.g. <g>/<symbol>) it's one past the matching close
 * tag, found by counting same-named open/close tags (depth-balanced). Returns 1
 * on success, 0 if no such id. Every scan is bounded by docend (no OOB), and the
 * close-tag search is bounded by SVG_MAX_TAGS (anti-hang on malformed markup). */
static int find_def(const char *doc, const char *docend, const char *id, int idlen,
                    const char **dstart, const char **dend) {
    if (idlen <= 0) return 0;
    const char *p = doc;
    int tags = 0;
    while (p < docend && tags++ < SVG_MAX_TAGS) {
        if (*p != '<') { p++; continue; }
        const char *tag = p + 1;
        if (tag >= docend) break;
        if (*tag == '!' || *tag == '?') {            /* comment / PI: skip to '>' */
            const char *q = tag;
            if (tag + 3 < docend && tag[0]=='!' && tag[1]=='-' && tag[2]=='-') {
                q = tag + 3;
                while (q + 2 < docend && !(q[0]=='-'&&q[1]=='-'&&q[2]=='>')) q++;
                p = (q + 2 < docend) ? q + 3 : docend;
            } else { while (q < docend && *q != '>') q++; p = (q < docend) ? q + 1 : docend; }
            continue;
        }
        const char *name = tag;
        if (*name == '/') { p = tag; while (p < docend && *p != '>') p++; p = (p<docend)?p+1:docend; continue; }
        const char *ne = name;
        while (ne < docend && !is_space((unsigned char)*ne) && *ne != '>' && *ne != '/') ne++;
        int nlen = (int)(ne - name);
        const char *tend = ne;
        while (tend < docend && *tend != '>') tend++;
        const char *body_e = tend;
        const char *next = (tend < docend) ? tend + 1 : docend;

        /* does THIS element's id="..." match? (get_attr is bounded by body_e) */
        char idv[64];
        if (nlen > 0 && get_attr(name, body_e, "id", idv, sizeof idv)) {
            int n = (int)strlen(idv);
            if (n == idlen) {
                int m = 1;
                for (int i = 0; i < idlen; i++) if (idv[i] != id[i]) { m = 0; break; }
                if (m) {
                    *dstart = p;                     /* the element's '<' */
                    int self_closing = (body_e > name && body_e[-1] == '/');
                    if (self_closing) { *dend = next; return 1; }   /* void: end of its tag */
                    /* container: scan forward for the depth-balanced close tag of
                     * the SAME element name; bounded by docend + SVG_MAX_TAGS. */
                    int depth = 1, t2 = 0;
                    const char *q = next;
                    while (q < docend && t2++ < SVG_MAX_TAGS) {
                        if (*q != '<') { q++; continue; }
                        const char *t = q + 1;
                        if (t >= docend) break;
                        if (*t == '!' || *t == '?') {     /* comment / PI: skip */
                            const char *r = t;
                            if (t + 3 < docend && t[0]=='!' && t[1]=='-' && t[2]=='-') {
                                r = t + 3;
                                while (r + 2 < docend && !(r[0]=='-'&&r[1]=='-'&&r[2]=='>')) r++;
                                q = (r + 2 < docend) ? r + 3 : docend;
                            } else { while (r < docend && *r != '>') r++; q = (r < docend) ? r + 1 : docend; }
                            continue;
                        }
                        int clo = 0; const char *nm = t;
                        if (*nm == '/') { clo = 1; nm++; }
                        const char *nme = nm;
                        while (nme < docend && !is_space((unsigned char)*nme) && *nme != '>' && *nme != '/') nme++;
                        int nl2 = (int)(nme - nm);
                        const char *te = nme;
                        while (te < docend && *te != '>') te++;
                        const char *qbody_e = te;
                        const char *qnext = (te < docend) ? te + 1 : docend;
                        int same = (nl2 == nlen);
                        if (same) for (int i = 0; i < nlen; i++)
                            if (lc((unsigned char)nm[i]) != lc((unsigned char)name[i])) { same = 0; break; }
                        if (same) {
                            if (clo) { depth--; if (depth == 0) { *dend = qnext; return 1; } }
                            else if (!(qbody_e > nm && qbody_e[-1] == '/')) depth++;  /* nested open (not self-closing) */
                        }
                        q = qnext;
                    }
                    *dend = docend;                  /* unterminated container -> to EOF */
                    return 1;
                }
            }
        }
        p = next;
    }
    return 0;
}

/* The element-walk render loop, factored out so <use href="#id"> can re-enter it
 * on a referenced definition span. Renders every element in [p,end): shapes are
 * built + rasterized; <g> maintains the transform+paint stack; <defs> content is
 * skipped (in_defs); <use> instantiates a definition (translated by x,y) by a
 * bounded, depth-capped recursive call. Behaviour for an ordinary top-level SVG
 * is byte-for-byte the old in-line loop (depth 0, no <use>). */
static void render_region(ctx_t *c, const char *p, const char *end, int depth);

/* ------------------------------------------------------------------------ */
int svg_decode(const uint8_t *data, int len, uint8_t *out, int out_cap,
               uint8_t *scratch, int scratch_cap, int *ow, int *oh) {
    if (!data || len < 5 || !out || out_cap < 4 || !ow || !oh) return -1;
    /* scratch holds the two fx point arrays */
    long need = (long)SVG_MAX_PTS * (long)sizeof(fx) * 2;
    if (!scratch || scratch_cap < need) return -1;

    const char *s = (const char *)data, *end = s + len;

    /* Locate the <svg ...> open tag (bounded scan). */
    const char *svg_s = 0, *svg_e = 0;
    {
        const char *p = s;
        int guard = 0;
        while (p < end && guard++ < SVG_MAX_TAGS) {
            if (*p == '<') {
                const char *q = p + 1;
                /* match "svg" (case-insensitive), as a whole element name */
                if (q + 3 <= end && lc(q[0])=='s' && lc(q[1])=='v' && lc(q[2])=='g' &&
                    (q+3==end || is_space(q[3]) || q[3]=='>' || q[3]=='/')) {
                    svg_s = q + 3;
                    const char *r = svg_s;
                    while (r < end && *r != '>') r++;       /* end of the open tag */
                    svg_e = r;
                    break;
                }
            }
            p++;
        }
    }
    if (!svg_s) return -1;

    ctx_t C;
    memset(&C, 0, sizeof C);
    set_identity(C.m);              /* transform stack starts at identity */
    C.out = out;
    C.px = (fx *)scratch;
    C.py = (fx *)(scratch + (long)SVG_MAX_PTS * sizeof(fx));
    if (read_svg_tag(&C, svg_s, svg_e, out_cap) != 0) return -1;

    /* SVG initial paint (fill black/opaque, stroke none, stroke-width 1), then let
     * the <svg> element's own fill/stroke seed the inherited paint for descendants. */
    parse_color("", &C.in_fill, 1);
    parse_color("", &C.in_stroke, 0);
    C.in_swid = FX_ONE;
    read_paint(&C, svg_s, svg_e, &C.in_fill, &C.in_stroke, &C.in_swid, 0);
    C.in_alpha = alpha_attr(svg_s, svg_e, "opacity");   /* root opacity (255 if absent) */

    /* collect gradient defs (pre-pass) so fill=url(#id) resolves, incl. forward refs */
    collect_gradients(&C, data, len);

    /* clear bitmap to fully-transparent black */
    memset(out, 0, (long)C.W * C.H * 4);

    /* document bounds for <use href="#id"> definition lookups (find_def). */
    C.doc = s; C.docend = end;

    /* Walk every element after the <svg> tag. render_region holds the actual
     * element loop so a <use> can re-enter it on a referenced definition span. */
    render_region(&C, svg_e < end ? svg_e + 1 : end, end, 0);

    *ow = C.W; *oh = C.H;
    return 0;
}

/* The factored-out element-walk loop (see the forward decl above). [p,end) is the
 * region to render; depth bounds <use> recursion. For depth 0 / no <use> this is
 * byte-for-byte the original in-line svg_decode loop. */
static void render_region(ctx_t *c, const char *p, const char *end, int depth) {
    int tags = 0;
    int in_defs = 0;       /* skip everything inside <defs>...</defs> */
    while (p < end && tags++ < SVG_MAX_TAGS) {
        if (*p != '<') { p++; continue; }
        const char *tag = p + 1;
        if (tag >= end) break;

        if (*tag == '!' || *tag == '?') {           /* comment / DOCTYPE / PI: skip to '>' */
            const char *q = tag;
            if (tag + 3 < end && tag[0]=='!' && tag[1]=='-' && tag[2]=='-') {   /* <!-- ... --> */
                q = tag + 3;
                while (q + 2 < end && !(q[0]=='-'&&q[1]=='-'&&q[2]=='>')) q++;
                p = (q + 2 < end) ? q + 3 : end;
            } else {
                while (q < end && *q != '>') q++;
                p = (q < end) ? q + 1 : end;
            }
            continue;
        }

        int closing = 0;
        const char *name = tag;
        if (*name == '/') { closing = 1; name++; }

        /* element name */
        const char *ne = name;
        while (ne < end && !is_space(*ne) && *ne != '>' && *ne != '/') ne++;
        int nlen = (int)(ne - name);

        /* find end of this tag (body = [ne, tend); '>') */
        const char *tend = ne;
        while (tend < end && *tend != '>') tend++;
        const char *body_e = tend;
        const char *next = (tend < end) ? tend + 1 : end;

        /* name compare helper */
        #define IS(nm) (nlen == (int)sizeof(nm)-1 && \
            ({ int _m=1; for (int _i=0;_i<nlen;_i++) if (lc((unsigned char)name[_i])!=lc((unsigned char)nm[_i])){_m=0;break;} _m; }))

        if (IS("defs")) { if (closing) in_defs = 0; else in_defs = 1; p = next; continue; }
        if (IS("svg") && closing) break;            /* </svg> ends it */
        /* A <symbol> only paints when instantiated via <use>: during the normal
         * top-level walk (depth 0) skip its whole subtree like <defs>. When it is
         * the wrapper opening a <use>-rendered region (depth > 0) it falls through
         * to the <g> path below and acts as a transparent group (renders children).
         * (<defs>/<symbol> share the in_defs skip; render_region's own in_defs
         * starts 0, so a def span beginning AT <symbol id=…> is not pre-skipped.) */
        if (IS("symbol") && depth == 0) { if (closing) in_defs = 0; else in_defs = 1; p = next; continue; }
        if (in_defs) { p = next; continue; }        /* skip ALL defs content (incl. <g>) */

        if (IS("g") || IS("symbol")) {              /* group / used <symbol>: transform+paint stack */
            if (closing) {                          /* </g>: pop -> restore parent CTM + paint */
                if (c->gdepth > 0) {
                    c->gdepth--;
                    if (c->gdepth < SVG_MAX_GROUPS) {
                        for (int i = 0; i < 6; i++) c->m[i] = c->gstack[c->gdepth][i];
                        c->in_fill = c->fillstk[c->gdepth];
                        c->in_stroke = c->strokestk[c->gdepth];
                        c->in_swid = c->swidstk[c->gdepth];
                        c->in_alpha = c->alphastk[c->gdepth];
                    }
                }
            } else if (!(body_e > name && body_e[-1] == '/')) {   /* <g ...> (not self-closing) */
                if (c->gdepth < SVG_MAX_GROUPS) {    /* save CTM+paint, then apply this group's */
                    for (int i = 0; i < 6; i++) c->gstack[c->gdepth][i] = c->m[i];
                    c->fillstk[c->gdepth] = c->in_fill;
                    c->strokestk[c->gdepth] = c->in_stroke;
                    c->swidstk[c->gdepth] = c->in_swid;
                    c->alphastk[c->gdepth] = c->in_alpha;
                    char tf[256];
                    if (get_attr(name, body_e, "transform", tf, sizeof tf)) {
                        fx tm[6]; parse_transform(tf, (int)strlen(tf), tm);
                        mat_mul(c->m, c->m, tm);
                    }
                    /* the group's own fill/stroke/stroke-width update the inherited paint;
                     * its opacity multiplies into the inherited group-opacity. */
                    read_paint(c, name, body_e, &c->in_fill, &c->in_stroke, &c->in_swid, 0);
                    c->in_alpha = c->in_alpha * alpha_attr(name, body_e, "opacity") / 255;
                }
                c->gdepth++;                         /* unconditional: keeps push/pop balanced past the cap */
            }
            p = next; continue;
        }

        if (IS("use")) {                            /* <use href="#id" x= y=>: instantiate a def */
            if (!closing && depth < SVG_MAX_USE) {
                char hv[128];
                int have = get_attr(name, body_e, "href", hv, sizeof hv) ||
                           get_attr(name, body_e, "xlink:href", hv, sizeof hv);
                if (have) {
                    const char *id = hv; if (*id == '#') id++;
                    int idlen = (int)strlen(id);
                    const char *ds = 0, *de = 0;
                    if (idlen > 0 && find_def(c->doc, c->docend, id, idlen, &ds, &de)) {
                        char v[48]; fx ux = 0, uy = 0;
                        if (get_attr(name,body_e,"x",v,sizeof v)) { const char*q=v; ux = parse_num(&q,v+strlen(v)); }
                        if (get_attr(name,body_e,"y",v,sizeof v)) { const char*q=v; uy = parse_num(&q,v+strlen(v)); }
                        fx saved[6]; for (int i = 0; i < 6; i++) saved[i] = c->m[i];
                        fx t[6]; set_identity(t); t[4] = ux; t[5] = uy;   /* translate(x,y) */
                        mat_mul(c->m, c->m, t);
                        /* the referenced span starts AT its element (e.g. <g id=...> or a
                         * bare shape), so render_region's own in_defs starts 0 and it
                         * renders; a <symbol>/<g> wrapper renders its children. */
                        render_region(c, ds, de, depth + 1);
                        for (int i = 0; i < 6; i++) c->m[i] = saved[i];   /* restore CTM */
                    }
                }
            }
            p = next; continue;
        }

        if (closing) { p = next; continue; }

        color_t fill, stroke; fx swid;
        /* per-element transform= : compose over the CTM for this shape only. */
        fx shape_saved[6]; int shape_tf = 0;
        { char tf[256];
          if (get_attr(name, body_e, "transform", tf, sizeof tf)) {
              for (int i = 0; i < 6; i++) shape_saved[i] = c->m[i];
              fx tm[6]; parse_transform(tf, (int)strlen(tf), tm);
              mat_mul(c->m, c->m, tm);
              shape_tf = 1;
          } }

        if (IS("rect")) {
            read_paint(c, name, body_e, &fill, &stroke, &swid, 1);
            build_rect(c, name, body_e);
            raster_shape(c, fill, stroke, fx_mul(swid, c->sx), 1);
        } else if (IS("circle")) {
            read_paint(c, name, body_e, &fill, &stroke, &swid, 1);
            char v[48]; fx cx=0,cy=0,r=0;
            if (get_attr(name,body_e,"cx",v,sizeof v)) { const char*q=v; cx=parse_num(&q,v+strlen(v)); }
            if (get_attr(name,body_e,"cy",v,sizeof v)) { const char*q=v; cy=parse_num(&q,v+strlen(v)); }
            if (get_attr(name,body_e,"r",v,sizeof v))  { const char*q=v; r =parse_num(&q,v+strlen(v)); }
            build_ellipse(c, cx, cy, r, r);
            raster_shape(c, fill, stroke, fx_mul(swid, c->sx), 1);
        } else if (IS("ellipse")) {
            read_paint(c, name, body_e, &fill, &stroke, &swid, 1);
            char v[48]; fx cx=0,cy=0,rx=0,ry=0;
            if (get_attr(name,body_e,"cx",v,sizeof v)) { const char*q=v; cx=parse_num(&q,v+strlen(v)); }
            if (get_attr(name,body_e,"cy",v,sizeof v)) { const char*q=v; cy=parse_num(&q,v+strlen(v)); }
            if (get_attr(name,body_e,"rx",v,sizeof v)) { const char*q=v; rx=parse_num(&q,v+strlen(v)); }
            if (get_attr(name,body_e,"ry",v,sizeof v)) { const char*q=v; ry=parse_num(&q,v+strlen(v)); }
            build_ellipse(c, cx, cy, rx, ry);
            raster_shape(c, fill, stroke, fx_mul(swid, c->sx), 1);
        } else if (IS("line")) {
            read_paint(c, name, body_e, &fill, &stroke, &swid, 1);
            char v[48]; fx x1=0,y1=0,x2=0,y2=0;
            if (get_attr(name,body_e,"x1",v,sizeof v)) { const char*q=v; x1=parse_num(&q,v+strlen(v)); }
            if (get_attr(name,body_e,"y1",v,sizeof v)) { const char*q=v; y1=parse_num(&q,v+strlen(v)); }
            if (get_attr(name,body_e,"x2",v,sizeof v)) { const char*q=v; x2=parse_num(&q,v+strlen(v)); }
            if (get_attr(name,body_e,"y2",v,sizeof v)) { const char*q=v; y2=parse_num(&q,v+strlen(v)); }
            reset_shape(c); push_user(c, x1,y1); push_user(c, x2,y2);
            /* a <line> defaults to stroke=black if none given (else invisible) */
            if (!stroke.set) parse_color("black", &stroke, 1);
            raster_shape(c, fill, stroke, fx_mul(swid, c->sx), 0);
        } else if (IS("polyline")) {
            read_paint(c, name, body_e, &fill, &stroke, &swid, 1);
            build_points(c, name, body_e);
            raster_shape(c, fill, stroke, fx_mul(swid, c->sx), 0);
        } else if (IS("polygon")) {
            read_paint(c, name, body_e, &fill, &stroke, &swid, 1);
            build_points(c, name, body_e);
            raster_shape(c, fill, stroke, fx_mul(swid, c->sx), 1);
        } else if (IS("path")) {
            read_paint(c, name, body_e, &fill, &stroke, &swid, 1);
            build_path(c, name, body_e);
            raster_shape(c, fill, stroke, fx_mul(swid, c->sx), 1);
        } else if (IS("text") && !closing) {
            read_paint(c, name, body_e, &fill, &stroke, &swid, 1);   /* text uses the fill colour */
            char v[64]; fx tx = 0, ty = 0, fsz = fx_from_int(16);     /* default font-size 16 */
            if (get_attr(name, body_e, "x", v, sizeof v)) { const char *q=v; tx = parse_num(&q, v+strlen(v)); }
            if (get_attr(name, body_e, "y", v, sizeof v)) { const char *q=v; ty = parse_num(&q, v+strlen(v)); }
            if (get_attr(name, body_e, "font-size", v, sizeof v)) { const char *q=v; fsz = parse_num(&q, v+strlen(v)); }
            else { char st[256]; st[0]=0; get_attr(name, body_e, "style", st, sizeof st);
                   char fv[32]; if (get_style(st, "font-size", fv, sizeof fv)) { const char *q=fv; fsz = parse_num(&q, fv+strlen(fv)); } }
            /* content = the text up to the next tag (nested <tspan> etc. truncate it, ok for v1) */
            const char *cs = next, *ce = next;
            while (ce < end && *ce != '<') ce++;
            char buf[SVG_TEXT_MAX]; int bn = 0;
            for (const char *q = cs; q < ce && bn < SVG_TEXT_MAX-1; q++) {
                char ch = *q;
                if (ch=='\n' || ch=='\t' || ch=='\r') ch = ' ';      /* collapse layout whitespace */
                buf[bn++] = ch;
            }
            buf[bn] = 0;
            draw_text(c, tx, ty, fsz, buf, bn, fill);
        }
        /* else: <image>, unknown -> skipped (<g>/<use> handled above; the text content
         * after a <text> tag is plain text and is skipped by the tag scanner) */

        if (shape_tf) for (int i = 0; i < 6; i++) c->m[i] = shape_saved[i];   /* restore CTM */
        #undef IS
        p = next;
    }
}
