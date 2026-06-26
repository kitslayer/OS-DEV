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
#define H   300                /* 240 dial + a 60-px readout strip (time + date) below */
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

/* ---- instrument-panel UI kit (M1445; see gconv.c M1430). Light touch: aclock keeps its
 * round analog face but joins the clock family — slate corners, a window bevel, and its
 * digital sub-readout on a recessed amber panel. ---- */
#define C_BEZHI   0x3C4A50u
#define C_BEZLO   0x0E1316u
#define C_SCREEN  0x0A0F0Cu
#define C_SCANLN  0x0D140Fu
#define C_AMBER   0xFFB23Eu
#define C_DIM     0x90A8C8u      /* the existing date blue-grey, kept */
static void fillr(int x, int y, int w, int h, unsigned c) { for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) putpx(x + i, y + j, c); }
static void bevel_up(int x, int y, int w, int h, unsigned hi, unsigned lo) {
    fillr(x, y, w, 1, hi); fillr(x, y, 1, h, hi); fillr(x, y + h - 1, w, 1, lo); fillr(x + w - 1, y, 1, h, lo);
}
static void bevel_dn(int x, int y, int w, int h, unsigned hi, unsigned lo) {
    fillr(x, y, w, 1, lo); fillr(x, y, 1, h, lo); fillr(x, y + h - 1, w, 1, hi); fillr(x + w - 1, y, 1, h, hi);
}
static void panel(int x, int y, int w, int h) {
    fillr(x, y, w, h, C_SCREEN);
    for (int r = 3; r < h - 1; r += 3) fillr(x + 1, y + r, w - 2, 1, C_SCANLN);
    bevel_dn(x, y, w, h, C_BEZHI, C_BEZLO);
}

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

/* a 3x5 bitmap font (digits 0-9, ':', then '-'), low 3 bits per row — for the readout */
static const unsigned char FONT[12][5] = {
    {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
    {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7}, {0,2,0,2,0}, {0,0,7,0,0},
};
static void drawglyph(int idx, int px, int py, int s, unsigned col) {
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 3; c++)
            if ((FONT[idx][r] >> (2 - c)) & 1)
                for (int yy = 0; yy < s; yy++)
                    for (int xx = 0; xx < s; xx++) putpx(px + c * s + xx, py + r * s + yy, col);
}
/* "HH:MM:SS" (from the RTC string tb[11..18]) in lime, centred in the bottom strip */
static void drawtime(const char *tb) {
    int s = 4, adv = 4 * s, x = (W - (8 * adv - s)) / 2, y = 248;
    for (int i = 11; i <= 18; i++) {
        char ch = tb[i]; int idx = (ch >= '0' && ch <= '9') ? ch - '0' : (ch == ':') ? 10 : -1;
        if (idx >= 0) drawglyph(idx, x, y, s, C_AMBER);
        x += adv;
    }
}
/* "YYYY-MM-DD" (from the RTC string tb[0..9]) in a soft blue-grey, below the time */
static void drawdate(const char *tb) {
    int s = 3, adv = 4 * s, x = (W - (10 * adv - s)) / 2, y = 276;
    for (int i = 0; i <= 9; i++) {
        char ch = tb[i]; int idx = (ch >= '0' && ch <= '9') ? ch - '0' : (ch == '-') ? 11 : -1;
        if (idx >= 0) drawglyph(idx, x, y, s, 0x90A8C8);
        x += adv;
    }
}

/* a hand from the centre, length `len`, at clock angle `ang` (0 = 12 o'clock, clockwise) */
static int hx(int len, int ang) { return CX + len * isin(ang) / 1024; }
static int hy(int len, int ang) { return CY - len * icos(ang) / 1024; }

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("aclock: graphics init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB) { print("aclock: out of memory\n"); return 1; }

    int psec = -1;
    for (;;) {
        char tb[40]; sys_time(tb, sizeof tb);                    /* "YYYY-MM-DD HH:MM:SS" */
        int hh = (tb[11] - '0') * 10 + (tb[12] - '0');
        int mm = (tb[14] - '0') * 10 + (tb[15] - '0');
        int ss = (tb[17] - '0') * 10 + (tb[18] - '0');
        if (ss == psec) { int k = sys_pollkey(); if (k == 'q' || k == 27) break; sys_sleep(120); continue; }
        psec = ss;                                           /* redraw only when the displayed time changes (M1367) */

        /* face: dark background, a filled disc, a near-white rim — one pass */
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int dx = x - CX, dy = y - CY, d2 = dx * dx + dy * dy;
                unsigned c;
                if (d2 > R * R)                         c = 0x1C2329;     /* slate background (M1445) */
                else if (d2 >= (R - 3) * (R - 3))       c = 0xE8E8F0;     /* rim */
                else                                    c = 0x161622;     /* face */
                FB[y * W + x] = c;
            }
        }

        /* sixty fine minute ticks (the 5-minute marks become the bolder hour ticks below) */
        for (int m = 0; m < 60; m++) {
            if (m % 5 == 0) continue;
            int ang = m * 6, ro = R - 4, ri = R - 7;
            line(hx(ro, ang), hy(ro, ang), hx(ri, ang), hy(ri, ang), 0x55607A);
        }
        /* twelve hour ticks (longer + white + thick at 12/3/6/9, shorter + grey elsewhere) */
        for (int h = 0; h < 12; h++) {
            int ang = h * 30, maj = (h % 3 == 0);
            int ro = R - 4, ri = R - (maj ? 17 : 9);
            if (maj) thline(hx(ro, ang), hy(ro, ang), hx(ri, ang), hy(ri, ang), 0xFFFFFF);
            else     line(hx(ro, ang), hy(ro, ang), hx(ri, ang), hy(ri, ang), 0x8890A0);
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

        bevel_up(0, 0, W, H, C_BEZHI, C_BEZLO);              /* window frame (M1445) */
        panel(8, 244, W - 16, 50);                           /* recessed sub-readout screen */
        drawtime(tb);                                        /* digital HH:MM:SS readout (amber) below the dial */
        drawdate(tb);                                        /* and the date below that */
        sys_gfx_blit(FB);

        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        sys_sleep(200);
    }
    return 0;
}
