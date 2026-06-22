/*
 * paint.c — a graphical mouse-driven paint program.
 *
 * The first app to use the mouse: it opens a pixel canvas (the graphics window
 * API), reads the cursor + buttons with sys_mouse(), and paints where you drag.
 * Keys 1-8 pick a colour, +/- change the brush size, g flood-fills under the
 * cursor, b/l/r/o switch brush/line/rectangle/circle tools (the shape tools drag
 * with a live rubber-band preview), c clears, s saves the canvas to PAINT.BMP,
 * L loads PAINT.BMP back, q/Esc quits.
 * Pure integer math (no FPU).
 */
#include "ulib.h"

#define W 300
#define H 200
#define BG 0x101018u
#define SAVEFILE "PAINT.BMP"

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

static int isqrt(int n) { if (n <= 0) return 0; int x = n, y = (x + 1) / 2; while (y < x) { x = y; y = (x + n / x) / 2; } return x; }
/* Circle outline (midpoint algorithm), brush-thick via disc at each octant point. */
static void circ(int cx, int cy, int rad, int br, unsigned int c) {
    int x = rad, y = 0, err = 0;
    while (x >= y) {
        disc(cx + x, cy + y, br, c); disc(cx + y, cy + x, br, c);
        disc(cx - y, cy + x, br, c); disc(cx - x, cy + y, br, c);
        disc(cx - x, cy - y, br, c); disc(cx - y, cy - x, br, c);
        disc(cx + y, cy - x, br, c); disc(cx + x, cy - y, br, c);
        y++; if (err <= 0) err += 2 * y + 1;
        if (err > 0) { x--; err -= 2 * x + 1; }
    }
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

/* Load SAVEFILE (a 24-bit BMP, the exact format sys_savebmp writes: bottom-up
 * rows, each padded to a 4-byte boundary) back into the canvas. Parses the
 * 54-byte header inline and bounds-checks everything: the file must be >= 54
 * bytes and 24bpp, dimensions are clamped to W/H (never writing past cv), and a
 * pixel is read only if it lies within the file buffer (so a truncated/forged
 * file can't over-read). A missing/malformed file fails gracefully (returns 0,
 * leaving the canvas untouched). Returns 1 if it loaded anything. */
static int load_bmp(void) {
    long cap = 54 + (long)W * 4 * H + 4 * H + 64;        /* header + worst-case (full-row pad) pixels */
    unsigned char *buf = malloc((unsigned long)cap);
    if (!buf) return 0;
    long n = sys_readfile(SAVEFILE, buf, (unsigned long)cap);
    if (n < 54 || buf[0] != 'B' || buf[1] != 'M') { free(buf); return 0; }   /* missing or not a BMP */
    unsigned long off = (unsigned long)buf[10] | ((unsigned long)buf[11] << 8) |
                        ((unsigned long)buf[12] << 16) | ((unsigned long)buf[13] << 24);
    int iw = (int)((unsigned)buf[18] | ((unsigned)buf[19] << 8) |
                   ((unsigned)buf[20] << 16) | ((unsigned)buf[21] << 24));
    int ih = (int)((unsigned)buf[22] | ((unsigned)buf[23] << 8) |
                   ((unsigned)buf[24] << 16) | ((unsigned)buf[25] << 24));
    int bpp = (int)((unsigned)buf[28] | ((unsigned)buf[29] << 8));
    if (bpp != 24 || iw <= 0 || ih <= 0 || off > (unsigned long)n) { free(buf); return 0; }
    int cw = iw < W ? iw : W, ch = ih < H ? ih : H;      /* clamp to the canvas; never write past cv */
    int stride = iw * 3; stride += (4 - (stride & 3)) & 3;   /* rows padded to 4 bytes */
    for (int y = 0; y < ch; y++) {
        long rowoff = (long)off + (long)(ih - 1 - y) * stride;   /* bottom-up */
        for (int x = 0; x < cw; x++) {
            long p = rowoff + (long)x * 3;
            if (p + 2 >= n) continue;                    /* guard against a truncated file */
            cv[y * W + x] = ((unsigned)buf[p+2] << 16) |  /* R */
                            ((unsigned)buf[p+1] << 8)  |  /* G */
                             (unsigned)buf[p];            /* B */
        }
    }
    free(buf);
    return 1;
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("paint: graphics init failed\n"); return 1; }
    cv = malloc((unsigned long)W * H * 4);
    if (!cv) { print("paint: out of memory\n"); return 1; }
    for (int i = 0; i < W * H; i++) cv[i] = BG;

    int col = 1, brush = 2, lx = -1, ly = -1;
    int mode = 0, anchored = 0, ax = 0, ay = 0;          /* mode: 0 brush, 1 line, 2 rect; rubber-band anchor */
    unsigned int *bk = 0;                                /* canvas backup for shape preview (lazy) */
    int notice = 0; unsigned int notecol = 0;            /* brief save/load flash (frames left + colour) */
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
        else if (k == 'o') mode = 3;                         /* circle tool (drag out from centre) */
        else if (k == 's') {                                 /* save the canvas to PAINT.BMP */
            int ok = sys_savebmp(SAVEFILE, cv, W, H) == 0;
            notice = 40; notecol = ok ? 0x40E060 : 0xFF4040;   /* green ok / red fail */
        }
        else if (k == 'L') {                                 /* load PAINT.BMP back into the canvas */
            int ok = load_bmp();
            notice = 40; notecol = ok ? 0x4090FF : 0xFF4040;   /* blue ok / red fail */
        }

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
                else if (mode == 3) circ(ax, ay, isqrt((x - ax) * (x - ax) + (y - ay) * (y - ay)), brush, palette[col]);   /* circle from centre */
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
        /* Brief save/load flash: a small swatch in the top-right corner. Drawn
         * over a saved copy of that region and restored right after the blit so
         * it stays purely visual — it never gets baked into the canvas (a save
         * taken while it shows captures the real drawing, not the indicator). */
        if (notice > 0) {
            unsigned int save[8 * 8];
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    save[y * 8 + x] = cv[y * W + (W - 9 + x)];
                    cv[y * W + (W - 9 + x)] = notecol;
                }
            sys_gfx_blit(cv);
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) cv[y * W + (W - 9 + x)] = save[y * 8 + x];
            notice--;
        } else {
            sys_gfx_blit(cv);
        }
        sys_sleep(12);
    }
}
