/*
 * gpass.c — a password generator, a userspace program (M1416).
 *
 * Generates a random password from a mixed charset (upper/lower/digits/symbols);
 * SPACE or r makes a new one, w/Up and s/Down change the length (6..32), c copies
 * it to the system clipboard (middle-click pastes it elsewhere), q/Esc quits.
 * A small LCG seeded from the uptime supplies the randomness.
 *
 * Launch: `run gpass` from the shell, or the Apps menu ("Password Gen").
 */
#include "ulib.h"

#define W 420
#define H 190

static unsigned *FB;
static unsigned char FONT[128 * 16];
static const char *CS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!@#$%^&*-_=+";

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void fill(int x0, int y0, int w, int h, unsigned c) { for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) putpx(x, y, c); }
static void box(int x, int y, int w, int h, unsigned c) { fill(x, y, w, 1, c); fill(x, y + h - 1, w, 1, c); fill(x, y, 1, h, c); fill(x + w - 1, y, 1, h, c); }
static void chS(char c, int px, int py, int s, unsigned col) { unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16]; for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) fill(px + b * s, py + r * s, s, s, col); }
static void textS(const char *t, int x, int y, int s, unsigned col) { for (int i = 0; t[i]; i++) { chS(t[i], x, y, s, col); x += 8 * s; } }
static void text(const char *t, int x, int y, unsigned col) { for (int i = 0; t[i]; i++) { chS(t[i], x, y, 1, col); x += 8; } }

static unsigned long seed = 88172645463325252UL;
static int rnd(void) { seed = seed * 6364136223846793005UL + 1442695040888963407UL; return (int)((seed >> 33) & 0x7FFFFFFF); }
static void gen(char *pw, int len) {
    int cl = 0; while (CS[cl]) cl++;
    for (int i = 0; i < len; i++) pw[i] = CS[rnd() % cl];
    pw[len] = 0;
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gpass: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gpass: init failed\n"); return 1; }
    seed ^= (unsigned long)sys_uptime_ms() * 2654435761UL + 1;

    int len = 16, copied = 0, dirty = 1;
    char pw[40];
    gen(pw, len);

    for (;;) {
        if (dirty) {
            for (int i = 0; i < W * H; i++) FB[i] = 0x121620;
            text("Password Generator", 14, 12, 0x8FD0FF);
            box(14, 44, W - 28, 50, 0x2A3850);
            fill(15, 45, W - 30, 48, 0x0C1018);
            int s = len <= 22 ? 2 : 1;
            textS(pw, 26, 44 + (50 - 16 * s) / 2, s, 0x7CF0A0);     /* the password, vertically centred in the box */

            char ln[24]; int p = 0; const char *a = "length: "; while (*a) ln[p++] = *a++;
            int v = len; char t[4]; int ti = 0; if (v == 0) t[ti++] = '0'; while (v) { t[ti++] = '0' + v % 10; v /= 10; }
            while (ti) ln[p++] = t[--ti];
            ln[p] = 0;
            text(ln, 14, 110, 0xC0C8D6);
            if (copied) text("copied to clipboard", 150, 110, 0x70E090);

            text("space: new   w/s: length   c: copy   q: quit", 14, H - 16, 0x707888);
            sys_gfx_blit(FB);
            dirty = 0;
        }
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        else if (k == ' ' || k == 'r' || k == 'R') { gen(pw, len); copied = 0; dirty = 1; }
        else if ((k == 'w' || k == 0x11) && len < 32) { len++; gen(pw, len); copied = 0; dirty = 1; }
        else if ((k == 's' || k == 0x12) && len > 6) { len--; gen(pw, len); copied = 0; dirty = 1; }
        else if (k == 'c' || k == 'C') { sys_clip_set(pw, len); copied = 1; dirty = 1; }
        sys_sleep(60);
    }
    return 0;
}
