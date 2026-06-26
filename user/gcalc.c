/*
 * gcalc.c — a graphical calculator, a userspace program (M1394, clickable M1396).
 *
 * A four-function calculator with an LCD-style display over a colour-coded
 * keypad. Driven by EITHER the keyboard OR the mouse (click the buttons, like
 * paint.c reads sys_mouse); the last key pressed is highlighted. Immediate-
 * execution model. All maths is INTEGER fixed-point scaled by 10000 (four exact
 * decimals, no FPU, no float rounding). Keys: 0-9 . digits, + - * / (and x)
 * operators, = or Enter evaluates, c/C clears, Backspace deletes, q/Esc quits.
 *
 * Launch: `run gcalc` from the shell, or the Apps menu ("Calculator").
 */
#include "ulib.h"

#define W 232
#define H 340
#define SCALE 10000L              /* fixed-point: value * 10000 */
#define BX 8                      /* keypad geometry (shared by the renderer + the click hit-test) */
#define BY 70
#define BW 52
#define BH 44
#define GX 56
#define GY 48

static unsigned *FB;
static unsigned char FONT[128 * 16];

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void fill(int x0, int y0, int w, int h, unsigned c) { for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) putpx(x, y, c); }
static void chS(char c, int px, int py, int s, unsigned col) { unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16]; for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) fill(px + b * s, py + r * s, s, s, col); }
static void textS(const char *t, int x, int y, int s, unsigned col) { for (int i = 0; t[i]; i++) { chS(t[i], x, y, s, col); x += 8 * s; } }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* ---- instrument-panel UI kit (M1439; see gconv.c M1430) ---- */
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
static void gtextS(const char *t, int x, int y, int s, unsigned col, unsigned glow) {
    for (int i = 0; t[i]; i++) chS(t[i], x + i * 8 * s + 1, y + 1, s, glow);
    for (int i = 0; t[i]; i++) chS(t[i], x + i * 8 * s,     y,     s, col);
}

/* parse a typed entry string ("31.4") into fixed-point (*10000) */
static long parse_fixed(const char *s) {
    long ip = 0; int i = 0, neg = 0;
    if (s[0] == '-') { neg = 1; i = 1; }
    while (s[i] >= '0' && s[i] <= '9') { ip = ip * 10 + (s[i] - '0'); i++; }
    long frac = 0, scale = 1000;
    if (s[i] == '.') { i++; while (s[i] >= '0' && s[i] <= '9' && scale > 0) { frac += (long)(s[i] - '0') * scale; scale /= 10; i++; } }
    long v = ip * SCALE + frac;
    return neg ? -v : v;
}
/* format fixed-point into a string, trimming trailing zeros (exact, no float) */
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
static long apply(long a, char op, long b) {
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b / SCALE;
        case '/': return b ? a * SCALE / b : 0;
    }
    return b;
}

static const char *PAD[5][4] = {
    { "7", "8", "9", "/" },
    { "4", "5", "6", "*" },
    { "1", "2", "3", "-" },
    { "0", ".", "=", "+" },
    { "C", "<", "%", 0   },
};

/* calculator state (file-scope so both the keyboard and the mouse can press()) */
static long reg; static char pend, last; static char entry[16]; static int elen, fresh = 1, dirty = 1;

/* apply one keypress (a digit, '.', an operator, '=', 'c'/'C', or 8=backspace) */
static void press(int k) {
    if (k >= '0' && k <= '9') { if (fresh) { elen = 0; entry[0] = 0; fresh = 0; } if (elen < 12) { entry[elen++] = (char)k; entry[elen] = 0; } last = (char)k; dirty = 1; }
    else if (k == '.') { if (fresh) { elen = 0; entry[0] = 0; fresh = 0; } int has = 0; for (int i = 0; i < elen; i++) if (entry[i] == '.') has = 1;
                         if (!has && elen < 12) { if (elen == 0) entry[elen++] = '0'; entry[elen++] = '.'; entry[elen] = 0; } last = '.'; dirty = 1; }
    else if (k == '+' || k == '-' || k == '*' || k == '/' || k == 'x' || k == 'X') {
        char op = (k == 'x' || k == 'X') ? '*' : (char)k;
        long v = elen ? parse_fixed(entry) : reg;
        reg = pend ? apply(reg, pend, v) : v;
        pend = op; elen = 0; entry[0] = 0; fresh = 1; last = op; dirty = 1;
    }
    else if (k == '=' || k == '\n' || k == '\r') {
        long v = elen ? parse_fixed(entry) : reg;
        reg = pend ? apply(reg, pend, v) : v;
        pend = 0; elen = 0; entry[0] = 0; fresh = 1; last = '='; dirty = 1;
    }
    else if (k == '%') { long v = elen ? parse_fixed(entry) : reg; reg = v / 100; pend = 0; elen = 0; entry[0] = 0; fresh = 1; last = '%'; dirty = 1; }   /* x% = x/100 */
    else if (k == 'c' || k == 'C') { reg = 0; pend = 0; elen = 0; entry[0] = 0; fresh = 1; last = 'C'; dirty = 1; }
    else if (k == 'y' || k == 'Y') { char s[24]; if (elen > 0 && !fresh) { int i = 0; for (; entry[i]; i++) s[i] = entry[i]; s[i] = 0; } else fmt_fixed(reg, s); sys_clip_set(s, slen(s)); last = 'y'; dirty = 1; }   /* copy the display to the clipboard */
    else if (k == 8 || k == 0x7F) { if (elen > 0) { entry[--elen] = 0; fresh = 0; } last = '<'; dirty = 1; }
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gcalc: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gcalc: init failed\n"); return 1; }

    int prevb = 0;
    for (;;) {
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        if (k > 0) press(k);

        int mx, my, b = sys_mouse(&mx, &my);                    /* clickable keypad (M1396) */
        if ((b & 1) && !(prevb & 1) && mx >= 0) {               /* left-click edge inside the canvas */
            for (int r = 0; r < 5; r++) for (int c = 0; c < 4; c++) {
                const char *lab = PAD[r][c]; if (!lab) continue;
                int x = BX + c * GX, y = BY + r * GY;
                if (mx >= x && mx < x + BW && my >= y && my < y + BH) press(lab[0] == '<' ? 8 : lab[0]);
            }
        }
        prevb = b;

        if (dirty) {
            vgrad(0, 0, W, H, C_FACE_T, C_FACE_B);              /* slate faceplate */
            bevel_up(0, 0, W, H, C_BEZHI, C_BEZLO);
            panel(8, 8, W - 16, 52);                            /* recessed amber LCD */
            if (pend) chS(pend, 14, 14, 1, C_AMBERLO);          /* pending-operator indicator (M1414) */
            char shown[24]; if (elen > 0 && !fresh) { for (int i = 0; i <= elen; i++) shown[i] = entry[i]; } else fmt_fixed(reg, shown);
            int sl = slen(shown); if (sl > 9) sl = 9;           /* right-align, clip to 9 glyphs */
            int rx = W - 12 - sl * 24; if (rx < 12) rx = 12;
            gtextS(shown + (slen(shown) > 9 ? slen(shown) - 9 : 0), rx, 24, 3, C_AMBER, C_AMBERLO);   /* result glows amber */

            for (int r = 0; r < 5; r++) for (int c = 0; c < 4; c++) {
                const char *lab = PAD[r][c]; if (!lab) continue;
                int x = BX + c * GX, y = BY + r * GY;
                int hot = (last && lab[0] == last);             /* highlight the last key pressed */
                int isop = (lab[0]=='/'||lab[0]=='*'||lab[0]=='-'||lab[0]=='+'||lab[0]=='='||lab[0]=='%');
                int isclr = (lab[0]=='C'||lab[0]=='<');
                unsigned bg = hot ? 0x2A6E50u : isop ? 0x3A2E1Au : isclr ? 0x3A2226u : 0x2C383Eu;   /* hot=green, op=amber-dark, clear=red-dark, digit=slate */
                fill(x, y, BW, BH, bg);
                bevel_up(x, y, BW, BH, C_BEZHI, C_BEZLO);       /* raised key */
                unsigned tc = hot ? 0xEFFFF4u : isop ? C_AMBER : isclr ? 0xF0A6A6u : C_LABEL;
                textS(lab, x + (BW - 16) / 2, y + (BH - 16) / 2, 2, tc);
            }
            textS("y copy result", 8, H - 16, 1, C_DIM);
            sys_gfx_blit(FB);
            dirty = 0;
        }
        sys_sleep(40);
    }
    return 0;
}
