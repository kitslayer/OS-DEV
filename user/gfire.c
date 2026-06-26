/*
 * gfire.c — a fireworks display, a userspace program (M1402).
 *
 * A pure-integer particle simulation: rockets launch from the bottom, decelerate
 * under gravity, and burst at their apex into a ring of coloured sparks that arc
 * down and fade. The frame is faded toward black each step (not cleared) so every
 * particle leaves a glowing trail. No FPU — positions are 8.8 fixed-point and a
 * Bhaskara sine places the burst velocities. q/Esc quits.
 *
 * Launch: `run gfire` from the shell, or the Apps menu ("Fireworks").
 */
#include "ulib.h"

#define W 360
#define H 300
#define NP 320                    /* max live particles */
#define G  34                     /* gravity, fixed-point units / frame^2 */

static unsigned *FB;
static int isin(int d) { d %= 360; if (d < 0) d += 360; int s = 1; if (d > 180) { d -= 180; s = -1; }
    long n = 4L * d * (180 - d), de = 40500 - (long)d * (180 - d); return s * (int)(n * 1024 / de); }
static int icos(int d) { return isin(d + 90); }

static unsigned long seed = 22695477;
static int rnd(void) { seed = seed * 1103515245 + 12345; return (int)((seed >> 16) & 0x7FFF); }

struct P { int x, y, vx, vy, life, max; unsigned col; char rocket; };
static struct P ps[NP];
static const unsigned pal[7] = { 0xFF5050, 0x50FF70, 0x60A0FF, 0xFFD040, 0xFF70D0, 0x50E0E0, 0xFFFFFF };

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static unsigned scale(unsigned c, int num, int den) {        /* c * num/den, per channel */
    int r = ((c >> 16) & 0xFF) * num / den, g = ((c >> 8) & 0xFF) * num / den, b = (c & 0xFF) * num / den;
    return (unsigned)(r << 16 | g << 8 | b);
}
static void glow(int px, int py, unsigned c) {               /* a soft 3x3-ish dot */
    putpx(px, py, c);
    unsigned h = scale(c, 1, 2);
    putpx(px - 1, py, h); putpx(px + 1, py, h); putpx(px, py - 1, h); putpx(px, py + 1, h);
}
static int slot(void) { for (int i = 0; i < NP; i++) if (ps[i].life <= 0) return i; return -1; }

static void burst(int x, int y, unsigned col) {              /* spawn a ring of sparks */
    int n = 24 + rnd() % 16;
    for (int i = 0; i < n; i++) {
        int s = slot(); if (s < 0) break;
        int a = i * 360 / n, sp = 280 + rnd() % 520;
        ps[s].x = x; ps[s].y = y;
        ps[s].vx = sp * isin(a) / 1024;
        ps[s].vy = -sp * icos(a) / 1024;
        ps[s].life = ps[s].max = 34 + rnd() % 28;
        ps[s].col = col; ps[s].rocket = 0;
    }
}
static void launch(void) {
    int s = slot(); if (s < 0) return;
    ps[s].x = (60 + rnd() % (W - 120)) << 8;
    ps[s].y = (H - 4) << 8;
    ps[s].vx = (rnd() % 120) - 60;
    ps[s].vy = -(1500 + rnd() % 500);
    ps[s].life = ps[s].max = 40 + rnd() % 18;
    ps[s].col = pal[rnd() % 7]; ps[s].rocket = 1;
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gfire: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB) { print("gfire: out of memory\n"); return 1; }
    for (int i = 0; i < W * H; i++) FB[i] = 0x05060E;
    seed ^= (unsigned long)sys_uptime_ms();

    int frame = 0, prevb = 0;
    for (;;) {
        for (int i = 0; i < W * H; i++) {                    /* fade toward the night sky for glowing trails */
            unsigned c = FB[i];
            FB[i] = (((c >> 1) & 0x7F7F7F) + ((c >> 2) & 0x3F3F3F) + ((c >> 3) & 0x1F1F1F)) | 0x05060E;
        }
        if (frame % 20 == 0 || (frame % 7 == 0 && rnd() % 3 == 0)) launch();

        for (int i = 0; i < NP; i++) {
            if (ps[i].life <= 0) continue;
            ps[i].x += ps[i].vx; ps[i].y += ps[i].vy; ps[i].vy += G; ps[i].life--;
            int px = ps[i].x >> 8, py = ps[i].y >> 8;
            if (ps[i].rocket) {
                if (ps[i].vy >= 0 || ps[i].life <= 0) { burst(ps[i].x, ps[i].y, ps[i].col); ps[i].life = 0; continue; }
                glow(px, py, 0xFFF0C0);                      /* rising rocket: a hot white-gold spark */
            } else {
                unsigned c = scale(ps[i].col, ps[i].life, ps[i].max);   /* sparks fade as they age */
                glow(px, py, c);
            }
        }

        int mx, my, b = sys_mouse(&mx, &my);                 /* click to burst a firework right there (M1403) */
        if ((b & 1) && !(prevb & 1) && mx >= 0) burst(mx << 8, my << 8, pal[rnd() % 7]);
        prevb = b;

        sys_gfx_blit(FB);
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        sys_sleep(45);
        frame++;
    }
    return 0;
}
