/*
 * gtimer.c — a graphical COUNTDOWN timer, a userspace program (M1390).
 *
 * Big MM:SS digits (the kernel 8x16 font scaled 3x) over a ring whose arc
 * shrinks as the time runs out, then sounds the PC speaker (sys_beep) when it
 * hits zero. Keys: SPACE = start/pause, w / Up = +1 min and s / Down = -1 min
 * (while stopped), r = reset, q/Esc = quit. Default 5:00. Colour shows the mode
 * (cyan = set, green = running, amber = paused, red = done). No FPU.
 *
 * Launch: `run gtimer` from the shell, or the Apps menu ("Countdown").
 */
#include "ulib.h"

#define W 240
#define H 212

static unsigned *FB;
static unsigned char FONT[128 * 16];

static int isin(int d) { d %= 360; if (d < 0) d += 360; int s = 1; if (d > 180) { d -= 180; s = -1; }
    long n = 4L * d * (180 - d), de = 40500 - (long)d * (180 - d); return s * (int)(n * 1024 / de); }
static int icos(int d) { return isin(d + 90); }
static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void fill(int x0, int y0, int w, int h, unsigned c) { for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) putpx(x, y, c); }
static void chS(char c, int px, int py, int s, unsigned col) {
    unsigned u = (unsigned char)c; if (u >= 128) u = '?'; const unsigned char *g = &FONT[u * 16];
    for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) fill(px + b * s, py + r * s, s, s, col);
}
/* (textS retired in M1442 — the time now draws via gtextS with a mode-colour bloom) */
static void text(const char *t, int x, int y, unsigned col) { for (int i = 0; t[i]; i++) { chS(t[i], x, y, 1, col); x += 8; } }

/* ---- instrument-panel UI kit (M1442; see gconv.c M1430). gtimer keeps its mode
 * colours (cyan/green/amber/red) on the recessed phosphor readout. ---- */
#define C_FACE_T  0x232D33u
#define C_FACE_B  0x161D21u
#define C_BEZHI   0x3C4A50u
#define C_BEZLO   0x0E1316u
#define C_SCREEN  0x0A0F0Cu
#define C_SCANLN  0x0D140Fu
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

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gtimer: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gtimer: init failed\n"); return 1; }

    long set_ms = 5L * 60000;             /* 5:00 default */
    long rem = set_ms, deadline = 0;
    int running = 0, done = 0, psec = -1, dirty = 1, prevb = 0;
    int typed = -1;                       /* quick-set minutes: digit-entry accumulator (M1456); -1 = idle */

    for (;;) {
        long now = sys_uptime_ms();
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        int mx, my, mb = sys_mouse(&mx, &my);                   /* a click = start/pause, like Space (M1398) */
        int clicked = (mb & 1) && !(prevb & 1) && mx >= 0; prevb = mb;
        if (k == ' ' || clicked) { if (running) { rem = deadline - now; if (rem < 0) rem = 0; running = 0; } else if (rem > 0) { deadline = now + rem; running = 1; done = 0; } dirty = 1; typed = -1; }
        else if ((k == 'w' || k == 0x11) && !running) { set_ms += 60000; if (set_ms > 99L * 60000) set_ms = 99L * 60000; rem = set_ms; done = 0; dirty = 1; }
        else if ((k == 's' || k == 0x12) && !running) { set_ms -= 60000; if (set_ms < 60000) set_ms = 60000; rem = set_ms; done = 0; dirty = 1; }
        else if (k == 'r' || k == 'R') { rem = set_ms; running = 0; done = 0; dirty = 1; }
        else if (k >= '0' && k <= '9' && !running) {            /* type the minutes directly (M1456): "25" -> 25:00 */
            if (typed < 0) typed = 0;
            typed = typed * 10 + (k - '0'); if (typed > 99) typed = 99;
            set_ms = (long)(typed < 1 ? 1 : typed) * 60000; rem = set_ms; done = 0; dirty = 1;
        }
        else if (k == 8 && !running) {                          /* backspace a typed digit */
            if (typed > 0) { typed /= 10; set_ms = (long)(typed < 1 ? 1 : typed) * 60000; rem = set_ms; dirty = 1; }
        }
        if (k > 0 && k != 8 && (k < '0' || k > '9')) typed = -1;   /* any other key ends the typed entry */

        long cur = running ? (deadline - now) : rem;
        if (running && cur <= 0) { cur = rem = 0; running = 0; done = 1; dirty = 1;
            for (int i = 0; i < 3; i++) { sys_beep(1000, 140); sys_sleep(70); } }

        int secs = (int)((cur + 999) / 1000);
        if (secs != psec || dirty) {
            psec = secs;
            int paused = !running && !done && rem > 0 && rem < set_ms;
            unsigned col = done ? 0xF05858 : running ? 0x5CE05C : paused ? 0xE8C040 : 0x50C8E0;
            unsigned glow = (col >> 1) & 0x7F7F7Fu;                 /* a dim halo of the mode colour */
            vgrad(0, 0, W, H, C_FACE_T, C_FACE_B);                  /* slate faceplate */
            bevel_up(0, 0, W, H, C_BEZHI, C_BEZLO);
            panel(8, 12, W - 16, 56);                               /* recessed readout */

            char b[8]; int i = 0; int mm = secs / 60, ss = secs % 60;
            b[i++] = '0' + (mm / 10) % 10; b[i++] = '0' + mm % 10; b[i++] = ':'; b[i++] = '0' + ss / 10; b[i++] = '0' + ss % 10; b[i] = 0;
            gtextS(b, (W - 5 * 24) / 2, 16, 3, col, glow);          /* digits glow in the mode colour */

            int cx = W / 2, cy = 140, R = 44;                       /* ring whose arc shrinks as time runs out */
            for (int a = 0; a < 360; a += 4) putpx(cx + R * isin(a) / 1024, cy - R * icos(a) / 1024, 0x33414Au);
            int len = set_ms > 0 ? (int)((long)cur * 360 / set_ms) : 0;
            for (int a = 0; a < len; a += 4) { int x = cx + R * isin(a) / 1024, y = cy - R * icos(a) / 1024; fill(x - 2, y - 2, 4, 4, col); }

            const char *st = done ? "DONE!    r reset  q quit"
                           : running ? "RUNNING  space pause  r reset"
                           : paused ? "PAUSED   space resume  r reset"
                           : "type min  w/s +-  space go";
            text(st, 8, H - 16, C_DIM);
            sys_gfx_blit(FB);
            dirty = 0;
        }
        sys_sleep(running ? 150 : 120);
    }
    return 0;
}
