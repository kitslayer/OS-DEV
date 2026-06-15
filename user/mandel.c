/*
 * mandel.c — a Mandelbrot set explorer, a userspace program.
 *
 * Escape-time rendering as text art: for each grid cell the complex point c is
 * iterated z = z*z + c until |z| > 2 (escape) or an iteration cap; the escape
 * speed picks a shading character. Userspace apps are built -mgeneral-regs-only
 * (no FPU/SSE), so all the maths is fixed-point Q16.16 with 64-bit products.
 * Arrow keys pan, +/- zoom, r resets. Runs ring-3 on the app text grid.
 */
#include "ulib.h"

#define W 43                   /* one less than APP_COLS (44) so a row + '\n' doesn't auto-wrap */
#define H 15
#define SH 16                 /* fixed-point: value = real * 65536 */
#define ONE (1 << SH)
#define MAXIT 90

/* shading by escape speed: few iterations (fast escape) -> light, many -> dense */
static const char PAL[] = " .,:;-=+ico*OX#%@";
#define NPAL ((int)sizeof(PAL) - 2)   /* exclude the trailing NUL; last index = in-set */

/* view: top-left corner (cx0,cy0) and span across the grid, all Q16.16 */
static long cx0, cy0, spanx, spany;

static void reset_view(void) {
    cx0 = -163840; spanx = 229376;   /* x in [-2.5, 1.0]  (Q16.16: 2.5*65536, 3.5*65536) */
    cy0 =  -91750; spany = 183500;   /* y in [-1.4, 1.4]  (1.4*65536, 2.8*65536) */
}

/* iterations before escape for c = (cr,ci) Q16.16; MAXIT if it stays bounded */
static int escape(long cr, long ci) {
    long zr = 0, zi = 0;
    for (int i = 0; i < MAXIT; i++) {
        long long zr2 = ((long long)zr * zr) >> SH;
        long long zi2 = ((long long)zi * zi) >> SH;
        if (zr2 + zi2 > ((long long)4 << SH)) return i;     /* |z|^2 > 4 */
        long long t = zr2 - zi2 + cr;                       /* zr' = zr^2 - zi^2 + cr */
        zi = (long)((((long long)zr * zi) >> (SH - 1)) + ci);/* zi' = 2*zr*zi + ci */
        zr = (long)t;
    }
    return MAXIT;
}

static void render(void) {
    sys_clear();
    print(" arrows pan   +/- zoom   r reset   q quit\n");
    char line[W + 2];
    for (int py = 0; py < H; py++) {
        long ci = cy0 + (long)((long long)py * spany / H);
        for (int px = 0; px < W; px++) {
            long cr = cx0 + (long)((long long)px * spanx / W);
            int it = escape(cr, ci);
            line[px] = (it >= MAXIT) ? PAL[NPAL] : PAL[it * NPAL / MAXIT];
        }
        line[W] = '\n'; line[W + 1] = 0; print(line);
    }
}

int main(void) {
    reset_view();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 27) return 0;
        else if (k == 'r') reset_view();
        else if (k == 0x13) cx0 -= spanx / 6;                    /* left  */
        else if (k == 0x14) cx0 += spanx / 6;                    /* right */
        else if (k == 0x11) cy0 -= spany / 6;                    /* up    */
        else if (k == 0x12) cy0 += spany / 6;                    /* down  */
        else if (k == '+' || k == '=') {                         /* zoom in: keep centre fixed */
            long nx = spanx * 3 / 5, ny = spany * 3 / 5;
            cx0 += (spanx - nx) / 2; cy0 += (spany - ny) / 2; spanx = nx; spany = ny;
        }
        else if (k == '-' || k == '_') {                         /* zoom out */
            long nx = spanx * 5 / 3, ny = spany * 5 / 3;
            cx0 -= (nx - spanx) / 2; cy0 -= (ny - spany) / 2; spanx = nx; spany = ny;
        }
        else continue;
        render();
    }
}
