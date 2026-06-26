/*
 * gseq.c — a step sequencer, a userspace program (M1421).
 *
 * A 16-step x 5-note grid you program by clicking cells; SPACE plays/stops a
 * looping playhead that beeps the topmost active note in each column on the PC
 * speaker (sys_beep), w/s change the tempo, c clears, q/Esc quits. Pentatonic
 * notes (A G E D C). No FPU. The pattern is saved to SEQ.DAT on every edit and
 * reloaded on launch (M1428), so a beat survives across runs.
 *
 * Launch: `run gseq` from the shell, or the Apps menu ("Sequencer").
 */
#include "ulib.h"

#define W 460
#define H 232
#define STEPS 16
#define ROWS 5
#define CW 24
#define X0 56
#define Y0 44

static unsigned *FB;
static unsigned char FONT[128 * 16];
static char cells[STEPS][ROWS];
static const int FREQ[ROWS] = { 880, 784, 659, 587, 523 };   /* A5 G5 E5 D5 C5 (top = high) */
static const char *NOTE[ROWS] = { "A", "G", "E", "D", "C" };
static const unsigned RC[ROWS] = { 0xF06060, 0xF0B040, 0x60E060, 0x40C0E0, 0x9070F0 };

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void fill(int x0, int y0, int w, int h, unsigned c) { for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) putpx(x, y, c); }
static void ch(char c, int px, int py, unsigned col) { unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16]; for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) putpx(px + b, py + r, col); }
static void text(const char *s, int x, int y, unsigned col) { for (int i = 0; s[i]; i++) { ch(s[i], x, y, col); x += 8; } }
static void beepstep(int s) { for (int r = 0; r < ROWS; r++) if (cells[s][r]) { sys_beep(FREQ[r], 55); return; } }

/* ---- instrument-panel UI kit (M1450; see gconv.c M1430). A step sequencer is a
 * groovebox/control panel, so the faceplate fits — the note cells keep their row colours. */
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

static const char *FNAME = "SEQ.DAT";
static void seq_save(void) {                              /* 16 lines of 5 bits */
    char buf[STEPS * (ROWS + 1)]; int p = 0;
    for (int s = 0; s < STEPS; s++) { for (int r = 0; r < ROWS; r++) buf[p++] = '0' + cells[s][r]; buf[p++] = '\n'; }
    sys_writefile(FNAME, buf, p);
}
static void seq_load(void) {
    char buf[256]; long n = sys_readfile(FNAME, buf, sizeof buf - 1);
    if (n <= 0) return; buf[n] = 0;
    int i = 0;
    for (int s = 0; s < STEPS && i < n; s++) {
        for (int r = 0; r < ROWS && i < n && buf[i] != '\n'; r++) { cells[s][r] = (buf[i] == '1'); i++; }
        while (i < n && buf[i] != '\n') i++;
        if (i < n) i++;
    }
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gseq: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gseq: init failed\n"); return 1; }

    seq_load();                                            /* restore the saved pattern */
    int playing = 0, step = 0, prevb = 0, interval = 150, dirty = 1;
    long last = 0;
    for (;;) {
        long now = sys_uptime_ms();
        if (playing && now - last >= interval) { step = (step + 1) % STEPS; last = now; beepstep(step); dirty = 1; }

        if (dirty) {
            vgrad(0, 0, W, H, C_FACE_T, C_FACE_B);                 /* slate faceplate (M1450) */
            bevel_up(0, 0, W, H, C_BEZHI, C_BEZLO);
            text("STEP SEQUENCER", 12, 10, C_LABEL);
            fill(12, 28, 128, 2, C_AMBERLO);                       /* amber title rule */
            led(W - 24, 11);
            panel(28, Y0 - 8, W - 40, ROWS * CW + 16);             /* recessed step-grid screen */
            for (int r = 0; r < ROWS; r++) text(NOTE[r], 36, Y0 + r * CW + 4, RC[r]);   /* note labels keep row colours */
            for (int c = 0; c < STEPS; c++) for (int r = 0; r < ROWS; r++) {
                int x = X0 + c * CW, y = Y0 + r * CW;
                fill(x, y, CW - 1, CW - 1, (c == step && playing) ? 0x33405C : (c % 4 == 0) ? 0x20242E : 0x141820);
                if (cells[c][r]) fill(x + 3, y + 3, CW - 7, CW - 7, RC[r]);
            }
            int ys = Y0 + ROWS * CW + 14;
            text(playing ? "PLAYING" : "STOPPED", 56, ys, playing ? C_LED : C_DIM);
            text("TEMPO", 156, ys, C_DIM);
            { int bpm = 60000 / (interval * 4); char t[6]; int ti = 0; if (bpm == 0) t[ti++] = '0'; while (bpm) { t[ti++] = '0' + bpm % 10; bpm /= 10; }
              char bs[6]; int bi = 0; while (ti) bs[bi++] = t[--ti]; bs[bi] = 0;
              text(bs, 212, ys, C_AMBER); text("BPM", 212 + bi * 8 + 4, ys, C_DIM); }   /* tempo glows amber */
            text("click cells   space play   w/s tempo   c clear   q quit", 12, H - 16, C_DIM);
            sys_gfx_blit(FB);
            dirty = 0;
        }

        int mx, my, b = sys_mouse(&mx, &my);                   /* click a cell to toggle it */
        if ((b & 1) && !(prevb & 1) && mx >= X0 && my >= Y0) {
            int c = (mx - X0) / CW, r = (my - Y0) / CW;
            if (c >= 0 && c < STEPS && r >= 0 && r < ROWS) { cells[c][r] ^= 1; seq_save(); dirty = 1; }
        }
        prevb = b;

        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        else if (k == ' ') { playing = !playing; if (playing) { step = 0; last = now; beepstep(0); } dirty = 1; }
        else if (k == 'c' || k == 'C') { for (int c = 0; c < STEPS; c++) for (int r = 0; r < ROWS; r++) cells[c][r] = 0; seq_save(); dirty = 1; }
        else if ((k == 'w' || k == 0x11) && interval > 60) { interval -= 20; dirty = 1; }
        else if ((k == 's' || k == 0x12) && interval < 400) { interval += 20; dirty = 1; }
        sys_sleep(playing ? 18 : 60);
    }
    return 0;
}
