/*
 * gsw.c — a graphical STOPWATCH, a userspace program (M1387).
 *
 * Big MM:SS.t digits (the kernel 8x16 font scaled 3x) over a ring with a
 * sweeping sub-second hand. Keys: SPACE = start/stop, r = reset, q/Esc = quit.
 * Elapsed time from sys_uptime_ms with proper pause/resume accumulation. While
 * running it redraws ~25 fps for a smooth sweep; while stopped it idles.
 *
 * Launch: `run gsw` from the shell, or the Apps menu ("Stopwatch").
 */
#include "ulib.h"

#define W 224
#define H 212

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
/* scaled glyph: each font pixel becomes an s*s block */
static void chS(char c, int px, int py, int s, unsigned col) {
    unsigned u = (unsigned char)c; if (u >= 128) u = '?'; const unsigned char *g = &FONT[u * 16];
    for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) fill(px + b * s, py + r * s, s, s, col);
}
static void textS(const char *t, int x, int y, int s, unsigned col) { for (int i = 0; t[i]; i++) { chS(t[i], x, y, s, col); x += 8 * s; } }
static void text(const char *t, int x, int y, unsigned col) { for (int i = 0; t[i]; i++) { chS(t[i], x, y, 1, col); x += 8; } }

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gsw: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gsw: init failed\n"); return 1; }

    int running = 0; long accum = 0, start = 0;
    int dirty = 1, prevb = 0;
    for (;;) {
        long now = sys_uptime_ms();
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        int mx, my, mb = sys_mouse(&mx, &my);                   /* a click = start/stop, like Space (M1398) */
        int clicked = (mb & 1) && !(prevb & 1) && mx >= 0; prevb = mb;
        if (k == ' ' || clicked) { if (running) { accum += now - start; running = 0; } else { start = now; running = 1; } dirty = 1; }
        else if (k == 'r' || k == 'R') { accum = 0; running = 0; dirty = 1; }

        if (running || dirty) {
            long ms = running ? (sys_uptime_ms() - start + accum) : accum;
            int mm = (int)(ms / 60000), ss = (int)((ms % 60000) / 1000), t = (int)((ms % 1000) / 100);
            for (int i = 0; i < W * H; i++) FB[i] = 0x0B0D14;

            char b[12]; int i = 0;
            b[i++] = '0' + (mm / 10) % 10; b[i++] = '0' + mm % 10; b[i++] = ':';
            b[i++] = '0' + ss / 10; b[i++] = '0' + ss % 10; b[i++] = '.'; b[i++] = '0' + t; b[i] = 0;
            unsigned dc = running ? 0x60E060 : 0xD0D0D8;             /* green while running, grey when stopped */
            textS(b, (W - 7 * 24) / 2, 26, 3, dc);                   /* 7 glyphs * 8 * scale-3 = 168 px wide */

            int cx = W / 2, cy = 132, R = 34;                        /* sub-second sweep ring */
            for (int a = 0; a < 360; a += 3) putpx(cx + R * isin(a) / 1024, cy - R * icos(a) / 1024, 0x303848);
            int ang = (int)((ms % 1000) * 360 / 1000);
            line(cx, cy, cx + (R - 4) * isin(ang) / 1024, cy - (R - 4) * icos(ang) / 1024, running ? 0x60E060 : 0x808890);
            fill(cx - 2, cy - 2, 5, 5, 0xFFD040);

            text(running ? "RUNNING   space:stop  r:reset" : "STOPPED   space:start  r:reset", 8, H - 16, 0x707888);
            sys_gfx_blit(FB);
            dirty = 0;
        }
        sys_sleep(running ? 40 : 110);
    }
    return 0;
}
