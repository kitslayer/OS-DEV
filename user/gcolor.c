/*
 * gcolor.c — a graphical colour picker, a userspace program (M1400).
 *
 * A large preview swatch over three R/G/B gradient sliders, with the hex
 * (#RRGGBB) and rgb(r,g,b) readout. Drag a slider with the mouse (or click a
 * point on it) to set that channel; or pick a channel with r/g/b and nudge it
 * with a/d (or Left/Right). q/Esc quits. No FPU.
 *
 * Launch: `run gcolor` from the shell, or the Apps menu ("Colour Picker").
 */
#include "ulib.h"

#define W 300
#define H 252
#define SX 44                     /* slider track: x in [SX, SX+SW] */
#define SW 226

static unsigned *FB;
static unsigned char FONT[128 * 16];
static const int SY[3] = { 130, 172, 214 };   /* slider y for R, G, B */
static const char CHN[3] = { 'R', 'G', 'B' };

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void fill(int x0, int y0, int w, int h, unsigned c) { for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) putpx(x, y, c); }
static void chS(char c, int px, int py, int s, unsigned col) { unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16]; for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) fill(px + b * s, py + r * s, s, s, col); }
/* (textS retired in M1440 — the hex readout now draws via gtextS with an amber bloom) */
static void text(const char *t, int x, int y, unsigned col) { for (int i = 0; t[i]; i++) { chS(t[i], x, y, 1, col); x += 8; } }
static int clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* ---- instrument-panel UI kit (M1440; see gconv.c M1430) ---- */
#define C_FACE_T  0x232D33u
#define C_FACE_B  0x161D21u
#define C_BEZHI   0x3C4A50u
#define C_BEZLO   0x0E1316u
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
static void gtextS(const char *t, int x, int y, int s, unsigned col, unsigned glow) {
    for (int i = 0; t[i]; i++) chS(t[i], x + i * 8 * s + 1, y + 1, s, glow);
    for (int i = 0; t[i]; i++) chS(t[i], x + i * 8 * s,     y,     s, col);
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gcolor: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gcolor: init failed\n"); return 1; }

    int rgb[3] = { 32, 178, 170 };           /* a pleasant default teal */
    int active = 0, prgb[3] = { -1, -1, -1 }, pact = -1;

    for (;;) {
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        else if (k == 'r') active = 0;
        else if (k == 'g') active = 1;
        else if (k == 'b') active = 2;
        else if (k == 'a' || k == 0x13) rgb[active] = clamp(rgb[active] - 8);   /* left  */
        else if (k == 'd' || k == 0x14) rgb[active] = clamp(rgb[active] + 8);   /* right */
        else if (k == 0x11) active = (active + 2) % 3;                          /* up    */
        else if (k == 0x12) active = (active + 1) % 3;                          /* down  */
        else if (k == 'c' || k == 'C') {                                       /* copy #RRGGBB to the clipboard */
            char hx[8]; const char *HX = "0123456789ABCDEF"; hx[0] = '#';
            for (int ch = 0; ch < 3; ch++) { hx[1 + ch * 2] = HX[(rgb[ch] >> 4) & 15]; hx[2 + ch * 2] = HX[rgb[ch] & 15]; }
            hx[7] = 0; sys_clip_set(hx, 7);
        }

        int mx, my, b = sys_mouse(&mx, &my);                                   /* drag/click a slider */
        if ((b & 1) && mx >= 0) for (int ch = 0; ch < 3; ch++)
            if (my >= SY[ch] - 12 && my <= SY[ch] + 18 && mx >= SX - 6 && mx <= SX + SW + 6) {
                int v = (mx - SX) * 255 / SW; rgb[ch] = clamp(v); active = ch;
            }

        if (rgb[0] != prgb[0] || rgb[1] != prgb[1] || rgb[2] != prgb[2] || active != pact) {
            prgb[0] = rgb[0]; prgb[1] = rgb[1]; prgb[2] = rgb[2]; pact = active;
            unsigned col = ((unsigned)rgb[0] << 16) | ((unsigned)rgb[1] << 8) | rgb[2];
            vgrad(0, 0, W, H, C_FACE_T, C_FACE_B);                             /* slate faceplate */
            bevel_up(0, 0, W, H, C_BEZHI, C_BEZLO);
            fill(13, 13, W - 26, 72, col);                                     /* preview swatch */
            bevel_dn(12, 12, W - 24, 74, C_BEZHI, C_BEZLO);                    /* recessed sample frame */

            char hx[10] = { '#', 0 }; const char *HX = "0123456789ABCDEF";     /* #RRGGBB, amber readout */
            int p = 1; for (int ch = 0; ch < 3; ch++) { hx[p++] = HX[(rgb[ch] >> 4) & 15]; hx[p++] = HX[rgb[ch] & 15]; } hx[p] = 0;
            gtextS(hx, 14, 96, 2, C_AMBER, C_AMBERLO);
            char rs[40]; int q = 0; const char *pre = "rgb(";                  /* rgb(r, g, b) */
            while (*pre) rs[q++] = *pre++;
            for (int ch = 0; ch < 3; ch++) {
                int v = rgb[ch], ti = 0; char t[4];
                if (v == 0) t[ti++] = '0';
                while (v) { t[ti++] = '0' + v % 10; v /= 10; }
                while (ti) rs[q++] = t[--ti];
                if (ch < 2) { rs[q++] = ','; rs[q++] = ' '; }
            }
            rs[q++] = ')'; rs[q] = 0; text(rs, 150, 100, C_DIM);

            for (int ch = 0; ch < 3; ch++) {                                   /* the three sliders */
                int y = SY[ch];
                text((char[]){ CHN[ch], 0 }, 18, y + 2, ch == active ? C_LED : C_DIM);
                for (int i = 0; i < SW; i++) {                                 /* black -> pure-channel gradient */
                    int v = i * 255 / SW; unsigned gc = ch == 0 ? (unsigned)v << 16 : ch == 1 ? (unsigned)v << 8 : (unsigned)v;
                    for (int yy = 0; yy < 14; yy++) putpx(SX + i, y + yy, gc);
                }
                bevel_dn(SX - 1, y - 1, SW + 2, 16, C_BEZHI, C_BEZLO);         /* recess the track */
                int hxp = SX + rgb[ch] * SW / 255;                             /* handle */
                fill(hxp - 2, y - 4, 5, 22, 0xFFFFFF); fill(hxp - 1, y - 3, 3, 20, 0x303840);
                char vt[4], tb[4]; int v = rgb[ch], ti = 0;
                if (v == 0) tb[ti++] = '0';
                while (v) { tb[ti++] = '0' + v % 10; v /= 10; }
                int vp = 0;
                while (ti) vt[vp++] = tb[--ti];
                vt[vp] = 0;
                text(vt, SX + SW + 8, y + 2, C_LABEL);
            }
            text("drag  r/g/b  a/d  c copy hex  q quit", 14, H - 16, C_DIM);
            sys_gfx_blit(FB);
        }
        sys_sleep(40);
    }
    return 0;
}
