/*
 * gconv.c — a unit converter, a userspace program (M1407).
 *
 * Type a value in the category's base unit (metres / kilograms / Celsius) and it
 * shows the equivalents in the other units of that category, live. c cycles the
 * category (Length / Weight / Temperature), digits and . type the value,
 * Backspace deletes, q/Esc quits. All maths is INTEGER fixed-point x10000 (exact,
 * no FPU); temperature uses the affine formulae.
 *
 * Launch: `run gconv` from the shell, or the Apps menu ("Unit Convert").
 */
#include "ulib.h"

#define W 340
#define H 250
#define SCALE 10000L

static unsigned *FB;
static unsigned char FONT[128 * 16];

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void fill(int x0, int y0, int w, int h, unsigned c) { for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) putpx(x, y, c); }
static void chS(char c, int px, int py, int s, unsigned col) { unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16]; for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) fill(px + b * s, py + r * s, s, s, col); }
static void textS(const char *t, int x, int y, int s, unsigned col) { for (int i = 0; t[i]; i++) { chS(t[i], x, y, s, col); x += 8 * s; } }
static void text(const char *t, int x, int y, unsigned col) { for (int i = 0; t[i]; i++) { chS(t[i], x, y, 1, col); x += 8; } }

static long parse_fixed(const char *s) {
    long ip = 0; int i = 0, neg = 0;
    if (s[0] == '-') { neg = 1; i = 1; }
    while (s[i] >= '0' && s[i] <= '9') { ip = ip * 10 + (s[i] - '0'); i++; }
    long frac = 0, scale = 1000;
    if (s[i] == '.') { i++; while (s[i] >= '0' && s[i] <= '9' && scale > 0) { frac += (long)(s[i] - '0') * scale; scale /= 10; i++; } }
    long v = ip * SCALE + frac;
    return neg ? -v : v;
}
static void fmt_fixed(long v, char *b) {
    int p = 0; if (v < 0) { b[p++] = '-'; v = -v; }
    long ip = v / SCALE, frac = v % SCALE;
    char t[20]; int ti = 0; long q = ip; if (q == 0) t[ti++] = '0'; while (q) { t[ti++] = '0' + q % 10; q /= 10; }
    while (ti) b[p++] = t[--ti];
    if (frac) {
        b[p++] = '.';
        char f[4] = { (char)('0' + (frac / 1000) % 10), (char)('0' + (frac / 100) % 10), (char)('0' + (frac / 10) % 10), (char)('0' + frac % 10) };
        int fl = 4; while (fl > 0 && f[fl - 1] == '0') fl--;
        for (int i = 0; i < fl; i++) b[p++] = f[i];
    }
    b[p] = 0;
}

/* per-category unit names + multiplicative factor from the base (fixed x10000); temperature is special-cased */
struct U { const char *name; long factor; };
static const struct U LEN[] = { {"ft", 32808}, {"in", 393701}, {"cm", 1000000}, {"km", 10} };  /* from metres (4-dec fixed-point; miles too small to represent) */
static const struct U WGT[] = { {"lb", 22046}, {"g", 10000000}, {"oz", 352740} };              /* from kilograms */
static const struct U SPD[] = { {"km/h", 36000}, {"mph", 22369}, {"ft/s", 32808} };            /* from metres/second */
static const struct U ARE[] = { {"sqft", 107639}, {"sqin", 15500031}, {"sqcm", 100000000} };   /* from square metres */
static const struct U *TBL[5] = { LEN, WGT, 0, SPD, ARE };                                      /* index 2 (temperature) is special-cased */
static const int TBLN[5] = { 4, 3, 0, 3, 3 };
static const char *CATN[5] = { "Length  (metres)", "Weight  (kilograms)", "Temperature  (Celsius)", "Speed  (metres/sec)", "Area  (square metres)" };

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gconv: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gconv: init failed\n"); return 1; }

    int cat = 0, elen = 0, dirty = 1, prevb = 0;
    char entry[16] = { 0 };

    for (;;) {
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        else if (k == 'c' || k == 'C') { cat = (cat + 1) % 5; dirty = 1; }
        else if (k >= '0' && k <= '9') { if (elen < 12) { entry[elen++] = (char)k; entry[elen] = 0; } dirty = 1; }
        else if (k == '.') { int has = 0; for (int i = 0; i < elen; i++) if (entry[i] == '.') has = 1;
                             if (!has && elen < 12) { if (elen == 0) entry[elen++] = '0'; entry[elen++] = '.'; entry[elen] = 0; } dirty = 1; }
        else if (k == 8 || k == 0x7F) { if (elen > 0) entry[--elen] = 0; dirty = 1; }

        int mx, my, b = sys_mouse(&mx, &my);                       /* click anywhere = next category */
        if ((b & 1) && !(prevb & 1) && mx >= 0) { cat = (cat + 1) % 5; dirty = 1; }
        prevb = b;

        if (dirty) {
            for (int i = 0; i < W * H; i++) FB[i] = 0x14171F;
            text("Unit Converter", 12, 8, 0x8FD0FF);
            text(CATN[cat], 12, 30, 0xF0D060);
            for (int x = 8; x < W - 8; x++) putpx(x, 50, 0x2A3040);

            long v = elen ? parse_fixed(entry) : 0;
            char vb[24]; for (int i = 0; i <= elen; i++) vb[i] = entry[i]; if (elen == 0) { vb[0] = '0'; vb[1] = 0; }
            textS(vb, 16, 62, 2, 0x6CF09A);                        /* the value being typed */

            int y = 110;
            char line[40];
            if (cat == 2) {                                        /* Temperature: affine */
                long f = v * 18000 / SCALE + 320000, kel = v + 2731500;
                char r[24]; int p = 0; fmt_fixed(f, r); const char *u = " F";
                p = 0; line[p++] = '='; line[p++] = ' '; for (int j = 0; r[j]; j++) line[p++] = r[j]; for (int j = 0; u[j]; j++) line[p++] = u[j]; line[p] = 0;
                text(line, 24, y, 0xD8E0EC); y += 24;
                fmt_fixed(kel, r); u = " K";
                p = 0; line[p++] = '='; line[p++] = ' '; for (int j = 0; r[j]; j++) line[p++] = r[j]; for (int j = 0; u[j]; j++) line[p++] = u[j]; line[p] = 0;
                text(line, 24, y, 0xD8E0EC); y += 24;
            } else {
                const struct U *tbl = TBL[cat];
                int n = TBLN[cat];
                for (int i = 0; i < n; i++) {
                    long out = v * tbl[i].factor / SCALE;
                    char r[24]; fmt_fixed(out, r);
                    int p = 0; line[p++] = '='; line[p++] = ' ';
                    for (int j = 0; r[j]; j++) line[p++] = r[j];
                    line[p++] = ' '; for (int j = 0; tbl[i].name[j]; j++) line[p++] = tbl[i].name[j]; line[p] = 0;
                    text(line, 24, y, 0xD8E0EC); y += 24;
                }
            }
            text("type a value   c: category   q: quit", 12, H - 16, 0x707888);
            sys_gfx_blit(FB);
            dirty = 0;
        }
        sys_sleep(50);
    }
    return 0;
}
