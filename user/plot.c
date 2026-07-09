/*
 * plot.c — a graphing calculator: plots y = f(x) on a pixel canvas (M1700).
 *
 * The OS had a scalar calculator (calc) and a spreadsheet (sheet); this is the
 * visual member of the family — type a formula in x and watch the curve. It is
 * a ring-3 gfx app (a w*h XRGB canvas via sys_gfx_init/blit, like the games and
 * gcalc), reusing the shared bitmap font for on-canvas text. The whole
 * expression engine (recursive-descent, the variable x, dmath.h's trig/log/exp)
 * lives in user/ploteval.h so it is host-unit-tested by tests/plot, exactly like
 * calc's calceval.h and sheet's sheeteval.h. This file is the canvas rendering
 * (grid, axes, curve) and the keyboard loop.
 *
 * Type to edit the formula (live-replotted); functions/operators are those of
 * ploteval.h (sin cos tan asin acos atan sqrt abs ln log log2 exp floor ceil
 * round sign, + - * / % ^, parens, pi, e, and the variable x). Separate several
 * formulas with ';' to plot up to four curves at once, each in its own colour.
 * Keys:  printable -> edit the formula · Backspace -> delete · arrows -> pan the
 *   view · Enter -> auto-fit the y-range to the curve · Tab -> reset the view ·
 *   Esc/`~` -> quit.  (Letter/digit/operator keys type into the formula, so only
 *   the non-typeable keys drive the view — that's why there's no +/- zoom.)
 *
 * Launch: `plot [formula]` from the shell, or the Apps menu (shows a demo wave).
 */
#include "ulib.h"
#include "ploteval.h"       /* the pure expression engine (host-tested by tests/plot) */

#define W    512
#define H    360
#define TOPH 14                    /* top formula bar height */
#define BOTH 14                    /* bottom status bar height */
#define PY0  TOPH                  /* plot area: rows [PY0, PY1] */
#define PY1  (H - BOTH - 1)
#define PH   (PY1 - PY0 + 1)

/* cyberpunk-ish palette (matches the desktop theme) */
#define C_BG    0x0A0A18u
#define C_GRID  0x1E2340u
#define C_AXIS  0x556099u
#define C_CURVE 0x35E0D0u          /* teal */
#define C_BAR   0x141830u
#define C_TEXT  0xC8D0F0u
#define C_DIM   0x7A86B0u
#define C_HILITE 0xFF4FA3u

static unsigned      *FB;
static unsigned char  FONT[128 * 16];
static double xmin = -10, xmax = 10, ymin = -6, ymax = 6;
static char   func[128] = "5*sin(x); x*x/9-6";   /* one or more ';'-separated formulas */
static int    func_len;

#define NF 4
static const unsigned FCOLORS[NF] = { 0x35E0D0u, 0xFF4FA3u, 0xF0D000u, 0x46E05Au };   /* teal, pink, yellow, green */
static char   funcs[NF][64]; static int nf;      /* `func` split on ';' */

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void fill(int x0, int y0, int w, int h, unsigned c) { for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) putpx(x, y, c); }
static void chS(char c, int px, int py, unsigned col) {
    unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16];
    for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) putpx(px + b, py + r, col);
}
static void text(const char *t, int x, int y, unsigned col) { for (int i = 0; t[i]; i++) { chS(t[i], x, y, col); x += 8; } }

/* Bresenham line (used to connect adjacent plot samples). */
static void line(int x0, int y0, int x1, int y1, unsigned c) {
    int dx = x1 - x0, dy = y1 - y0;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (ax >= ay) { int err = ax / 2, y = y0; for (int x = x0;; x += sx) { putpx(x, y, c); if (x == x1) break; err -= ay; if (err < 0) { y += sy; err += ax; } } }
    else          { int err = ay / 2, x = x0; for (int y = y0;; y += sy) { putpx(x, y, c); if (y == y1) break; err -= ax; if (err < 0) { x += sx; err += ay; } } }
}

static int sx_of(double x) { return (int)((x - xmin) / (xmax - xmin) * (W - 1) + 0.5); }
static int sy_of(double y) { return PY0 + (int)((ymax - y) / (ymax - ymin) * (PH - 1) + 0.5); }

/* Split `func` on ';' into up to NF trimmed, non-empty sub-expressions. */
static void split_funcs(void) {
    nf = 0; const char *p = func;
    while (*p && nf < NF) {
        while (*p == ' ') p++;
        int n = 0;
        while (*p && *p != ';' && n < 63) funcs[nf][n++] = *p++;
        while (n > 0 && funcs[nf][n - 1] == ' ') n--;
        funcs[nf][n] = 0;
        if (n > 0) nf++;
        while (*p && *p != ';') p++;                 /* skip any overflow in this segment */
        if (*p == ';') p++;
    }
}

static int sappend(char *d, int p, int max, const char *s) { while (*s && p < max - 1) d[p++] = *s++; d[p] = 0; return p; }

/* Compact axis-label formatter: round to 2 decimals, trim trailing zeros (so
 * auto-fit's long values like -2.99994810 read as a clean "-3"). */
static void fmtnum(double v, char *out) {
    int neg = v < 0; double a = neg ? -v : v;
    if (a > 1e9) { const char *s = dnum_to_str(v); int i = 0; while (s[i]) { out[i] = s[i]; i++; } out[i] = 0; return; }
    long ip = (long)a; int cents = (int)((a - (double)ip) * 100 + 0.5);
    if (cents >= 100) { ip++; cents -= 100; }
    int p = 0;
    if (neg && (ip || cents)) out[p++] = '-';
    char tmp[20]; int tn = 0; long x = ip; if (x == 0) tmp[tn++] = '0'; while (x) { tmp[tn++] = (char)('0' + x % 10); x /= 10; }
    while (tn) out[p++] = tmp[--tn];
    if (cents) { out[p++] = '.'; out[p++] = (char)('0' + cents / 10); if (cents % 10) out[p++] = (char)('0' + cents % 10); }
    out[p] = 0;
}

/* Auto-scale the y window to the finite values of f(x) over the current x range. */
static void auto_fit_y(void) {
    split_funcs();
    double lo = 0, hi = 0; int any = 0;
    for (int fi = 0; fi < nf; fi++)
        for (int px = 0; px < W; px += 2) {
            double x = xmin + (xmax - xmin) * px / (W - 1);
            int err; double y = plot_eval(funcs[fi], x, &err);
            if (err || js_isnan(y) || !js_isfinite(y)) continue;
            if (!any) { lo = hi = y; any = 1; } else { if (y < lo) lo = y; if (y > hi) hi = y; }
        }
    if (!any) return;
    if (hi - lo < 1e-9) { lo -= 1; hi += 1; }             /* flat line: give it room */
    double m = (hi - lo) * 0.10;                           /* 10% margin */
    ymin = lo - m; ymax = hi + m;
}

static void draw(void) {
    fill(0, 0, W, H, C_BG);

    /* gridlines at integer coordinates (thinned so they never crowd) */
    int xstep = 1; while ((xmax - xmin) / xstep > 24) xstep *= 2;
    for (int gx = (int)js_ceil(xmin); gx <= (int)js_floor(xmax); gx++) {
        if (gx % xstep) continue;
        int px = sx_of(gx); fill(px, PY0, 1, PH, gx == 0 ? C_AXIS : C_GRID);
    }
    int ystep = 1; while ((ymax - ymin) / ystep > 16) ystep *= 2;
    for (int gy = (int)js_ceil(ymin); gy <= (int)js_floor(ymax); gy++) {
        if (gy % ystep) continue;
        int py = sy_of(gy); if (py >= PY0 && py <= PY1) fill(0, py, W, 1, gy == 0 ? C_AXIS : C_GRID);
    }

    /* the curves: each ';'-separated function in its own colour, one sample/column */
    split_funcs();
    for (int fi = 0; fi < nf; fi++) {
        unsigned col = FCOLORS[fi & 3];
        int prevpy = 0, have = 0;
        for (int px = 0; px < W; px++) {
            double x = xmin + (xmax - xmin) * px / (W - 1);
            int err; double y = plot_eval(funcs[fi], x, &err);
            if (err || js_isnan(y) || !js_isfinite(y)) { have = 0; continue; }
            int py = sy_of(y);
            if (py < PY0 - PH || py > PY1 + PH) { have = 0; continue; }   /* far off-screen: break */
            if (have) line(px - 1, prevpy, px, py, col); else putpx(px, py, col);
            prevpy = py; have = 1;
        }
    }

    /* top bar: the (editable) formula, each function coloured like its curve */
    fill(0, 0, W, TOPH, C_BAR);
    text("y=", 2, 0, C_DIM);
    { int seg = 0, x = 18;
      for (int i = 0; func[i]; i++) {
          if (func[i] == ';') { chS(';', x, 0, C_DIM); x += 8; seg++; continue; }
          chS(func[i], x, 0, FCOLORS[seg & 3]); x += 8;
      } }
    fill(18 + func_len * 8, 1, 6, 12, C_HILITE);           /* block caret */

    /* bottom bar: the view range + a key hint */
    fill(0, H - BOTH, W, BOTH, C_BAR);
    char b[96], nb[24]; int p = 0;
    p = sappend(b, p, sizeof b, "x[");  fmtnum(xmin, nb); p = sappend(b, p, sizeof b, nb);
    p = sappend(b, p, sizeof b, ", ");  fmtnum(xmax, nb); p = sappend(b, p, sizeof b, nb);
    p = sappend(b, p, sizeof b, "]  y["); fmtnum(ymin, nb); p = sappend(b, p, sizeof b, nb);
    p = sappend(b, p, sizeof b, ", ");  fmtnum(ymax, nb); p = sappend(b, p, sizeof b, nb);
    p = sappend(b, p, sizeof b, "]");
    text(b, 2, H - BOTH, C_DIM);
    const char *hint = "arrows:pan  Enter:fit  Tab:reset  Esc:quit";
    int hl = 0; while (hint[hl]) hl++;
    text(hint, W - hl * 8 - 2, H - BOTH, C_DIM);

    sys_gfx_blit(FB);
}

int main(void) {
    char arg[64];
    if (sys_getarg(arg, sizeof arg) > 0 && arg[0] && !streq(arg, "demo")) {
        int i = 0; for (; arg[i] && i < (int)sizeof func - 1; i++) func[i] = arg[i]; func[i] = 0;
    }
    func_len = 0; while (func[func_len]) func_len++;

    if (sys_gfx_init(W, H) < 0) { print("plot: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("plot: init failed\n"); return 1; }
    draw();

    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(15); continue; }
        double xspan = xmax - xmin, yspan = ymax - ymin;
        if (k == 27 || k == '`' || k == '~') break;                 /* quit */
        else if (k == 8 || k == 127) { if (func_len > 0) func[--func_len] = 0; }   /* backspace */
        else if (k == '\n' || k == '\r') { auto_fit_y(); }          /* fit y to the curve */
        else if (k == '\t') { xmin = -10; xmax = 10; ymin = -6; ymax = 6; }        /* reset view */
        else if (k == 0x11) { ymin += yspan * 0.15; ymax += yspan * 0.15; }        /* pan up */
        else if (k == 0x12) { ymin -= yspan * 0.15; ymax -= yspan * 0.15; }        /* pan down */
        else if (k == 0x13) { xmin -= xspan * 0.15; xmax -= xspan * 0.15; }        /* pan left */
        else if (k == 0x14) { xmin += xspan * 0.15; xmax += xspan * 0.15; }        /* pan right */
        else if (k >= 32 && k < 127) { if (func_len < (int)sizeof func - 1) { func[func_len++] = (char)k; func[func_len] = 0; } }
        draw();
    }
    sys_gfx_blit(FB);
    return 0;
}
