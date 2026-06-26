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

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gfont: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gfont: init failed\n"); return 1; }

    int idx = 'A', pidx = -1;
    for (;;) {
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        else if ((k == 0x11 || k == 'w') && idx >= 16) idx -= 16;     /* up    */
        else if ((k == 0x12 || k == 's') && idx < 112) idx += 16;     /* down  */
        else if ((k == 0x13 || k == 'a') && idx > 0)   idx -= 1;      /* left  */
        else if ((k == 0x14 || k == 'd') && idx < 127) idx += 1;      /* right */

        if (idx != pidx) {
            pidx = idx;
            for (int i = 0; i < W * H; i++) FB[i] = 0x0A0C12;

            for (int c = 0; c < 16; c++) { char h[2] = { HEX[c], 0 }; text(h, GX + c * CW + 4, 10, 0x80C0E0); }   /* col headers (low nibble) */
            for (int r = 0; r < 8; r++)  { char h[2] = { HEX[r], 0 }; text(h, 6, GY + r * CH + 1, 0x80C0E0); }    /* row headers (high nibble) */

            for (int r = 0; r < 8; r++) for (int c = 0; c < 16; c++) {
                int ch = r * 16 + c;
                int cx = GX + c * CW, cy = GY + r * CH;
                if (ch == idx) fill(cx, cy, CW, CH, 0x2860C0);
                chS(ch, cx + 4, cy + 1, 1, ch == idx ? 0xFFFFFF : 0xB0B8C4);
            }

            fill(0, H - 60, W, 60, 0x12161E);                          /* info panel */
            chS(idx, 18, H - 54, 3, 0x8CF09A);                         /* enlarged glyph */
            text("dec", 70, H - 50, 0x808A9A); putdec(idx, 102, H - 50, 0xE0E8F4);
            text("hex 0x", 70, H - 28, 0x808A9A);
            { char hx[3] = { HEX[(idx >> 4) & 15], HEX[idx & 15], 0 }; text(hx, 70 + 6 * 8, H - 28, 0xE0E8F4); }
        }

        sys_gfx_blit(FB);
        sys_sleep(60);
    }
    return 0;
}
