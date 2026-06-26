/*
 * taskman.c — a graphical Task Manager, a userspace program (M1362).
 *
 * The first gfx app to render REAL text: it pulls the kernel's 8x16 console
 * font through the new sys_font() syscall and uses it to draw a live, auto-
 * refreshing process list (from sys_ps()) into a WM canvas — PID, state and
 * name per row, the whole line tinted by run-state (run=green, ready=cyan,
 * blocked=amber, stopped=grey). Refreshes ~1.4x a second. q/Esc quits.
 *
 * Launch: `run taskman` from the shell, or the Apps menu ("Task Manager").
 */
#include "ulib.h"

#define W 380
#define H 330

static unsigned *FB;
static unsigned char FONT[128 * 16];           /* 128 glyphs x 16 rows, from sys_font (8x16, MSB = left) */

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void ch(char c, int px, int py, unsigned col) {
    unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16];
    for (int r = 0; r < 16; r++)
        for (int b = 0; b < 8; b++)
            if ((g[r] >> (7 - b)) & 1) putpx(px + b, py + r, col);
}
static void text(const char *s, int x, int y, unsigned col) { for (int i = 0; s[i]; i++) { ch(s[i], x, y, col); x += 8; } }
static int has(const char *s, const char *sub) {            /* naive substring test */
    for (int i = 0; s[i]; i++) { int j = 0; while (sub[j] && s[i + j] == sub[j]) j++; if (!sub[j]) return 1; }
    return 0;
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("taskman: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("taskman: init failed\n"); return 1; }

    char ps[1200];
    for (;;) {
        long n = sys_ps(ps, sizeof ps - 1); if (n < 0) n = 0; ps[n] = 0;

        for (int i = 0; i < W * H; i++) FB[i] = 0x0C0C16;           /* dark background */
        text("Task Manager", 12, 8, 0x8FD0FF);
        for (int x = 8; x < W - 8; x++) putpx(x, 30, 0x2A2A3A);     /* header rule */

        int y = 40, count = 0;
        for (int i = 0; ps[i]; ) {
            int eol = i; while (ps[eol] && ps[eol] != '\n') eol++;
            char line[80]; int s = 0; for (int k = i; k < eol && s < 79; k++) line[s++] = ps[k]; line[s] = 0;
            if (s > 0) {
                unsigned col = 0xC8C8D2;                            /* default */
                if      (has(line, "run"))   col = 0x5CE070;        /* running: green */
                else if (has(line, "ready")) col = 0x60C8F0;        /* ready:   cyan  */
                else if (has(line, "block")) col = 0xE0B050;        /* blocked: amber */
                else if (has(line, "stop"))  col = 0x9090A0;        /* stopped: grey  */
                text(line, 12, y, col);
                y += 18; count++;
            }
            if (ps[eol] == '\n') eol++;
            i = eol;
            if (y > H - 22) break;
        }

        char foot[24]; int fi = 0; const char *lbl = "tasks: ";
        for (int k = 0; lbl[k]; k++) foot[fi++] = lbl[k];
        if (count >= 10) foot[fi++] = '0' + count / 10;
        foot[fi++] = '0' + count % 10; foot[fi] = 0;
        text(foot, 12, H - 20, 0x8890A0);

        sys_gfx_blit(FB);
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        sys_sleep(700);
    }
    return 0;
}
