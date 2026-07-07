/*
 * gsw.c — a graphical STOPWATCH, a userspace program (M1387).
 *
 * Big MM:SS.t digits (the kernel 8x16 font scaled 3x) over a ring with a
 * sweeping sub-second hand. Keys: SPACE = start/stop, l = lap, r = reset, q/Esc = quit.
 * Elapsed time from sys_uptime_ms with proper pause/resume accumulation. While
 * running it redraws ~25 fps for a smooth sweep; while stopped it idles.
 *
 * Launch: `run gsw` from the shell, or the Apps menu ("Stopwatch").
 */
#include "ulib.h"

#define W 224
#define H 300                 /* taller: room for the lap-splits panel (M1458) */

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
/* (textS retired in M1441 — the time now draws via gtextS with an amber bloom) */
static void text(const char *t, int x, int y, unsigned col) { for (int i = 0; t[i]; i++) { chS(t[i], x, y, 1, col); x += 8; } }

/* ---- instrument-panel UI kit (M1441; see gconv.c M1430) ---- */
#define C_FACE_T  0x232D33u
#define C_FACE_B  0x161D21u
#define C_BEZHI   0x3C4A50u
#define C_BEZLO   0x0E1316u
#define C_SCREEN  0x0A0F0Cu
#define C_SCANLN  0x0D140Fu
#define C_AMBER   0xFFB23Eu
#define C_AMBERLO 0x7A521Au
#define C_DIM     0x6E827Fu
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
static void bevel_dn(int x, int y, int w, int h, unsigned hi, unsigned lo) {
    fill(x, y, w, 1, lo); fill(x, y, 1, h, lo); fill(x, y + h - 1, w, 1, hi); fill(x + w - 1, y, 1, h, hi);
}
static void panel(int x, int y, int w, int h) {
    fill(x, y, w, h, C_SCREEN);
    for (int r = 3; r < h - 1; r += 3) fill(x + 1, y + r, w - 2, 1, C_SCANLN);
    bevel_dn(x, y, w, h, C_BEZHI, C_BEZLO);
}
static void gtextS(const char *t, int x, int y, int s, unsigned col, unsigned glow) {
    for (int i = 0; t[i]; i++) chS(t[i], x + i * 8 * s + 1, y + 1, s, glow);
    for (int i = 0; t[i]; i++) chS(t[i], x + i * 8 * s,     y,     s, col);
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gsw: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gsw: init failed\n"); return 1; }

    int running = 0; long accum = 0, start = 0;
    long laps[40]; int nlap = 0;             /* recorded lap splits (M1458) */
    int dirty = 1, prevb = 0;
    for (;;) {
        long now = sys_uptime_ms();
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        int mx, my, mb = sys_mouse(&mx, &my);                   /* a click = start/stop, like Space (M1398) */
        int clicked = (mb & 1) && !(prevb & 1) && mx >= 0; prevb = mb;
        if (k == ' ' || clicked) { if (running) { accum += now - start; running = 0; } else { start = now; running = 1; } dirty = 1; }
        else if (k == 'r' || k == 'R') { accum = 0; running = 0; nlap = 0; dirty = 1; }
        else if ((k == 'l' || k == 'L') && running && nlap < 40) { laps[nlap++] = now - start + accum; dirty = 1; }   /* record a lap split */

        if (running || dirty) {
            long ms = running ? (sys_uptime_ms() - start + accum) : accum;
            int mm = (int)(ms / 60000), ss = (int)((ms % 60000) / 1000), t = (int)((ms % 1000) / 100);
            vgrad(0, 0, W, H, C_FACE_T, C_FACE_B);                   /* slate faceplate */
            bevel_up(0, 0, W, H, C_BEZHI, C_BEZLO);
            panel(8, 12, W - 16, 56);                                /* recessed amber readout */

            char b[12]; int i = 0;
            b[i++] = '0' + (mm / 10) % 10; b[i++] = '0' + mm % 10; b[i++] = ':';
            b[i++] = '0' + ss / 10; b[i++] = '0' + ss % 10; b[i++] = '.'; b[i++] = '0' + t; b[i] = 0;
            gtextS(b, (W - 7 * 24) / 2, 16, 3, running ? C_AMBER : C_AMBERLO, C_AMBERLO);   /* time glows amber, dim when stopped */

            int cx = W / 2, cy = 124, R = 34;                        /* sub-second sweep ring (raised to centre it above the laps) */
            for (int a = 0; a < 360; a += 3) putpx(cx + R * isin(a) / 1024, cy - R * icos(a) / 1024, 0x33414A);
            int ang = (int)((ms % 1000) * 360 / 1000);
            line(cx, cy, cx + (R - 4) * isin(ang) / 1024, cy - (R - 4) * icos(ang) / 1024, running ? C_LED : C_DIM);
            fill(cx - 2, cy - 2, 5, 5, C_AMBER);                     /* hub */

            int lpy = 170, lph = H - lpy - 24;                       /* lap-splits panel (M1458) */
            panel(8, lpy, W - 16, lph);
            if (nlap == 0) text("- press l for a lap -", 18, lpy + 8, C_DIM);
            else {
                int show = nlap < 6 ? nlap : 6, first = nlap - show;
                for (int j = 0; j < show; j++) {
                    int idx = first + j; long d = laps[idx] - (idx > 0 ? laps[idx - 1] : 0);
                    int dm = (int)(d / 60000), dss = (int)((d % 60000) / 1000), dt = (int)((d % 1000) / 100);
                    char lb[20]; int p = 0; int n = idx + 1;
                    lb[p++] = 'L'; lb[p++] = '0' + n / 10; lb[p++] = '0' + n % 10; lb[p++] = ' '; lb[p++] = ' ';   /* always 2 digits (was 1 for n<10) so the time column doesn't shift once lap 10 appears */
                    lb[p++] = '0' + (dm / 10) % 10; lb[p++] = '0' + dm % 10; lb[p++] = ':';
                    lb[p++] = '0' + dss / 10; lb[p++] = '0' + dss % 10; lb[p++] = '.'; lb[p++] = '0' + dt; lb[p] = 0;
                    text(lb, 18, lpy + 8 + j * 16, idx == nlap - 1 ? C_AMBER : C_LED);   /* newest lap amber, the rest green */
                }
            }

            text(running ? "space stop  l lap  r reset" : "space start  r reset", 8, H - 16, C_DIM);
            sys_gfx_blit(FB);
            dirty = 0;
        }
        sys_sleep(running ? 40 : 110);
    }
    return 0;
}
