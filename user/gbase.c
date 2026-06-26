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
#define H 224

static unsigned *FB;
static unsigned char FONT[128 * 16];

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void fill(int x0, int y0, int w, int h, unsigned c) { for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) putpx(x, y, c); }
static void chS(char c, int px, int py, int s, unsigned col) { unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16]; for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) fill(px + b * s, py + r * s, s, s, col); }
static void textS(const char *t, int x, int y, int s, unsigned col) { for (int i = 0; t[i]; i++) { chS(t[i], x, y, s, col); x += 8 * s; } }
static void text(const char *t, int x, int y, unsigned col) { for (int i = 0; t[i]; i++) { chS(t[i], x, y, 1, col); x += 8; } }

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
            for (int i = 0; i < W * H; i++) FB[i] = 0x121620;
            text("Base Converter", 12, 8, 0x8FD0FF);
            for (int x = 8; x < W - 8; x++) putpx(x, 30, 0x2A3040);

            unsigned long v = 0; for (int i = 0; i < elen; i++) v = v * (unsigned)ibase + digval(entry[i]);
            textS(elen ? entry : "0", 16, 40, 3, 0x6CF09A);       /* the value as typed, large */
            const char *ibn = ibase == 10 ? "dec input" : ibase == 16 ? "hex input" : "bin input";
            text(ibn, 16 + (elen ? elen : 1) * 24 + 16, 58, 0x808A9A);

            char dc[24], hx[68], bn[68], oc[68];
            tobase(v, 10, dc); tobase(v, 16, hx); tobase(v, 2, bn); tobase(v, 8, oc);
            text("DEC", 16, 102, 0xF0C060);                 text(dc, 80, 102, 0xE0E8F4);
            text("HEX", 16, 126, 0xF0C060); text("0x", 64, 126, 0x808A9A); text(hx, 80, 126, 0xE0E8F4);
            text("OCT", 16, 150, 0xF0C060); text("0o", 64, 150, 0x808A9A); text(oc, 80, 150, 0xE0E8F4);
            text("BIN", 16, 174, 0xF0C060); text("0b", 64, 174, 0x808A9A); text(bn, 80, 174, 0xE0E8F4);

            text("type a number   i: input base   q: quit", 12, H - 16, 0x707888);
            sys_gfx_blit(FB);
            dirty = 0;
        }
        sys_sleep(50);
    }
    return 0;
}
