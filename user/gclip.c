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

/* ---- instrument-panel UI kit (M1431; see gconv.c M1430). A text app, so only
 * the faceplate shell — the content keeps readable colours, not amber. ---- */
#define C_FACE_T  0x232D33u
#define C_FACE_B  0x161D21u
#define C_BEZHI   0x3C4A50u
#define C_BEZLO   0x0E1316u
#define C_SCREEN  0x0A0F0Cu
#define C_SCANLN  0x0D140Fu
#define C_AMBERLO 0x7A521Au
#define C_LABEL   0xC6D0CCu
#define C_DIM     0x6E827Fu
#define C_LED     0x46E0A0u

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
static void led(int x, int y) {
    fill(x, y, 9, 9, C_BEZLO); fill(x + 1, y + 1, 7, 7, 0x1A6E50u);
    fill(x + 2, y + 2, 5, 5, C_LED); putpx(x + 3, y + 3, 0xCFFFE8u);
}

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
            vgrad(0, 0, W, H, C_FACE_T, C_FACE_B);                 /* slate faceplate */
            bevel_up(0, 0, W, H, C_BEZHI, C_BEZLO);
            text("CLIPBOARD", 12, 10, C_LABEL);
            fill(12, 27, 76, 2, C_AMBERLO);
            led(W - 24, 11);
            char hd[32]; int p = 0;
            int v = n; char t[8]; int ti = 0; if (v == 0) t[ti++] = '0'; while (v) { t[ti++] = '0' + v % 10; v /= 10; }
            while (ti) hd[p++] = t[--ti];
            const char *b2 = " bytes"; while (*b2) hd[p++] = *b2++; hd[p] = 0;
            text(hd, W - 36 - p * 8, 10, C_DIM);

            int sy = 32, sh = H - 32 - 22;                         /* recessed content screen */
            panel(8, sy, W - 16, sh);
            if (n == 0) text("(empty)", 20, sy + 12, C_DIM);
            else {
                int col = 0, row = 0;
                for (int i = 0; i < n && row < ROWS; i++) {
                    char c = clip[i];
                    if (c == '\n') { col = 0; row++; continue; }
                    if (c == '\t') { col = (col + 4) & ~3; if (col >= COLS) { col = 0; row++; } continue; }
                    if (c < 32) c = '.';                    /* non-printable -> dot */
                    if (row < ROWS) ch(c, 18 + col * 8, sy + 10 + row * 18, C_LABEL);
                    if (++col >= COLS) { col = 0; row++; }
                }
            }
            text("c clear    q quit", 12, H - 14, C_DIM);
            sys_gfx_blit(FB);
        }
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        else if (k == 'c' || k == 'C') { sys_clip_set("", 0); plen = -2; }   /* clear + force redraw */
        sys_sleep(250);
    }
    return 0;
}
