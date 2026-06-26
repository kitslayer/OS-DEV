/*
 * gconv.c — a unit converter, a userspace program (M1407; from-any-unit M1410).
 *
 * Pick a category (c) and an input unit (u), type a value, and it shows the
 * equivalents in every other unit of that category, live. Categories: Length,
 * Weight, Temperature, Speed, Area. All maths is INTEGER fixed-point x10000
 * (exact, no FPU): multiplicative units convert as out = V * factor_j / factor_i
 * with the base treated as a unit (factor = 1.0000); temperature uses the affine
 * formulae through Celsius. q/Esc quits.
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

struct U { const char *name; long factor; };                                       /* factor = units per base, x10000; base is the unit with factor 10000 */
static const struct U LEN[] = { {"m", 10000}, {"ft", 32808}, {"in", 393701}, {"cm", 1000000}, {"km", 10} };
static const struct U WGT[] = { {"kg", 10000}, {"lb", 22046}, {"g", 10000000}, {"oz", 352740} };
static const struct U SPD[] = { {"m/s", 10000}, {"km/h", 36000}, {"mph", 22369}, {"ft/s", 32808} };
static const struct U ARE[] = { {"sqm", 10000}, {"sqft", 107639}, {"sqin", 15500031}, {"sqcm", 100000000} };
static const struct U VOL[] = { {"L", 10000}, {"mL", 10000000}, {"gal", 2642}, {"cup", 42268} };   /* from litres */
static const struct U *TBL[6] = { LEN, WGT, 0, SPD, ARE, VOL };                     /* index 2 (temperature) is special-cased (affine) */
static const int TBLN[6] = { 5, 4, 0, 4, 4, 4 };
static const char *TEMPU[3] = { "C", "F", "K" };
static const char *CATN[6] = { "Length", "Weight", "Temperature", "Speed", "Area", "Volume" };

static int ucount(int cat) { return cat == 2 ? 3 : TBLN[cat]; }
static const char *uname(int cat, int u) { return cat == 2 ? TEMPU[u] : TBL[cat][u].name; }
static long toC(int u, long v) { return u == 0 ? v : u == 1 ? (v - 320000) * 5 / 9 : v - 2731500; }   /* temp unit -> Celsius (x10000) */
static long fromC(int u, long c) { return u == 0 ? c : u == 1 ? c * 9 / 5 + 320000 : c + 2731500; }   /* Celsius -> temp unit */

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gconv: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gconv: init failed\n"); return 1; }

    int cat = 0, inunit = 0, elen = 0, dirty = 1, prevb = 0;
    char entry[16] = { 0 };

    for (;;) {
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        else if (k == 'c' || k == 'C') { cat = (cat + 1) % 6; inunit = 0; dirty = 1; }
        else if (k == 'u' || k == 'U') { inunit = (inunit + 1) % ucount(cat); dirty = 1; }
        else if (k >= '0' && k <= '9') { if (elen < 12) { entry[elen++] = (char)k; entry[elen] = 0; } dirty = 1; }
        else if (k == '.') { int has = 0; for (int i = 0; i < elen; i++) if (entry[i] == '.') has = 1;
                             if (!has && elen < 12) { if (elen == 0) entry[elen++] = '0'; entry[elen++] = '.'; entry[elen] = 0; } dirty = 1; }
        else if (k == 8 || k == 0x7F) { if (elen > 0) entry[--elen] = 0; dirty = 1; }

        int mx, my, b = sys_mouse(&mx, &my);                       /* click = next input unit */
        if ((b & 1) && !(prevb & 1) && mx >= 0) { inunit = (inunit + 1) % ucount(cat); dirty = 1; }   /* (category cycles with c) */
        prevb = b;

        if (dirty) {
            for (int i = 0; i < W * H; i++) FB[i] = 0x14171F;
            text("Unit Converter", 12, 8, 0x8FD0FF);
            text(CATN[cat], 12, 30, 0xF0D060);
            for (int x = 8; x < W - 8; x++) putpx(x, 50, 0x2A3040);

            long v = elen ? parse_fixed(entry) : 0;
            char vb[24]; int q = 0; for (int i = 0; i < elen; i++) vb[q++] = entry[i]; if (elen == 0) vb[q++] = '0';
            vb[q++] = ' '; const char *iu = uname(cat, inunit); for (int j = 0; iu[j]; j++) vb[q++] = iu[j]; vb[q] = 0;
            textS(vb, 16, 62, 2, 0x6CF09A);                        /* value + input unit */

            long base = cat == 2 ? toC(inunit, v) : 0;             /* for temp, value in Celsius */
            int y = 110, n = ucount(cat);
            for (int j = 0; j < n; j++) {
                if (j == inunit) continue;
                long out = cat == 2 ? fromC(j, base) : v * TBL[cat][j].factor / TBL[cat][inunit].factor;
                char r[24]; fmt_fixed(out, r);
                char line[40]; int p = 0; line[p++] = '='; line[p++] = ' ';
                for (int z = 0; r[z]; z++) line[p++] = r[z];
                line[p++] = ' '; const char *un = uname(cat, j); for (int z = 0; un[z]; z++) line[p++] = un[z]; line[p] = 0;
                text(line, 24, y, 0xD8E0EC); y += 22;
            }
            text("c: category   u: input unit   q: quit", 12, H - 16, 0x707888);
            sys_gfx_blit(FB);
            dirty = 0;
        }
        sys_sleep(50);
    }
    return 0;
}
