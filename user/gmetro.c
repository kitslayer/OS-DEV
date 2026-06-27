/*
 * gmetro.c — a metronome, a userspace program (M1406).
 *
 * A swinging pendulum keeps the beat while the PC speaker ticks (sys_beep) once
 * per beat; the bob flashes on each tick. SPACE (or a click) starts/stops, w/Up
 * and s/Down change the tempo by 5 BPM (40..240), 1-9 set the time signature
 * (beats per bar, with an accented downbeat), q/Esc quits. Pendulum angle is
 * a Bhaskara sine of the beat phase — no FPU.
 *
 * Launch: `run gmetro` from the shell, or the Apps menu ("Metronome").
 */
#include "ulib.h"

#define W 280
#define H 240
#define CX 140
#define PY 46                     /* pivot y */
#define L  126                    /* rod length */
#define SWING 42                  /* max swing from vertical, degrees */

static unsigned *FB;
static unsigned char FONT[128 * 16];

static int isin(int d) { d %= 360; if (d < 0) d += 360; int s = 1; if (d > 180) { d -= 180; s = -1; }
    long n = 4L * d * (180 - d), de = 40500 - (long)d * (180 - d); return s * (int)(n * 1024 / de); }
static int icos(int d) { return isin(d + 90); }
static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void fill(int x0, int y0, int w, int h, unsigned c) { for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) putpx(x, y, c); }
static void line(int x0, int y0, int x1, int y1, unsigned c) {
    int dx = x1 - x0, dy = y1 - y0, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1, e = (ax > ay ? ax : -ay) / 2, e2;
    for (;;) { putpx(x0, y0, c); putpx(x0 + 1, y0, c); if (x0 == x1 && y0 == y1) break; e2 = e; if (e2 > -ax) { e -= ay; x0 += sx; } if (e2 < ay) { e += ax; y0 += sy; } }
}
static void disc(int cx, int cy, int r, unsigned c) { for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) if (dx * dx + dy * dy <= r * r) putpx(cx + dx, cy + dy, c); }
static void chS(char c, int px, int py, int s, unsigned col) { unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16]; for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) fill(px + b * s, py + r * s, s, s, col); }
static void textS(const char *t, int x, int y, int s, unsigned col) { for (int i = 0; t[i]; i++) { chS(t[i], x, y, s, col); x += 8 * s; } }
static void text(const char *t, int x, int y, unsigned col) { for (int i = 0; t[i]; i++) { chS(t[i], x, y, 1, col); x += 8; } }

/* ---- instrument-panel UI kit (M1446; see gconv.c M1430). Minimal touch: the metronome
 * keeps its pendulum (its identity) but joins the family — slate gradient bg, a window
 * bevel, and an amber BPM readout. ---- */
#define C_FACE_T  0x232D33u
#define C_FACE_B  0x161D21u
#define C_BEZHI   0x3C4A50u
#define C_BEZLO   0x0E1316u
#define C_AMBER   0xFFB23Eu
#define C_AMBERLO 0x7A521Au
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

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gmetro: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gmetro: init failed\n"); return 1; }

    int bpm = 120, running = 1, flash = 0, prevb = 0, sig = 4, accent = 0;   /* sig = beats per bar (M1459) */
    long t0 = sys_uptime_ms(); int pbeat = -1;

    for (;;) {
        long now = sys_uptime_ms();
        int interval = 60000 / bpm;
        if (running) {
            int beat = (int)((now - t0) / interval);
            if (beat != pbeat) { pbeat = beat; flash = 6; accent = (beat % sig == 0); sys_beep(accent ? 1760 : 1047, accent ? 22 : 14); }   /* accented downbeat */
        }

        vgrad(0, 0, W, H, C_FACE_T, C_FACE_B);               /* slate faceplate (M1446) */
        int phase = running ? (int)(((now - t0) % (2L * interval)) * 360 / (2 * interval)) : 0;
        int ang = SWING * isin(phase) / 1024;                /* pendulum angle from vertical */
        int bx = CX + L * isin(ang) / 1024, by = PY + L * icos(ang) / 1024;
        for (int a = -SWING; a <= SWING; a += 6)             /* faint swing arc guide */
            putpx(CX + (L + 8) * isin(a) / 1024, PY + (L + 8) * icos(a) / 1024, 0x222838);
        line(CX, PY, bx, by, 0xB0B8C8);                      /* rod */
        disc(bx, by, 11, flash > 0 ? (accent ? 0xFFFFC8 : 0xFFE060) : 0xE05858);   /* bob: bright on the downbeat, gold on off-beats */
        disc(CX, PY, 4, 0x8090A8);                           /* pivot */

        int curbeat = (running && pbeat >= 0) ? pbeat % sig : -1;   /* time-signature beat dots + N/4 (M1459) */
        int dx0 = (W - sig * 18) / 2 + 5;
        for (int i = 0; i < sig; i++) {
            int lit = (i == curbeat);
            disc(dx0 + i * 18, 16, lit ? 6 : 4, lit ? (i == 0 ? 0xFFFFC8 : 0xFFE060) : (i == 0 ? 0x8A6A24 : 0x3A444E));   /* downbeat dot always amber-tinted */
        }
        { char sg[4] = { (char)('0' + sig), '/', '4', 0 }; text(sg, 12, 12, C_AMBER); }

        char b[8], t[4]; int p = 0, v = bpm, ti = 0;            /* big BPM readout */
        if (v == 0) t[ti++] = '0';
        while (v) { t[ti++] = '0' + v % 10; v /= 10; }
        while (ti) b[p++] = t[--ti];
        b[p] = 0;
        textS(b, (W - (int)(p) * 24) / 2, H - 56, 3, running ? C_AMBER : C_AMBERLO);   /* BPM glows amber */
        text("BPM", (W - 24) / 2, H - 60, C_DIM);
        text(running ? "space stop  w/s bpm  2-9 sig  q" : "space start  w/s bpm  2-9 sig  q", 18, H - 16, C_DIM);

        int mx, my, mb = sys_mouse(&mx, &my);
        int clicked = (mb & 1) && !(prevb & 1) && mx >= 0; prevb = mb;
        bevel_up(0, 0, W, H, C_BEZHI, C_BEZLO);              /* window frame */
        sys_gfx_blit(FB);
        if (flash > 0) flash--;

        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        else if (k == ' ' || clicked) { running = !running; t0 = now; pbeat = -1; }
        else if (k == 'w' || k == 0x11) { if (bpm < 240) bpm += 5; t0 = now; pbeat = -1; }
        else if (k == 's' || k == 0x12) { if (bpm > 40) bpm -= 5; t0 = now; pbeat = -1; }
        else if (k >= '1' && k <= '9') { sig = k - '0'; t0 = now; pbeat = -1; }   /* set the time signature (beats per bar) */
        sys_sleep(running ? 26 : 90);
    }
    return 0;
}
