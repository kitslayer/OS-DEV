/*
 * aclock.c — a graphical ANALOG clock, a userspace program (M1358).
 *
 * The OS had a digital clock (clock.c, text) and the taskbar time, but no
 * graphical clock. This asks the window manager for a pixel canvas
 * (sys_gfx_init) and draws a real clock face every frame: a rim, twelve hour
 * ticks, and hour / minute / second hands swept from the RTC (sys_time gives
 * "YYYY-MM-DD HH:MM:SS"). Pure integer math — a Bhaskara-I sine approximation
 * (borrowed from demoscene.c) places the hands, so no FPU is needed.
 *
 * Launch: `run aclock` from the shell, or the Apps menu ("Analog Clock").
 * Press q or Esc to quit.
 */
#include "ulib.h"

#define W   240
#define H   240
#define CX  120
#define CY  120
#define R   112                /* face radius */

/* sin(deg) * 1024, deg in 0..359. Bhaskara I on [0,180]; odd half negated. */
static int isin(int d) {
    d %= 360; if (d < 0) d += 360;
    int sign = 1;
    if (d > 180) { d -= 180; sign = -1; }
    long num = 4L * d * (180 - d);
    long den = 40500 - (long)d * (180 - d);
    return sign * (int)(num * 1024 / den);
}
static int icos(int d) { return isin(d + 90); }

static unsigned *FB;
static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }

/* Bresenham line */
static void line(int x0, int y0, int x1, int y1, unsigned c) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = (adx > ady ? adx : -ady) / 2, e2;
    for (;;) {
        putpx(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -adx) { err -= ady; x0 += sx; }
        if (e2 <  ady) { err += adx; y0 += sy; }
    }
}
/* a thicker line: the centre line plus its four neighbours (for the hour/minute hands) */
static void thline(int x0, int y0, int x1, int y1, unsigned c) {
    line(x0, y0, x1, y1, c);
    line(x0 + 1, y0, x1 + 1, y1, c); line(x0 - 1, y0, x1 - 1, y1, c);
    line(x0, y0 + 1, x1, y1 + 1, c); line(x0, y0 - 1, x1, y1 - 1, c);
}

/* a hand from the centre, length `len`, at clock angle `ang` (0 = 12 o'clock, clockwise) */
static int hx(int len, int ang) { return CX + len * isin(ang) / 1024; }
static int hy(int len, int ang) { return CY - len * icos(ang) / 1024; }

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("aclock: graphics init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB) { print("aclock: out of memory\n"); return 1; }

    for (;;) {
        char tb[40]; sys_time(tb, sizeof tb);                    /* "YYYY-MM-DD HH:MM:SS" */
        int hh = (tb[11] - '0') * 10 + (tb[12] - '0');
        int mm = (tb[14] - '0') * 10 + (tb[15] - '0');
        int ss = (tb[17] - '0') * 10 + (tb[18] - '0');

        /* face: dark background, a filled disc, a near-white rim — one pass */
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int dx = x - CX, dy = y - CY, d2 = dx * dx + dy * dy;
                unsigned c;
                if (d2 > R * R)                         c = 0x0A0A16;     /* background */
                else if (d2 >= (R - 3) * (R - 3))       c = 0xE8E8F0;     /* rim */
                else                                    c = 0x161622;     /* face */
                FB[y * W + x] = c;
            }
        }

        /* twelve hour ticks (longer + white at 12/3/6/9, shorter + grey elsewhere) */
        for (int h = 0; h < 12; h++) {
            int ang = h * 30, maj = (h % 3 == 0);
            int ro = R - 4, ri = R - (maj ? 17 : 9);
            line(hx(ro, ang), hy(ro, ang), hx(ri, ang), hy(ri, ang), maj ? 0xFFFFFF : 0x8890A0);
        }

        /* hands: hour (short, thick, white), minute (long, thick, light-blue), second (thin, red) */
        int ah = (hh % 12) * 30 + mm / 2;       /* hour hand creeps with the minutes */
        int am = mm * 6 + ss / 10;              /* minute hand creeps with the seconds */
        int as = ss * 6;
        thline(CX, CY, hx(R * 52 / 100, ah), hy(R * 52 / 100, ah), 0xFFFFFF);
        thline(CX, CY, hx(R * 76 / 100, am), hy(R * 76 / 100, am), 0x6FB7FF);
        line(CX, CY, hx(R * 88 / 100, as), hy(R * 88 / 100, as), 0xFF4444);

        /* centre hub (a small amber disc over the hand pivots) */
        for (int y = -4; y <= 4; y++)
            for (int x = -4; x <= 4; x++)
                if (x * x + y * y <= 16) putpx(CX + x, CY + y, 0xFFD040);

        sys_gfx_blit(FB);

        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        sys_sleep(200);
    }
    return 0;
}
