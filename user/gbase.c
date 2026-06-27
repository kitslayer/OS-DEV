/*
 * gbase.c — a number-base converter, a userspace program (M1411; any-base in M1413).
 *
 * Type an integer in the chosen input base and it shows it live in decimal, hex,
 * octal and binary. `i` cycles the input base (dec -> hex -> bin); the keypad
 * accepts only digits valid for that base (hex takes a-f). Backspace deletes,
 * q/Esc quits. Pure integer maths.
 *
 * Launch: `run gbase` from the shell, or the Apps menu ("Base Convert").
 */
#include "ulib.h"

#define W 360
#define H 248                 /* taller: room for the CHR (ASCII) row (M1454) */

static unsigned *FB;
static unsigned char FONT[128 * 16];

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void fill(int x0, int y0, int w, int h, unsigned c) { for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) putpx(x, y, c); }
static void chS(char c, int px, int py, int s, unsigned col) { unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16]; for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) fill(px + b * s, py + r * s, s, s, col); }
static void textS(const char *t, int x, int y, int s, unsigned col) { for (int i = 0; t[i]; i++) { chS(t[i], x, y, s, col); x += 8 * s; } }
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
static void pill(int x, int y, int w, const char *t, unsigned txt) {
    fill(x, y, w, 18, 0x2C383Eu); bevel_up(x, y, w, 18, C_BEZHI, C_BEZLO); text(t, x + 8, y + 1, txt);
}
static void gtextS(const char *t, int x, int y, int s, unsigned col, unsigned glow) {
    for (int i = 0; t[i]; i++) chS(t[i], x + i * 8 * s + 1, y + 1, s, glow);
    for (int i = 0; t[i]; i++) chS(t[i], x + i * 8 * s,     y,     s, col);
}

static int digval(int c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; }
/* unsigned long -> string in `base` (2..16), no leading zeros */
static void tobase(unsigned long v, int base, char *out) {
    static const char *D = "0123456789ABCDEF";
    char t[68]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = D[v % base]; v /= base; }
    int p = 0; while (n) out[p++] = t[--n];
    out[p] = 0;
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gbase: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gbase: init failed\n"); return 1; }

    char entry[18] = { 0 }; int elen = 0, dirty = 1, ibase = 10;
    for (;;) {
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        else if (k == 'i' || k == 'I') { ibase = ibase == 10 ? 16 : ibase == 16 ? 2 : 10; elen = 0; entry[0] = 0; dirty = 1; }
        else if (k == 8 || k == 0x7F) { if (elen > 0) entry[--elen] = 0; dirty = 1; }
        else { int dv = digval(k); if (dv >= 0 && dv < ibase && elen < 16) { entry[elen++] = (char)k; entry[elen] = 0; dirty = 1; } }

        if (dirty) {
            vgrad(0, 0, W, H, C_FACE_T, C_FACE_B);                 /* slate faceplate */
            bevel_up(0, 0, W, H, C_BEZHI, C_BEZLO);
            text("BASE CONVERTER", 14, 10, C_LABEL);
            fill(14, 27, 116, 2, C_AMBERLO);
            led(W - 24, 11);
            const char *ibn = ibase == 10 ? "DEC INPUT" : ibase == 16 ? "HEX INPUT" : "BIN INPUT";
            pill(14, 38, 9 * 8 + 16, ibn, C_LED);

            unsigned long v = 0; for (int i = 0; i < elen; i++) v = v * (unsigned)ibase + digval(entry[i]);
            textS(elen ? entry : "0", 14, 60, 2, C_LABEL);         /* the value you typed, white */

            char dc[24], hx[68], bn[68], oc[68];
            tobase(v, 10, dc); tobase(v, 16, hx); tobase(v, 2, bn); tobase(v, 8, oc);
            int sx = 12, sy = 88, sw = W - 24, sh = H - 88 - 22;   /* recessed amber readout */
            panel(sx, sy, sw, sh);
            int lx = sx + 12, vx = sx + 76, y = sy + 14;
            text("DEC", lx, y, C_AMBERLO);                                gtextS(dc, vx, y, 1, C_AMBER, C_AMBERLO); y += 24;
            text("HEX", lx, y, C_AMBERLO); text("0x", vx - 16, y, C_AMBERLO); gtextS(hx, vx, y, 1, C_AMBER, C_AMBERLO); y += 24;
            text("OCT", lx, y, C_AMBERLO); text("0o", vx - 16, y, C_AMBERLO); gtextS(oc, vx, y, 1, C_AMBER, C_AMBERLO); y += 24;
            text("BIN", lx, y, C_AMBERLO); text("0b", vx - 16, y, C_AMBERLO); gtextS(bn, vx, y, 1, C_AMBER, C_AMBERLO); y += 24;
            text("CHR", lx, y, C_AMBERLO);                         /* ASCII char for a printable byte (M1454) */
            if (v >= 32 && v <= 126) { char cs[4] = { '\'', (char)v, '\'', 0 }; gtextS(cs, vx, y, 1, C_AMBER, C_AMBERLO); }
            else text("-", vx, y, C_AMBERLO);

            text("type a number    i input base    q quit", 14, H - 14, C_DIM);
            sys_gfx_blit(FB);
            dirty = 0;
        }
        sys_sleep(50);
    }
    return 0;
}
