/*
 * mandel.c — a graphical Mandelbrot explorer with mouse zoom.
 *
 * Renders the escape-time fractal to a pixel canvas (the graphics window API).
 * Left-click zooms in centred on the cursor, right-click zooms out, r resets,
 * q quits. Userspace is built without the FPU, so the iteration is Q*.28 fixed
 * point in int64 — high enough precision for several zoom levels. (Click-driven:
 * a frame is only re-rendered after you zoom, since escape-time is heavy.)
 */
#include "ulib.h"

#define W   320
#define H   240
#define SH  28                       /* fixed-point: value = real * 2^28 */
#define ONE (1LL << SH)
#define MAXIT 150

static unsigned int *cv;
static long long cx, cy, scale;      /* centre (Q.28) and units-per-pixel (Q.28) */

static void reset(void) {
    cx = -3LL * ONE / 4;             /* centre ~ (-0.75, 0) */
    cy = 0;
    scale = (3LL * ONE) / W;         /* ~3.0 wide across the canvas */
}

/* iterations before |z|^2 > 4 for c=(cr,ci) Q.28; MAXIT if bounded */
static int escape(long long cr, long long ci) {
    long long zr = 0, zi = 0;
    for (int i = 0; i < MAXIT; i++) {
        long long zr2 = (zr * zr) >> SH;
        long long zi2 = (zi * zi) >> SH;
        if (zr2 + zi2 > (4LL << SH)) return i;
        zi = ((zr * zi) >> (SH - 1)) + ci;     /* 2*zr*zi + ci */
        zr = zr2 - zi2 + cr;                   /* zr^2 - zi^2 + cr */
    }
    return MAXIT;
}

/* iteration count -> a smooth-ish rainbow colour (in-set = black) */
static unsigned int colour(int it) {
    if (it >= MAXIT) return 0x000000;
    int r = (it * 8)  & 255;
    int g = (it * 5 + 80)  & 255;
    int b = (it * 11 + 160) & 255;
    return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

static void render(void) {
    for (int py = 0; py < H; py++) {
        long long ci = cy + (long long)(py - H / 2) * scale;
        for (int px = 0; px < W; px++) {
            long long cr = cx + (long long)(px - W / 2) * scale;
            cv[py * W + px] = colour(escape(cr, ci));
        }
    }
    sys_gfx_blit(cv);
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("mandel: graphics init failed\n"); return 1; }
    cv = malloc((unsigned long)W * H * 4);
    if (!cv) { print("mandel: out of memory\n"); return 1; }
    reset();
    render();

    int pb = 0;                                  /* previous button state (edge detect) */
    for (;;) {
        int k = sys_pollkey();
        if (k == 'q' || k == 27) { free(cv); return 0; }
        if (k == 'r') { reset(); render(); }

        int mx, my, b = sys_mouse(&mx, &my);
        if ((b & 1) && !(pb & 1) && mx >= 0) {       /* left click: zoom in on cursor */
            cx += (long long)(mx - W / 2) * scale;
            cy += (long long)(my - H / 2) * scale;
            scale = scale / 2;
            render();
        } else if ((b & 2) && !(pb & 2)) {           /* right click: zoom out */
            scale = scale * 2;
            render();
        }
        pb = b;
        sys_sleep(20);
    }
}
