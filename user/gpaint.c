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
 * toolbar to change colour. Keys:  1-9/0 pick a palette colour · [ / ] shrink /
 * grow the brush · e erase (paint in white) · c clear · s save to PAINT.BMP ·
 * Esc quit.  Launch: `gpaint` from the shell, or the Apps menu.
 */
#include "ulib.h"

#define W  640
#define H  420
#define TB 22                          /* toolbar height (canvas is y in [TB, H)) */
#define NC 12                          /* palette entries */
#define SW 22                          /* palette swatch width */

static unsigned      *FB;
static unsigned char  FONT[128 * 16];
static int  cur = 0;                    /* selected palette index */
static int  brush = 3;                  /* brush radius in pixels */
static char msg[40];

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
    char s[64]; int p = 0;
    const char *pre = "brush "; for (int i = 0; pre[i]; i++) s[p++] = pre[i];
    if (brush >= 10) s[p++] = (char)('0' + brush / 10);
    s[p++] = (char)('0' + brush % 10);
    const char *post = " [ ] size  c clear  s save  Esc quit";   /* 'e' erase in the header doc */
    for (int i = 0; post[i]; i++) s[p++] = post[i];
    s[p] = 0;
    text(s, tx, 3, 0xC8D0F0);
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gpaint: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gpaint: init failed\n"); return 1; }
    rect(0, TB, W, H - TB, 0xFFFFFF);                    /* white canvas */
    draw_toolbar();
    sys_gfx_blit(FB);

    int lastdown = 0, lx = 0, ly = 0;
    for (;;) {
        int dirty = 0;
        int k = sys_pollkey();
        if (k >= 0 && k != 's' && msg[0]) msg[0] = 0;    /* any action clears a stale save message */
        if (k == 27) break;
        else if (k == 'c') { rect(0, TB, W, H - TB, 0xFFFFFF); msg[0] = 0; draw_toolbar(); dirty = 1; }
        else if (k == 'e') { cur = 1; draw_toolbar(); dirty = 1; }
        else if (k == '[') { if (brush > 1) brush--; draw_toolbar(); dirty = 1; }
        else if (k == ']') { if (brush < 24) brush++; draw_toolbar(); dirty = 1; }
        else if (k >= '1' && k <= '9') { int i = k - '1'; if (i < NC) { cur = i; draw_toolbar(); dirty = 1; } }
        else if (k == '0') { cur = 9; draw_toolbar(); dirty = 1; }
        else if (k == 's') {
            if (sys_savebmp("PAINT.BMP", &FB[TB * W], W, H - TB) >= 0) scpy(msg, "saved PAINT.BMP");
            else scpy(msg, "save failed");
            draw_toolbar(); dirty = 1;
        }

        int mx, my, b = sys_mouse(&mx, &my);
        if ((b & 1) && mx >= 0) {
            if (my < TB) {                               /* toolbar: pick a swatch */
                int i = (mx - 3) / SW; if (i >= 0 && i < NC && i != cur) { cur = i; draw_toolbar(); dirty = 1; }
                lastdown = 0;
            } else {                                     /* canvas: paint */
                if (lastdown) seg(lx, ly, mx, my, brush, PAL[cur]);
                else { if (msg[0]) { msg[0] = 0; draw_toolbar(); } disc(mx, my, brush, PAL[cur]); }
                lx = mx; ly = my; lastdown = 1; dirty = 1;
            }
        } else lastdown = 0;

        if (dirty) sys_gfx_blit(FB);
        sys_sleep(8);
    }
    return 0;
}
