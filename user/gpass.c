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
/* (box helper retired in M1431 — the password now sits in a recessed panel) */
static void chS(char c, int px, int py, int s, unsigned col) { unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16]; for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) fill(px + b * s, py + r * s, s, s, col); }
/* (textS retired in M1431 — the password now draws via gtextS with an amber bloom) */
static void text(const char *t, int x, int y, unsigned col) { for (int i = 0; t[i]; i++) { chS(t[i], x, y, 1, col); x += 8; } }

/* ---- instrument-panel UI kit (M1431; see gconv.c M1430) ---- */
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
static void gtextS(const char *t, int x, int y, int s, unsigned col, unsigned glow) {
    for (int i = 0; t[i]; i++) chS(t[i], x + i * 8 * s + 1, y + 1, s, glow);
    for (int i = 0; t[i]; i++) chS(t[i], x + i * 8 * s,     y,     s, col);
}

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
            vgrad(0, 0, W, H, C_FACE_T, C_FACE_B);                 /* slate faceplate */
            bevel_up(0, 0, W, H, C_BEZHI, C_BEZLO);
            text("PASSWORD GENERATOR", 14, 10, C_LABEL);
            fill(14, 27, 148, 2, C_AMBERLO);
            led(W - 24, 11);

            int sx = 12, sy = 38, sw = W - 24, sh = 60;            /* recessed amber readout */
            panel(sx, sy, sw, sh);
            int s = len <= 24 ? 2 : 1, tw = len * 8 * s;
            int px = sx + (sw - tw) / 2; if (px < sx + 8) px = sx + 8;
            gtextS(pw, px, sy + (sh - 16 * s) / 2, s, C_AMBER, C_AMBERLO);   /* the password, glowing */

            char ln[24]; int p = 0; const char *a = "LENGTH "; while (*a) ln[p++] = *a++;
            int v = len; char t[4]; int ti = 0; if (v == 0) t[ti++] = '0'; while (v) { t[ti++] = '0' + v % 10; v /= 10; }
            while (ti) ln[p++] = t[--ti]; ln[p] = 0;
            text(ln, 14, sy + sh + 12, C_LABEL);
            if (copied) text("COPIED", W - 78, sy + sh + 12, C_LED);

            text("space new    w/s length    c copy    q quit", 14, H - 14, C_DIM);
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
