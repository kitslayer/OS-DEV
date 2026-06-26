/*
 * gfont.c — a graphical character map / font viewer, a userspace program (M1389).
 *
 * Shows all 128 glyphs of the kernel's from-scratch 8x16 console font in a
 * hex-addressed 16x8 grid (column header = low nibble 0-F, row header = high
 * nibble 0-7), so any glyph's code reads straight off the headers. Arrow keys
 * (or WASD) move a cursor; the selected glyph is shown enlarged with its decimal
 * and hex code. Font via sys_font; pure render (redraws only when the cursor
 * moves). No FPU.
 *
 * Launch: `run gfont` from the shell, or the Apps menu ("Char Map").
 */
#include "ulib.h"

#define W 288
#define H 240
#define GX 22
#define GY 26
#define CW 16
#define CH 18

static unsigned *FB;
static unsigned char FONT[128 * 16];
static const char *HEX = "0123456789ABCDEF";

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void fill(int x, int y, int w, int h, unsigned c) { for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) putpx(x + i, y + j, c); }
static void chS(int u, int px, int py, int s, unsigned col) { if (u < 0 || u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16]; for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) fill(px + b * s, py + r * s, s, s, col); }
static void text(const char *s, int x, int y, unsigned col) { for (int i = 0; s[i]; i++) { chS((unsigned char)s[i], x, y, 1, col); x += 8; } }
static void putdec(int v, int x, int y, unsigned col) { char b[8]; int i = 0; if (v == 0) b[i++] = '0'; while (v) { b[i++] = '0' + v % 10; v /= 10; }
    char r[8]; int j = 0; while (i) r[j++] = b[--i]; r[j] = 0; text(r, x, y, col); }

/* ---- instrument-panel UI kit (M1443; see gconv.c M1430) ---- */
#define C_FACE_T  0x232D33u
#define C_FACE_B  0x161D21u
#define C_BEZHI   0x3C4A50u
#define C_BEZLO   0x0E1316u
#define C_SCREEN  0x0A0F0Cu
#define C_SCANLN  0x0D140Fu
#define C_AMBER   0xFFB23Eu
#define C_AMBERLO 0x7A521Au
#define C_LABEL   0xC6D0CCu
#define C_DIM     0x6E827Fu

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

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gfont: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gfont: init failed\n"); return 1; }

    int idx = 'A', pidx = -1, prevb = 0;
    for (;;) {
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        else if ((k == 0x11 || k == 'w') && idx >= 16) idx -= 16;     /* up    */
        else if ((k == 0x12 || k == 's') && idx < 112) idx += 16;     /* down  */
        else if ((k == 0x13 || k == 'a') && idx > 0)   idx -= 1;      /* left  */
        else if ((k == 0x14 || k == 'd') && idx < 127) idx += 1;      /* right */

        int mx, my, b = sys_mouse(&mx, &my);                          /* click a cell to select it (M1399) */
        if ((b & 1) && !(prevb & 1) && mx >= GX && my >= GY) {
            int col = (mx - GX) / CW, row = (my - GY) / CH;
            if (col >= 0 && col < 16 && row >= 0 && row < 8) idx = row * 16 + col;
        }
        prevb = b;

        if (idx != pidx) {
            pidx = idx;
            vgrad(0, 0, W, H, C_FACE_T, C_FACE_B);                     /* slate faceplate */
            bevel_up(0, 0, W, H, C_BEZHI, C_BEZLO);
            panel(2, 2, W - 4, 174);                                   /* recessed glyph-grid screen */

            for (int c = 0; c < 16; c++) { char h[2] = { HEX[c], 0 }; text(h, GX + c * CW + 4, 10, C_DIM); }   /* col headers (low nibble) */
            for (int r = 0; r < 8; r++)  { char h[2] = { HEX[r], 0 }; text(h, 6, GY + r * CH + 1, C_DIM); }    /* row headers (high nibble) */

            for (int r = 0; r < 8; r++) for (int c = 0; c < 16; c++) {
                int ch = r * 16 + c;
                int cx = GX + c * CW, cy = GY + r * CH;
                if (ch == idx) fill(cx, cy, CW, CH, 0x2E5AA0u);
                chS(ch, cx + 4, cy + 1, 1, ch == idx ? 0xFFFFFFu : C_LABEL);
            }

            panel(2, H - 60, W - 4, 58);                               /* recessed info readout */
            chS(idx, 19, H - 53, 3, C_AMBERLO); chS(idx, 18, H - 54, 3, C_AMBER);   /* enlarged glyph, amber w/ bloom */
            text("dec", 70, H - 50, C_DIM); putdec(idx, 102, H - 50, C_LABEL);
            text("hex 0x", 70, H - 28, C_DIM);
            { char hx[3] = { HEX[(idx >> 4) & 15], HEX[idx & 15], 0 }; text(hx, 70 + 6 * 8, H - 28, C_LABEL); }
        }

        sys_gfx_blit(FB);
        sys_sleep(60);
    }
    return 0;
}
