/*
 * gpaint.c — a graphical paint program (M1701).
 *
 * The OS had `paint`, an ASCII-art text canvas; this is the real thing: a
 * mouse-driven pixel canvas with a colour palette, a resizable round brush, and
 * export to a 24-bit BMP on the FAT32 disk. A ring-3 gfx app (a w*h XRGB canvas
 * via sys_gfx_init/blit, cursor + buttons via sys_mouse, the shared bitmap font
 * for the toolbar), integer-only — no floating point, so it uses the ordinary
 * userspace build (unlike calc/sheet/plot, which need SSE for their math).
 *
 * Drag the left button on the canvas to paint; click a swatch in the top
 * toolbar to change colour. Tools (keys): f freehand pen · l line · r rectangle
 * · b filled box · o ellipse · g flood-fill — line/rect/box/ellipse preview live
 * as you drag out from the press point. Other keys: 1-9/0 pick a palette colour
 * · [ / ] shrink / grow the brush · e erase (paint white) · c clear · s save to
 * PAINT.BMP · Esc quit.  Launch: `gpaint` from the shell, or the Apps menu.
 */
#include "ulib.h"

#define W  640
#define H  420
#define TB 22                          /* toolbar height (canvas is y in [TB, H)) */
#define NC 12                          /* palette entries */
#define SW 20                          /* palette swatch width */

static unsigned      *FB;
static unsigned char  FONT[128 * 16];
static int  cur = 0;                    /* selected palette index */
static int  brush = 3;                  /* brush radius in pixels */
static char msg[40];
enum { T_FREE, T_LINE, T_RECT, T_BOX, T_OVAL, T_FILL, T_N };
static int  tool = T_FREE;
static const char *TOOLNAME[T_N] = { "pen", "line", "rect", "box", "oval", "fill" };
static unsigned *BK;                    /* canvas backup, for live shape preview */

static const unsigned PAL[NC] = {
    0x000000, 0xFFFFFF, 0x808080, 0xE03030, 0xF08000, 0xF0D000,
    0x30B030, 0x30C0C0, 0x3060E0, 0x8030C0, 0xE040A0, 0x8B5A2B
};

static void scpy(char *d, const char *s) { int i = 0; for (; s[i]; i++) d[i] = s[i]; d[i] = 0; }

/* toolbar drawing writes anywhere; canvas drawing (putpx/disc/seg) clips to [TB,H) */
static void rect(int x, int y, int w, int h, unsigned c) {
    for (int yy = y; yy < y + h; yy++) for (int xx = x; xx < x + w; xx++)
        if (xx >= 0 && xx < W && yy >= 0 && yy < H) FB[yy * W + xx] = c;
}
static void ch(char c, int px, int py, unsigned col) {
    unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16];
    for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++)
        if ((g[r] >> (7 - b)) & 1) { int X = px + b, Y = py + r; if (X >= 0 && X < W && Y >= 0 && Y < H) FB[Y * W + X] = col; }
}
static void text(const char *t, int x, int y, unsigned col) { for (int i = 0; t[i]; i++) { ch(t[i], x, y, col); x += 8; } }

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= TB && y < H) FB[y * W + x] = c; }
static void disc(int cx, int cy, int r, unsigned c) {
    for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) if (x * x + y * y <= r * r) putpx(cx + x, cy + y, c);
}
static void seg(int x0, int y0, int x1, int y1, int r, unsigned c) {   /* stamp discs along a segment */
    int dx = x1 - x0, dy = y1 - y0;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, n = ax > ay ? ax : ay; if (n < 1) n = 1;
    for (int i = 0; i <= n; i++) disc(x0 + dx * i / n, y0 + dy * i / n, r, c);
}
static void cfill(int x0, int y0, int x1, int y1, unsigned c) {        /* filled rect, canvas-clipped */
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) putpx(x, y, c);
}
/* Ellipse outline in the bounding box (x0,y0)-(x1,y1), stamped with the brush.
 * Integer midpoint algorithm — no floating point (gpaint builds without SSE). */
static void oval(int x0, int y0, int x1, int y1, int r, unsigned c) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    long a = (x1 - x0) / 2, b = (y1 - y0) / 2, cx = x0 + a, cy = y0 + b;
    if (a <= 0 || b <= 0) { seg(x0, cy, x1, cy, r, c); return; }
    long a2 = a * a, b2 = b * b, x = 0, y = b, dx = 0, dy = 2 * a2 * y;
    long d = b2 - a2 * b + a2 / 4;
    while (dx < dy) {                                                   /* region 1 */
        disc(cx + x, cy + y, r, c); disc(cx - x, cy + y, r, c); disc(cx + x, cy - y, r, c); disc(cx - x, cy - y, r, c);
        if (d < 0) { x++; dx += 2 * b2; d += dx + b2; }
        else { x++; y--; dx += 2 * b2; dy -= 2 * a2; d += dx - dy + b2; }
    }
    d = b2 * (2 * x + 1) * (2 * x + 1) / 4 + a2 * (y - 1) * (y - 1) - a2 * b2;
    while (y >= 0) {                                                   /* region 2 */
        disc(cx + x, cy + y, r, c); disc(cx - x, cy + y, r, c); disc(cx + x, cy - y, r, c); disc(cx - x, cy - y, r, c);
        if (d > 0) { y--; dy -= 2 * a2; d += a2 - dy; }
        else { y--; x++; dx += 2 * b2; dy -= 2 * a2; d += dx - dy + a2; }
    }
}
/* Scanline flood-fill of the connected same-colour region at (sx,sy). Iterative
 * (an explicit seed stack, one seed per contiguous span) — no recursion. */
static void flood(int sx, int sy, unsigned newc) {
    if (sx < 0 || sx >= W || sy < TB || sy >= H) return;
    unsigned oldc = FB[sy * W + sx];
    if (oldc == newc) return;
    long cap = (long)W * (H - TB);                                    /* generous seed-stack bound */
    int *stk = (int *)malloc((unsigned long)cap * 2 * sizeof(int));
    if (!stk) return;
    long sp = 0; stk[sp++] = sx; stk[sp++] = sy;
    while (sp > 0) {
        int y = stk[--sp], x = stk[--sp];
        if (FB[y * W + x] != oldc) continue;
        int xl = x; while (xl > 0 && FB[y * W + xl - 1] == oldc) xl--;
        int xr = x; while (xr < W - 1 && FB[y * W + xr + 1] == oldc) xr++;
        for (int xx = xl; xx <= xr; xx++) FB[y * W + xx] = newc;
        for (int ny = y - 1; ny <= y + 1; ny += 2) {                  /* seed spans above and below */
            if (ny < TB || ny >= H) continue;
            int xx = xl;
            while (xx <= xr) {
                while (xx <= xr && FB[ny * W + xx] != oldc) xx++;
                if (xx > xr) break;
                int start = xx;
                while (xx <= xr && FB[ny * W + xx] == oldc) xx++;
                if (sp < cap * 2 - 2) { stk[sp++] = xx - 1; stk[sp++] = ny; }
                (void)start;
            }
        }
    }
    free(stk);
}
static void canvas_copy(unsigned *dst, const unsigned *src) { long n = (long)W * (H - TB); for (long i = 0; i < n; i++) dst[i] = src[i]; }

static void draw_toolbar(void) {
    rect(0, 0, W, TB, 0x1A1A24);
    for (int i = 0; i < NC; i++) {
        int x = 3 + i * SW;
        rect(x, 3, SW - 3, TB - 6, PAL[i]);
        if (i == cur) {                                  /* white outline on the selected swatch */
            rect(x - 1, 1, SW - 1, 1, 0xFFFFFF); rect(x - 1, TB - 2, SW - 1, 1, 0xFFFFFF);
            rect(x - 1, 1, 1, TB - 2, 0xFFFFFF); rect(x + SW - 3, 1, 1, TB - 2, 0xFFFFFF);
        }
    }
    int tx = 3 + NC * SW + 8;
    if (msg[0]) { text(msg, tx, 3, 0x46E05A); return; }    /* save-confirmation replaces the hint */
    char s[80]; int p = 0;
    const char *tn = TOOLNAME[tool]; for (int i = 0; tn[i]; i++) s[p++] = tn[i];
    const char *pre = "  br "; for (int i = 0; pre[i]; i++) s[p++] = pre[i];
    if (brush >= 10) s[p++] = (char)('0' + brush / 10);
    s[p++] = (char)('0' + brush % 10);
    const char *post = "  [ ]size  f l r b o g  s save  Esc";   /* 'e' erase / 'c' clear in the header doc */
    for (int i = 0; post[i]; i++) s[p++] = post[i];
    s[p] = 0;
    text(s, tx, 3, 0xC8D0F0);
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gpaint: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    BK = (unsigned *)malloc((unsigned long)W * H * 4);   /* canvas backup for shape preview */
    if (!FB || !BK || sys_font(FONT, sizeof FONT) < 0) { print("gpaint: init failed\n"); return 1; }
    rect(0, TB, W, H - TB, 0xFFFFFF);                    /* white canvas */
    draw_toolbar();
    sys_gfx_blit(FB);

    int wasdown = 0, sx = 0, sy = 0, lx = 0, ly = 0;
    for (;;) {
        int dirty = 0;
        int k = sys_pollkey();
        if (k >= 0 && k != 's' && msg[0]) { msg[0] = 0; draw_toolbar(); dirty = 1; }   /* clear stale save msg */
        if (k == 27) break;
        else if (k == 'c') { rect(0, TB, W, H - TB, 0xFFFFFF); draw_toolbar(); dirty = 1; }
        else if (k == 'e') { cur = 1; draw_toolbar(); dirty = 1; }
        else if (k == '[') { if (brush > 1) brush--; draw_toolbar(); dirty = 1; }
        else if (k == ']') { if (brush < 24) brush++; draw_toolbar(); dirty = 1; }
        else if (k == 'f') { tool = T_FREE; draw_toolbar(); dirty = 1; }
        else if (k == 'l') { tool = T_LINE; draw_toolbar(); dirty = 1; }
        else if (k == 'r') { tool = T_RECT; draw_toolbar(); dirty = 1; }
        else if (k == 'b') { tool = T_BOX;  draw_toolbar(); dirty = 1; }
        else if (k == 'o') { tool = T_OVAL; draw_toolbar(); dirty = 1; }
        else if (k == 'g') { tool = T_FILL; draw_toolbar(); dirty = 1; }
        else if (k >= '1' && k <= '9') { int i = k - '1'; if (i < NC) { cur = i; draw_toolbar(); dirty = 1; } }
        else if (k == '0') { cur = 9; draw_toolbar(); dirty = 1; }
        else if (k == 's') {
            if (sys_savebmp("PAINT.BMP", &FB[TB * W], W, H - TB) >= 0) scpy(msg, "saved PAINT.BMP");
            else scpy(msg, "save failed");
            draw_toolbar(); dirty = 1;
        }

        int mx, my, b = sys_mouse(&mx, &my);
        int down  = (b & 1) && mx >= 0 && my >= TB;      /* left button in the canvas */
        int inbar = (b & 1) && mx >= 0 && my < TB;       /* left button in the toolbar */
        if (inbar) {
            int i = (mx - 3) / SW; if (i >= 0 && i < NC && i != cur) { cur = i; draw_toolbar(); dirty = 1; }
        } else if (down) {
            unsigned c = PAL[cur];
            if (!wasdown && tool != T_FREE && tool != T_FILL) { sx = mx; sy = my; canvas_copy(&BK[TB * W], &FB[TB * W]); }
            if (tool == T_FREE) {
                if (wasdown) seg(lx, ly, mx, my, brush, c); else disc(mx, my, brush, c);
                dirty = 1;
            } else if (tool == T_FILL) {
                if (!wasdown) { flood(mx, my, c); dirty = 1; }
            } else if (!wasdown || mx != lx || my != ly) {   /* shape: re-preview only when the cursor moves */
                canvas_copy(&FB[TB * W], &BK[TB * W]);        /* restore, then draw the preview shape */
                if (tool == T_LINE) seg(sx, sy, mx, my, brush, c);
                else if (tool == T_RECT) { seg(sx, sy, mx, sy, brush, c); seg(mx, sy, mx, my, brush, c); seg(mx, my, sx, my, brush, c); seg(sx, my, sx, sy, brush, c); }
                else if (tool == T_BOX)  cfill(sx, sy, mx, my, c);
                else if (tool == T_OVAL) oval(sx, sy, mx, my, brush, c);
                dirty = 1;
            }
            lx = mx; ly = my;
        }
        wasdown = down;

        if (dirty) sys_gfx_blit(FB);
        sys_sleep(8);
    }
    return 0;
}
