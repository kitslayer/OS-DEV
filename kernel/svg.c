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
 * Ignored (skipped gracefully): <text>/<image>/<use>/<defs>/gradients/filters,
 *   CSS classes, transforms beyond viewBox, opacity/fill-rule overrides.
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

/* ---- caps (documented) ------------------------------------------------- */
#define SVG_MAX_DIM    512      /* W,H each <= this */
#define SVG_MAX_PTS    8192     /* total polygon vertices buffered per shape */
#define SVG_MAX_SUBS   1024     /* subpaths per shape */
#define SVG_MAX_CMDS   16384    /* path 'd' commands processed (bounds the work) */
#define SVG_BEZ_STEPS  16       /* line segments per cubic/quadratic bezier */
#define SVG_MAX_TAGS   65536    /* hard ceiling on tags scanned (anti-hang) */

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
typedef struct { uint8_t r, g, b, a; int set; } color_t;   /* set=0 -> "none"/unset */

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
    c->r = c->g = c->b = 0; c->a = 255; c->set = dflt_set;
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

/* Append a device-space point to the current subpath (bounds-checked). */
static void push_pt(ctx_t *c, fx dx, fx dy) {
    if (c->npt >= SVG_MAX_PTS) return;     /* cap: silently truncate the shape */
    c->px[c->npt] = fx_clamp(dx);
    c->py[c->npt] = fx_clamp(dy);
    c->npt++;
}
/* Append a user-space point (mapped to device). */
static void push_user(ctx_t *c, fx ux, fx uy) { push_pt(c, map_x(c, ux), map_y(c, uy)); }

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
    /* device-space vertical extent -> pixel scanline range */
    fx ymin = c->py[0], ymax = c->py[0];
    for (int i = 1; i < c->npt; i++) { if (c->py[i]<ymin) ymin=c->py[i]; if (c->py[i]>ymax) ymax=c->py[i]; }
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
            for (int x = xa; x < xb; x++) blend_px(c, x, y, col, col.a);
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
    /* cos/sin via a tiny fixed-point table (no FPU). 16-entry quarter table. */
    /* Use the cubic flattener-free direct parametric approach with a sine LUT. */
    static const int SIN256[65] = {     /* sin(k*pi/128)*256, k=0..64 (quarter) */
        0,6,13,19,25,31,38,44,50,56,62,68,74,80,86,92,98,104,109,115,121,126,
        132,137,142,147,152,157,162,167,172,177,181,185,190,194,198,202,206,
        209,213,216,220,223,226,229,231,234,237,239,241,243,245,247,248,250,
        251,252,253,254,255,255,256,256,256 };
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

/* ---- presentation attrs (fill / stroke / stroke-width) ----------------- */
static void read_paint(const char *s, const char *e, color_t *fill, color_t *stroke, fx *swid) {
    char style[1024]; style[0] = 0;
    get_attr(s, e, "style", style, sizeof style);
    char v[256];

    /* fill: style overrides attr overrides default(black). */
    int got = 0;
    if (get_style(style, "fill", v, sizeof v)) { parse_color(v, fill, 1); got = 1; }
    if (!got && get_attr(s, e, "fill", v, sizeof v)) { parse_color(v, fill, 1); got = 1; }
    if (!got) parse_color("", fill, 1);            /* default: opaque black, set=1 */

    /* stroke: default unset. */
    if (get_style(style, "stroke", v, sizeof v)) parse_color(v, stroke, 0);
    else if (get_attr(s, e, "stroke", v, sizeof v)) parse_color(v, stroke, 0);
    else parse_color("", stroke, 0);

    /* stroke-width: default 1 (user units). */
    *swid = FX_ONE;
    if (get_style(style, "stroke-width", v, sizeof v)) { const char*p=v; *swid = parse_num(&p, v+strlen(v)); }
    else if (get_attr(s, e, "stroke-width", v, sizeof v)) { const char*p=v; *swid = parse_num(&p, v+strlen(v)); }
    /* scale stroke width into device px (use sx; aspect ignored) — done by caller via swid*sx */
}

/* Rasterize the current point list with the given paint. `closed`=1 for filled
 * shapes (rect/circle/polygon/path) — fill then stroke; 0 for line/polyline. */
static void raster_shape(ctx_t *c, color_t fill, color_t stroke, fx swid_dev, int closed) {
    end_shape(c);
    if (closed) fill_poly(c, fill);
    stroke_poly(c, stroke, closed, swid_dev);
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
    C.out = out;
    C.px = (fx *)scratch;
    C.py = (fx *)(scratch + (long)SVG_MAX_PTS * sizeof(fx));
    if (read_svg_tag(&C, svg_s, svg_e, out_cap) != 0) return -1;

    /* clear bitmap to fully-transparent black */
    memset(out, 0, (long)C.W * C.H * 4);

    /* Walk every element after the <svg> tag (bounded). For each shape tag,
     * build its geometry and rasterize. The scanner finds '<name ...>' spans;
     * it never reads past `end` and always advances. */
    const char *p = svg_e < end ? svg_e + 1 : end;
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
        if (in_defs || closing) { p = next; continue; }

        color_t fill, stroke; fx swid;

        if (IS("rect")) {
            read_paint(name, body_e, &fill, &stroke, &swid);
            build_rect(&C, name, body_e);
            raster_shape(&C, fill, stroke, fx_mul(swid, C.sx), 1);
        } else if (IS("circle")) {
            read_paint(name, body_e, &fill, &stroke, &swid);
            char v[48]; fx cx=0,cy=0,r=0;
            if (get_attr(name,body_e,"cx",v,sizeof v)) { const char*q=v; cx=parse_num(&q,v+strlen(v)); }
            if (get_attr(name,body_e,"cy",v,sizeof v)) { const char*q=v; cy=parse_num(&q,v+strlen(v)); }
            if (get_attr(name,body_e,"r",v,sizeof v))  { const char*q=v; r =parse_num(&q,v+strlen(v)); }
            build_ellipse(&C, cx, cy, r, r);
            raster_shape(&C, fill, stroke, fx_mul(swid, C.sx), 1);
        } else if (IS("ellipse")) {
            read_paint(name, body_e, &fill, &stroke, &swid);
            char v[48]; fx cx=0,cy=0,rx=0,ry=0;
            if (get_attr(name,body_e,"cx",v,sizeof v)) { const char*q=v; cx=parse_num(&q,v+strlen(v)); }
            if (get_attr(name,body_e,"cy",v,sizeof v)) { const char*q=v; cy=parse_num(&q,v+strlen(v)); }
            if (get_attr(name,body_e,"rx",v,sizeof v)) { const char*q=v; rx=parse_num(&q,v+strlen(v)); }
            if (get_attr(name,body_e,"ry",v,sizeof v)) { const char*q=v; ry=parse_num(&q,v+strlen(v)); }
            build_ellipse(&C, cx, cy, rx, ry);
            raster_shape(&C, fill, stroke, fx_mul(swid, C.sx), 1);
        } else if (IS("line")) {
            read_paint(name, body_e, &fill, &stroke, &swid);
            char v[48]; fx x1=0,y1=0,x2=0,y2=0;
            if (get_attr(name,body_e,"x1",v,sizeof v)) { const char*q=v; x1=parse_num(&q,v+strlen(v)); }
            if (get_attr(name,body_e,"y1",v,sizeof v)) { const char*q=v; y1=parse_num(&q,v+strlen(v)); }
            if (get_attr(name,body_e,"x2",v,sizeof v)) { const char*q=v; x2=parse_num(&q,v+strlen(v)); }
            if (get_attr(name,body_e,"y2",v,sizeof v)) { const char*q=v; y2=parse_num(&q,v+strlen(v)); }
            reset_shape(&C); push_user(&C, x1,y1); push_user(&C, x2,y2);
            /* a <line> defaults to stroke=black if none given (else invisible) */
            if (!stroke.set) parse_color("black", &stroke, 1);
            raster_shape(&C, fill, stroke, fx_mul(swid, C.sx), 0);
        } else if (IS("polyline")) {
            read_paint(name, body_e, &fill, &stroke, &swid);
            build_points(&C, name, body_e);
            raster_shape(&C, fill, stroke, fx_mul(swid, C.sx), 0);
        } else if (IS("polygon")) {
            read_paint(name, body_e, &fill, &stroke, &swid);
            build_points(&C, name, body_e);
            raster_shape(&C, fill, stroke, fx_mul(swid, C.sx), 1);
        } else if (IS("path")) {
            read_paint(name, body_e, &fill, &stroke, &swid);
            build_path(&C, name, body_e);
            raster_shape(&C, fill, stroke, fx_mul(swid, C.sx), 1);
        }
        /* else: <g>, <text>, <image>, <use>, gradients, unknown -> skipped */

        #undef IS
        p = next;
    }

    *ow = C.W; *oh = C.H;
    return 0;
}
