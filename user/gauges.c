/*
 * gauges.c — a graphical CPU/RAM GAUGE dashboard, a userspace program (M1386).
 *
 * A speedometer-style alternative to sysgraph's line graphs: two semicircular
 * gauges (CPU left, RAM right), each with a colour-zoned arc (green<50% /
 * amber 50-80% / red >=80%), 0/25/50/75/100 ticks, a needle pointing to the
 * current value, a centre % readout, and a label. Data from /proc/stat (the
 * busy/total delta) and /proc/meminfo. Integer Bhaskara sin, kernel 8x16 font.
 *
 * Launch: `run gauges` or the Apps menu ("Gauges"); q/Esc quits.
 */
#include "ulib.h"

#define W 320
#define H 184

static unsigned *FB;
static unsigned char FONT[128 * 16];

static int isin(int d) { d %= 360; if (d < 0) d += 360; int s = 1; if (d > 180) { d -= 180; s = -1; }
    long n = 4L * d * (180 - d), de = 40500 - (long)d * (180 - d); return s * (int)(n * 1024 / de); }
static int icos(int d) { return isin(d + 90); }
static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void fill(int x0, int y0, int w, int h, unsigned c) { for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) putpx(x, y, c); }
static void line(int x0, int y0, int x1, int y1, unsigned c) {
    int dx = x1 - x0, dy = y1 - y0, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1, e = (ax > ay ? ax : -ay) / 2, e2;
    for (;;) { putpx(x0, y0, c); if (x0 == x1 && y0 == y1) break; e2 = e; if (e2 > -ax) { e -= ay; x0 += sx; } if (e2 < ay) { e += ax; y0 += sy; } }
}
static void ch(char c, int px, int py, unsigned col) { unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16]; for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) putpx(px + b, py + r, col); }
static void text(const char *s, int x, int y, unsigned col) { for (int i = 0; s[i]; i++) { ch(s[i], x, y, col); x += 8; } }
static int slurp(const char *p, char *b, int m) { long n = sys_readfile(p, b, m - 1); if (n < 0) n = 0; b[n] = 0; return (int)n; }
static long nth(const char *s, int n) { int k = 0; for (int i = 0; s[i]; ) { if (s[i] >= '0' && s[i] <= '9') { long v = 0; while (s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0'); if (k == n) return v; k++; } else i++; } return 0; }
static int putu(char *o, int i, long v) { char t[12]; int n = 0; if (v < 0) v = 0; do { t[n++] = '0' + (int)(v % 10); v /= 10; } while (v); while (n) o[i++] = t[--n]; return i; }

/* ---- instrument-panel UI kit (M1447; see gconv.c M1430). A gauge cluster belongs on a
 * faceplate — slate bg, silkscreen header, green LED; the gauges keep their zone colours. */
#define C_FACE_T  0x232D33u
#define C_FACE_B  0x161D21u
#define C_BEZHI   0x3C4A50u
#define C_BEZLO   0x0E1316u
#define C_AMBERLO 0x7A521Au
#define C_LABEL   0xC6D0CCu
#define C_LED     0x46E0A0u

static void vgrad(int x, int y, int w, int h, unsigned t, unsigned b) {
    for (int r = 0; r < h; r++) {
        int R = ((int)(t >> 16 & 0xFF) * (h - 1 - r) + (int)(b >> 16 & 0xFF) * r) / (h - 1);
        int G = ((int)(t >> 8  & 0xFF) * (h - 1 - r) + (int)(b >> 8  & 0xFF) * r) / (h - 1);
        int B = ((int)(t       & 0xFF) * (h - 1 - r) + (int)(b       & 0xFF) * r) / (h - 1);
        fill(x, y + r, w, 1, ((unsigned)R << 16) | ((unsigned)G << 8) | (unsigned)B);
    }
}
static void bevel_up(int x, int y, int w, int h, unsigned hi, unsigned lo) {
    fill(x, y, w, 1, hi); fill(x, y, 1, h, hi); fill(x, y + h - 1, w, 1, lo); fill(x + w - 1, y, 1, h, lo);
}
static void led(int x, int y) {
    fill(x, y, 9, 9, C_BEZLO); fill(x + 1, y + 1, 7, 7, 0x1A6E50u);
    fill(x + 2, y + 2, 5, 5, C_LED); putpx(x + 3, y + 3, 0xCFFFE8u);
}

/* one semicircular gauge: top-half arc (0%=left, 100%=right), needle, %, label */
static void gauge(int cx, int cy, int R, int val, const char *name) {
    if (val < 0) val = 0; if (val > 100) val = 100;
    for (int t = 0; t <= 180; t++) {                       /* colour-zoned arc; angle t: 180=left(0%) .. 0=right(100%) */
        int v = (180 - t) * 100 / 180;
        unsigned c = v >= 80 ? 0xF05050 : v >= 50 ? 0xE8C040 : 0x40D060;
        for (int rr = R; rr > R - 4; rr--) putpx(cx + rr * icos(t) / 1024, cy - rr * isin(t) / 1024, c);
    }
    for (int q = 0; q <= 100; q += 25) {                   /* ticks */
        int t = 180 - q * 180 / 100;
        line(cx + (R + 1) * icos(t) / 1024, cy - (R + 1) * isin(t) / 1024, cx + (R - 7) * icos(t) / 1024, cy - (R - 7) * isin(t) / 1024, 0x808890);
    }
    int tv = 180 - val * 180 / 100;                        /* needle to the value */
    int nx = cx + (R - 10) * icos(tv) / 1024, ny = cy - (R - 10) * isin(tv) / 1024;
    line(cx, cy, nx, ny, 0xFFFFFF); line(cx + 1, cy, nx + 1, ny, 0xFFFFFF); line(cx, cy + 1, nx, ny + 1, 0xFFFFFF);
    fill(cx - 3, cy - 3, 7, 7, 0xFFD040);                  /* hub */
    char s[8]; int si = putu(s, 0, val); s[si++] = '%'; s[si] = 0;
    unsigned vc = val >= 80 ? 0xF07070 : val >= 50 ? 0xF0D060 : 0x70E080;
    text(s, cx - si * 8 / 2, cy - 34, vc);                 /* % readout inside the arc */
    int nl = 0; while (name[nl]) nl++;
    text(name, cx - nl * 8 / 2, cy + 10, C_LABEL);         /* label below the baseline */
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gauges: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gauges: init failed\n"); return 1; }

    long pbusy = 0, ptot = 0; int first = 1; char buf[2048];
    for (;;) {
        slurp("/proc/stat", buf, sizeof buf);
        long u = nth(buf, 0), ni = nth(buf, 1), sy = nth(buf, 2), id = nth(buf, 3);
        long busy = u + ni + sy, tot = busy + id; int cp = 0;
        if (!first && tot > ptot) { cp = (int)((busy - pbusy) * 100 / (tot - ptot)); if (cp < 0) cp = 0; if (cp > 100) cp = 100; }
        pbusy = busy; ptot = tot; first = 0;

        slurp("/proc/meminfo", buf, sizeof buf);
        long mt = nth(buf, 0), mu = nth(buf, 2);
        int rp = (mt > 0) ? (int)(mu * 100 / mt) : 0; if (rp < 0) rp = 0; if (rp > 100) rp = 100;

        vgrad(0, 0, W, H, C_FACE_T, C_FACE_B);               /* slate faceplate (M1447) */
        text("SYSTEM LOAD", 12, 10, C_LABEL);
        fill(12, 27, 88, 2, C_AMBERLO);                      /* amber title rule */
        led(W - 24, 11);                                     /* power LED */
        gauge(85, 132, 62, cp, "CPU");
        gauge(235, 132, 62, rp, "RAM");

        bevel_up(0, 0, W, H, C_BEZHI, C_BEZLO);              /* window frame */
        sys_gfx_blit(FB);
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        sys_sleep(500);
    }
    return 0;
}
