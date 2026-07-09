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
 * ':' opens a command line (':' is never part of a formula), with the calculus
 *   commands  int  (toggle the definite integral of the first curve over the
 *   visible x-range — shades the area to the x-axis and prints the value) ·  der
 *   (toggle the numeric derivative f'(x) of the first curve, drawn as a lavender
 *   overlay) ·  root  (toggle the zeros of the first curve in view — orange ticks
 *   on the x-axis + the x-values listed) ·  fit ·  reset.  Integral (Simpson),
 *   derivative (central difference) and roots (sign-change scan + bisection) are
 *   all pure, host-tested ploteval.h helpers.
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
#define C_FILL  0x123A44u          /* dim teal — :int area shading */
#define C_DER   0xB0A6E6u          /* lavender — :der derivative overlay */
#define C_ROOT  0xFF8828u          /* orange — :root zero markers */

static unsigned      *FB;
static unsigned char  FONT[128 * 16];
static double xmin = -10, xmax = 10, ymin = -6, ymax = 6;
static char   func[128] = "5*sin(x); x*x/9-6";   /* one or more ';'-separated formulas */
static int    func_len;

#define NF 4
static const unsigned FCOLORS[NF] = { 0x35E0D0u, 0xFF4FA3u, 0xF0D000u, 0x46E05Au };   /* teal, pink, yellow, green */
static char   funcs[NF][64]; static int nf;      /* `func` split on ';' */

static int    cmdmode;                           /* ':' command line active */
static char   cmd[48]; static int cmd_len;
static int    show_int, show_der, show_root;     /* :int / :der / :root toggles (first curve) */

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

/* Run the ':'-command in cmd[]. Unknown commands are ignored silently. */
static void exec_plot_cmd(void) {
    const char *c = cmd; while (*c == ' ') c++;
    if      (streq(c, "int"))   show_int = !show_int;
    else if (streq(c, "der"))   show_der = !show_der;
    else if (streq(c, "root"))  show_root = !show_root;
    else if (streq(c, "fit"))   auto_fit_y();
    else if (streq(c, "reset")) { xmin = -10; xmax = 10; ymin = -6; ymax = 6; }
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

    split_funcs();

    /* :int — shade the signed area between the first curve and the x-axis over
     * the whole visible x-range (drawn under the curves so they stay on top) */
    if (show_int && nf > 0) {
        int ay = sy_of(0);                                 /* the y=0 axis row */
        for (int px = 0; px < W; px++) {
            double x = xmin + (xmax - xmin) * px / (W - 1);
            int err; double y = plot_eval(funcs[0], x, &err);
            if (err || js_isnan(y) || !js_isfinite(y)) continue;
            int cy = sy_of(y), y0 = cy < ay ? cy : ay, y1 = cy < ay ? ay : cy;
            for (int yy = y0; yy <= y1; yy++) if (yy >= PY0 && yy <= PY1) putpx(px, yy, C_FILL);
        }
    }

    /* the curves: each ';'-separated function in its own colour, one sample/column */
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

    /* :der — the first curve's numeric derivative f'(x), a lavender overlay */
    if (show_der && nf > 0) {
        double h = (xmax - xmin) / (W * 4.0);
        int prevpy = 0, have = 0;
        for (int px = 0; px < W; px++) {
            double x = xmin + (xmax - xmin) * px / (W - 1);
            int err; double dy = plot_derivative(funcs[0], x, h, &err);
            if (err || js_isnan(dy) || !js_isfinite(dy)) { have = 0; continue; }
            int py = sy_of(dy);
            if (py < PY0 - PH || py > PY1 + PH) { have = 0; continue; }
            if (have) line(px - 1, prevpy, px, py, C_DER); else putpx(px, py, C_DER);
            prevpy = py; have = 1;
        }
    }

    /* :int / :der / :root readouts, top-left of the plot area */
    { int ly = PY0 + 3;
      if (show_int && nf > 0) {
          int err; double area = plot_integral(funcs[0], xmin, xmax, 2000, &err);
          char lb[64], nb[24]; int p = sappend(lb, 0, sizeof lb, "area = ");
          if (err) p = sappend(lb, p, sizeof lb, "n/a"); else { fmtnum(area, nb); p = sappend(lb, p, sizeof lb, nb); }
          text(lb, 4, ly, FCOLORS[0]); ly += 14;
      }
      if (show_der && nf > 0) { text("f'(x)", 4, ly, C_DER); ly += 14; }
      if (show_root && nf > 0) {
          double rts[16]; int nrt = plot_find_roots(funcs[0], xmin, xmax, W * 2, rts, 16);
          char lb[80], nb[24]; int p = sappend(lb, 0, sizeof lb, "roots: ");
          if (nrt == 0) p = sappend(lb, p, sizeof lb, "none in view");
          for (int i = 0; i < nrt && i < 6; i++) { if (i) p = sappend(lb, p, sizeof lb, ", "); fmtnum(rts[i], nb); p = sappend(lb, p, sizeof lb, nb); }
          if (nrt > 6) p = sappend(lb, p, sizeof lb, ", ...");
          text(lb, 4, ly, C_ROOT);
          int ay = sy_of(0);                              /* an orange tick on the x-axis at each root */
          for (int i = 0; i < nrt; i++) {
              int mx = sx_of(rts[i]);
              for (int yy = ay - 6; yy <= ay + 6; yy++) if (yy >= PY0 && yy <= PY1) putpx(mx, yy, C_ROOT);
          }
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

    /* bottom bar: a ':' command line while active, else the view range + hint */
    fill(0, H - BOTH, W, BOTH, C_BAR);
    if (cmdmode) {
        char cl[56]; int p = 0; cl[p++] = ':';
        for (int i = 0; cmd[i] && p < (int)sizeof cl - 1; i++) cl[p++] = cmd[i];
        cl[p] = 0;
        text(cl, 2, H - BOTH, C_TEXT);
        fill(2 + (cmd_len + 1) * 8, H - BOTH + 1, 6, 12, C_HILITE);   /* caret */
        const char *ch = "int  der  fit  reset";
        int chl = 0; while (ch[chl]) chl++;
        text(ch, W - chl * 8 - 2, H - BOTH, C_DIM);
    } else {
        char b[96], nb[24]; int p = 0;
        p = sappend(b, p, sizeof b, "x[");  fmtnum(xmin, nb); p = sappend(b, p, sizeof b, nb);
        p = sappend(b, p, sizeof b, ", ");  fmtnum(xmax, nb); p = sappend(b, p, sizeof b, nb);
        p = sappend(b, p, sizeof b, "]  y["); fmtnum(ymin, nb); p = sappend(b, p, sizeof b, nb);
        p = sappend(b, p, sizeof b, ", ");  fmtnum(ymax, nb); p = sappend(b, p, sizeof b, nb);
        p = sappend(b, p, sizeof b, "]");
        text(b, 2, H - BOTH, C_DIM);
        const char *hint = "arrows:pan  :int :der :root  Enter:fit  Esc:quit";
        int hl = 0; while (hint[hl]) hl++;
        text(hint, W - hl * 8 - 2, H - BOTH, C_DIM);
    }

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

        if (cmdmode) {                                              /* ':' command line */
            if (k == 27) cmdmode = 0;                               /* cancel */
            else if (k == '\n' || k == '\r') { exec_plot_cmd(); cmdmode = 0; }
            else if (k == 8 || k == 127) { if (cmd_len > 0) cmd[--cmd_len] = 0; }
            else if (k >= 32 && k < 127) { if (cmd_len < (int)sizeof cmd - 1) { cmd[cmd_len++] = (char)k; cmd[cmd_len] = 0; } }
            if (!cmdmode) { cmd_len = 0; cmd[0] = 0; }
            draw(); continue;
        }

        double xspan = xmax - xmin, yspan = ymax - ymin;
        if (k == 27 || k == '`' || k == '~') break;                 /* quit */
        else if (k == ':') { cmdmode = 1; cmd_len = 0; cmd[0] = 0; } /* open the command line */
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
