/*
 * paint.c — a graphical mouse-driven paint program.
 *
 * The first app to use the mouse: it opens a pixel canvas (the graphics window
 * API), reads the cursor + buttons with sys_mouse(), and paints where you drag.
 * Keys 1-8 pick a colour, +/- change the brush size, g flood-fills under the
 * cursor, b/l/r switch brush/line/rectangle tools (line & rect drag with a live
 * rubber-band preview), c clears, q/Esc quits.
 * Pure integer math (no FPU).
 */
#include "ulib.h"

#define W 300
#define H 200
#define BG 0x101018u

static unsigned int *cv;

static const unsigned int palette[8] = {
    0xF0F0F0, 0xFF4040, 0x40E060, 0x4090FF, 0xFFD040, 0xFF60D0, 0x40E0E0, 0x101018,
};

static void putpx(int x, int y, unsigned int c) {
    if (x >= 0 && x < W && y >= 0 && y < H) cv[y * W + x] = c;
}
static void disc(int cx, int cy, int r, unsigned int c) {     /* filled round brush */
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) putpx(cx + dx, cy + dy, c);
}
static int iabs(int v) { return v < 0 ? -v : v; }
static void stroke(int x0, int y0, int x1, int y1, int r, unsigned int c) {
    int dx = iabs(x1 - x0), sx = x0 < x1 ? 1 : -1;            /* Bresenham line of discs */
    int dy = -iabs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        disc(x0, y0, r, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Flood fill (paint bucket): recolour the 4-connected region of the same colour
 * as the seed pixel. Recolours on push so each pixel is queued once (stack <= W*H). */
static void fill(int sx, int sy, unsigned int newc) {
    if (sx < 0 || sx >= W || sy < 0 || sy >= H) return;
    unsigned int target = cv[sy * W + sx];
    if (target == newc) return;
    int *stk = malloc((unsigned long)W * H * sizeof(int));
    if (!stk) return;
    int sp = 0;
    cv[sy * W + sx] = newc; stk[sp++] = sy * W + sx;
    while (sp > 0) {
        int idx = stk[--sp], x = idx % W, y = idx / W;
        if (x > 0     && cv[idx - 1] == target) { cv[idx - 1] = newc; stk[sp++] = idx - 1; }
        if (x < W - 1 && cv[idx + 1] == target) { cv[idx + 1] = newc; stk[sp++] = idx + 1; }
        if (y > 0     && cv[idx - W] == target) { cv[idx - W] = newc; stk[sp++] = idx - W; }
        if (y < H - 1 && cv[idx + W] == target) { cv[idx + W] = newc; stk[sp++] = idx + W; }
    }
    free(stk);
}

static void draw_palette(int sel) {
    for (int p = 0; p < 8; p++) {                            /* colour swatches, top-left */
        int x0 = 3 + p * 16, y0 = 3;
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < 14; x++) cv[(y0 + y) * W + (x0 + x)] = palette[p];
        if (p == sel)                                        /* white outline on the selected one */
            for (int x = -1; x < 15; x++) { putpx(x0 + x, y0 - 1, 0xFFFFFF); putpx(x0 + x, y0 + 12, 0xFFFFFF); }
    }
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("paint: graphics init failed\n"); return 1; }
    cv = malloc((unsigned long)W * H * 4);
    if (!cv) { print("paint: out of memory\n"); return 1; }
    for (int i = 0; i < W * H; i++) cv[i] = BG;

    int col = 1, brush = 2, lx = -1, ly = -1;
    int mode = 0, anchored = 0, ax = 0, ay = 0;          /* mode: 0 brush, 1 line, 2 rect; rubber-band anchor */
    unsigned int *bk = 0;                                /* canvas backup for shape preview (lazy) */
    draw_palette(col);
    for (;;) {
        int k = sys_pollkey();
        if (k == 'q' || k == 27) { free(cv); free(bk); return 0; }
        else if (k >= '1' && k <= '8') col = k - '1';
        else if (k == 'c') { for (int i = 0; i < W * H; i++) cv[i] = BG; }
        else if (k == '+' || k == '=') { if (brush < 20) brush++; }
        else if (k == '-' || k == '_') { if (brush > 1) brush--; }
        else if (k == 'g') { int mx, my; sys_mouse(&mx, &my); fill(mx, my, palette[col]); }   /* flood fill under the cursor */
        else if (k == 'b') mode = 0;                         /* brush (freehand) */
        else if (k == 'l') mode = 1;                         /* line tool */
        else if (k == 'r') mode = 2;                         /* rectangle tool */

        int x, y;
        int b = sys_mouse(&x, &y);
        if ((b & 1) && x >= 0 && y >= 0) {                   /* left button */
            if (mode == 0) {                                 /* brush: freehand stroke */
                if (lx < 0) { lx = x; ly = y; }
                stroke(lx, ly, x, y, brush, palette[col]);
                lx = x; ly = y;
            } else {                                         /* line/rect: rubber-band preview */
                if (!anchored) {                             /* press: set anchor + back up the canvas */
                    ax = x; ay = y; anchored = 1;
                    if (!bk) bk = malloc((unsigned long)W * H * 4);
                    if (bk) for (int i = 0; i < W * H; i++) bk[i] = cv[i];
                } else if (bk) {                             /* drag: restore, then redraw the preview */
                    for (int i = 0; i < W * H; i++) cv[i] = bk[i];
                }
                if (mode == 1) stroke(ax, ay, x, y, brush, palette[col]);   /* line */
                else {                                       /* rectangle outline (4 edges) */
                    stroke(ax, ay, x,  ay, brush, palette[col]);
                    stroke(ax, y,  x,  y,  brush, palette[col]);
                    stroke(ax, ay, ax, y,  brush, palette[col]);
                    stroke(x,  ay, x,  y,  brush, palette[col]);
                }
            }
        } else if ((b & 2) && x >= 0 && y >= 0) {            /* right button: erase */
            if (lx < 0) { lx = x; ly = y; }
            stroke(lx, ly, x, y, brush + 2, BG);
            lx = x; ly = y;
        } else { lx = ly = -1; anchored = 0; }               /* button up: end stroke / commit the shape */

        draw_palette(col);
        sys_gfx_blit(cv);
        sys_sleep(12);
    }
}
