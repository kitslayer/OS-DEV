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

/* ---- instrument-panel UI kit (M1430): a slate faceplate with a recessed
 * amber-phosphor readout — a cohesive look shared across the utility apps. ---- */
#define C_FACE_T  0x232D33u     /* faceplate gradient: slate-teal top */
#define C_FACE_B  0x161D21u     /* ... to a darker bottom */
#define C_BEZHI   0x3C4A50u     /* raised bevel highlight */
#define C_BEZLO   0x0E1316u     /* bevel / recess shadow */
#define C_SCREEN  0x0A0F0Cu     /* recessed readout screen */
#define C_SCANLN  0x0D140Fu     /* faint CRT scanline */
#define C_AMBER   0xFFB23Eu     /* phosphor readout */
#define C_AMBERLO 0x7A521Au     /* amber bloom / dim digits */
#define C_LABEL   0xC6D0CCu     /* silkscreen label (soft white) */
#define C_DIM     0x6E827Fu     /* caption / help (dim teal-grey) */
#define C_LED     0x46E0A0u     /* status LED / active (phosphor green) */

static void vgrad(int x, int y, int w, int h, unsigned t, unsigned b) {
    for (int r = 0; r < h; r++) {
        int R = ((int)(t >> 16 & 0xFF) * (h - 1 - r) + (int)(b >> 16 & 0xFF) * r) / (h - 1);
        int G = ((int)(t >> 8  & 0xFF) * (h - 1 - r) + (int)(b >> 8  & 0xFF) * r) / (h - 1);
        int B = ((int)(t       & 0xFF) * (h - 1 - r) + (int)(b       & 0xFF) * r) / (h - 1);
        fill(x, y + r, w, 1, ((unsigned)R << 16) | ((unsigned)G << 8) | (unsigned)B);
    }
}
static void bevel_up(int x, int y, int w, int h, unsigned hi, unsigned lo) {   /* raised: light top-left */
    fill(x, y, w, 1, hi); fill(x, y, 1, h, hi);
    fill(x, y + h - 1, w, 1, lo); fill(x + w - 1, y, 1, h, lo);
}
static void bevel_dn(int x, int y, int w, int h, unsigned hi, unsigned lo) {   /* recessed: dark top-left */
    fill(x, y, w, 1, lo); fill(x, y, 1, h, lo);
    fill(x, y + h - 1, w, 1, hi); fill(x + w - 1, y, 1, h, hi);
}
static void panel(int x, int y, int w, int h) {              /* the recessed phosphor screen */
    fill(x, y, w, h, C_SCREEN);
    for (int r = 3; r < h - 1; r += 3) fill(x + 1, y + r, w - 2, 1, C_SCANLN);
    bevel_dn(x, y, w, h, C_BEZHI, C_BEZLO);
}
static void led(int x, int y) {                              /* a small lit status LED */
    fill(x, y, 9, 9, C_BEZLO); fill(x + 1, y + 1, 7, 7, 0x1A6E50u);
    fill(x + 2, y + 2, 5, 5, C_LED); putpx(x + 3, y + 3, 0xCFFFE8u);
}
static void pill(int x, int y, int w, const char *t, unsigned txt) {   /* a raised mode tab */
    fill(x, y, w, 18, 0x2C383Eu); bevel_up(x, y, w, 18, C_BEZHI, C_BEZLO); text(t, x + 8, y + 1, txt);
}
static void gtextS(const char *t, int x, int y, int s, unsigned col, unsigned glow) {  /* amber text + bloom */
    for (int i = 0; t[i]; i++) chS(t[i], x + i * 8 * s + 1, y + 1, s, glow);
    for (int i = 0; t[i]; i++) chS(t[i], x + i * 8 * s,     y,     s, col);
}

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
static const struct U TIME[] = { {"day", 10000}, {"hr", 240000}, {"min", 14400000}, {"sec", 864000000} };   /* base = day; integer factors (M1453) */
static const struct U *TBL[7] = { LEN, WGT, 0, SPD, ARE, VOL, TIME };               /* index 2 (temperature) is special-cased (affine) */
static const int TBLN[7] = { 5, 4, 0, 4, 4, 4, 4 };
static const char *TEMPU[3] = { "C", "F", "K" };
static const char *CATN[7] = { "Length", "Weight", "Temperature", "Speed", "Area", "Volume", "Time" };

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
        else if (k == 'c' || k == 'C') { cat = (cat + 1) % 7; inunit = 0; dirty = 1; }
        else if (k == 'u' || k == 'U') { inunit = (inunit + 1) % ucount(cat); dirty = 1; }
        else if (k >= '0' && k <= '9') { if (elen < 12) { entry[elen++] = (char)k; entry[elen] = 0; } dirty = 1; }
        else if (k == '.') { int has = 0; for (int i = 0; i < elen; i++) if (entry[i] == '.') has = 1;
                             if (!has && elen < 12) { if (elen == 0) entry[elen++] = '0'; entry[elen++] = '.'; entry[elen] = 0; } dirty = 1; }
        else if (k == 8 || k == 0x7F) { if (elen > 0) entry[--elen] = 0; dirty = 1; }

        int mx, my, b = sys_mouse(&mx, &my);                       /* click = next input unit */
        if ((b & 1) && !(prevb & 1) && mx >= 0) { inunit = (inunit + 1) % ucount(cat); dirty = 1; }   /* (category cycles with c) */
        prevb = b;

        if (dirty) {
            vgrad(0, 0, W, H, C_FACE_T, C_FACE_B);                 /* slate faceplate */
            bevel_up(0, 0, W, H, C_BEZHI, C_BEZLO);
            text("UNIT CONVERTER", 14, 10, C_LABEL);               /* silkscreen title */
            fill(14, 27, 116, 2, C_AMBERLO);                       /* amber title rule */
            led(W - 24, 11);                                       /* power LED */
            { int pw = 0; const char *cn = CATN[cat]; while (cn[pw]) pw++; pill(14, 38, pw * 8 + 16, cn, C_LED); }

            long v = elen ? parse_fixed(entry) : 0;
            char vb[24]; int q = 0; for (int i = 0; i < elen; i++) vb[q++] = entry[i]; if (elen == 0) vb[q++] = '0';
            vb[q++] = ' '; const char *iu = uname(cat, inunit); for (int j = 0; iu[j]; j++) vb[q++] = iu[j]; vb[q] = 0;
            text("INPUT", 210, 42, C_DIM);
            textS(vb, 14, 62, 2, C_LABEL);                         /* the value you set, in white */

            int sx = 12, sy = 94, sw = W - 24, sh = H - 94 - 24;   /* recessed amber readout */
            panel(sx, sy, sw, sh);
            long base = cat == 2 ? toC(inunit, v) : 0;             /* for temp, value in Celsius */
            int y = sy + 14, n = ucount(cat);
            for (int j = 0; j < n; j++) {
                if (j == inunit) continue;
                long out = cat == 2 ? fromC(j, base) : v * TBL[cat][j].factor / TBL[cat][inunit].factor;
                char r[24]; fmt_fixed(out, r);
                char line[40]; int p = 0; line[p++] = '='; line[p++] = ' ';
                for (int z = 0; r[z]; z++) line[p++] = r[z];
                line[p++] = ' '; const char *un = uname(cat, j); for (int z = 0; un[z]; z++) line[p++] = un[z]; line[p] = 0;
                gtextS(line, sx + 12, y, 1, C_AMBER, C_AMBERLO); y += 20;
            }
            text("c category    u input unit    q quit", 14, H - 15, C_DIM);
            sys_gfx_blit(FB);
            dirty = 0;
        }
        sys_sleep(50);
    }
    return 0;
}
