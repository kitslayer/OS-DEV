/*
 * gclip.c — a clipboard viewer, a userspace program (M1417).
 *
 * Shows the system clipboard (sys_clip_get) live — word-wrapped, with a byte
 * count — so you can see what the editor, gpass, etc. copied. It polls a few
 * times a second and redraws when the contents change. c clears the clipboard,
 * q/Esc quits.
 *
 * Launch: `run gclip` from the shell, or the Apps menu ("Clipboard").
 */
#include "ulib.h"

#define W 460
#define H 280
#define COLS 54                   /* wrap width in glyphs */
#define ROWS 11                   /* visible rows */

static unsigned *FB;
static unsigned char FONT[128 * 16];

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void fill(int x0, int y0, int w, int h, unsigned c) { for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) putpx(x, y, c); }
static void ch(char c, int px, int py, unsigned col) { unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16]; for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) putpx(px + b, py + r, col); }
static void text(const char *s, int x, int y, unsigned col) { for (int i = 0; s[i]; i++) { ch(s[i], x, y, col); x += 8; } }

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gclip: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gclip: init failed\n"); return 1; }

    static char clip[2048];
    int plen = -2;                                         /* force the first draw */
    for (;;) {
        int n = sys_clip_get(clip, sizeof clip - 1);
        if (n < 0) n = 0;
        clip[n] = 0;

        if (n != plen) {                                   /* redraw when the length changes (good-enough change test) */
            plen = n;
            for (int i = 0; i < W * H; i++) FB[i] = 0x121620;
            text("Clipboard", 12, 8, 0x8FD0FF);
            char hd[32]; int p = 0; const char *a = "         "; while (*a) hd[p++] = *a++;
            int v = n; char t[8]; int ti = 0; if (v == 0) t[ti++] = '0'; while (v) { t[ti++] = '0' + v % 10; v /= 10; }
            while (ti) hd[p++] = t[--ti];
            const char *b2 = " bytes"; while (*b2) hd[p++] = *b2++;
            hd[p] = 0;
            text(hd, W - (int)p * 8 - 12, 8, 0x808A9A);
            for (int x = 8; x < W - 8; x++) putpx(x, 28, 0x2A3040);
            fill(8, 32, W - 16, H - 64, 0x0C1018);          /* content panel */

            if (n == 0) text("(empty)", 18, 44, 0x707888);
            else {
                int col = 0, row = 0;
                for (int i = 0; i < n && row < ROWS; i++) {
                    char c = clip[i];
                    if (c == '\n') { col = 0; row++; continue; }
                    if (c == '\t') { col = (col + 4) & ~3; if (col >= COLS) { col = 0; row++; } continue; }
                    if (c < 32) c = '.';                    /* non-printable -> dot */
                    if (row < ROWS) ch(c, 18 + col * 8, 40 + row * 18, 0xCFE0C0);
                    if (++col >= COLS) { col = 0; row++; }
                }
            }
            text("c: clear clipboard   q: quit", 12, H - 16, 0x707888);
            sys_gfx_blit(FB);
        }
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        else if (k == 'c' || k == 'C') { sys_clip_set("", 0); plen = -2; }   /* clear + force redraw */
        sys_sleep(250);
    }
    return 0;
}
