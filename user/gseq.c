/*
 * gseq.c — a step sequencer, a userspace program (M1421).
 *
 * A 16-step x 5-note grid you program by clicking cells; SPACE plays/stops a
 * looping playhead that beeps the topmost active note in each column on the PC
 * speaker (sys_beep), w/s change the tempo, c clears, q/Esc quits. Pentatonic
 * notes (A G E D C). No FPU.
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

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gseq: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gseq: init failed\n"); return 1; }

    int playing = 0, step = 0, prevb = 0, interval = 150, dirty = 1;
    long last = 0;
    for (;;) {
        long now = sys_uptime_ms();
        if (playing && now - last >= interval) { step = (step + 1) % STEPS; last = now; beepstep(step); dirty = 1; }

        if (dirty) {
            for (int i = 0; i < W * H; i++) FB[i] = 0x12141C;
            text("Step Sequencer", 12, 10, 0x8FD0FF);
            for (int r = 0; r < ROWS; r++) text(NOTE[r], 36, Y0 + r * CW + 4, RC[r]);
            for (int c = 0; c < STEPS; c++) for (int r = 0; r < ROWS; r++) {
                int x = X0 + c * CW, y = Y0 + r * CW;
                fill(x, y, CW - 1, CW - 1, (c == step && playing) ? 0x33405C : (c % 4 == 0) ? 0x20242E : 0x191C24);
                if (cells[c][r]) fill(x + 3, y + 3, CW - 7, CW - 7, RC[r]);
            }
            char st[40]; int p = 0; const char *s1 = playing ? "PLAYING  " : "STOPPED  ";
            while (*s1) st[p++] = *s1++;
            const char *s2 = "tempo "; while (*s2) st[p++] = *s2++;
            int bpm = 60000 / (interval * 4); char t[6]; int ti = 0; if (bpm == 0) t[ti++] = '0'; while (bpm) { t[ti++] = '0' + bpm % 10; bpm /= 10; }
            while (ti) st[p++] = t[--ti];
            const char *s3 = " bpm"; while (*s3) st[p++] = *s3++;
            st[p] = 0;
            text(st, 56, Y0 + ROWS * CW + 12, playing ? 0x70E090 : 0x9098A8);
            text("click cells   space: play   w/s: tempo   c: clear   q: quit", 12, H - 16, 0x707888);
            sys_gfx_blit(FB);
            dirty = 0;
        }

        int mx, my, b = sys_mouse(&mx, &my);                   /* click a cell to toggle it */
        if ((b & 1) && !(prevb & 1) && mx >= X0 && my >= Y0) {
            int c = (mx - X0) / CW, r = (my - Y0) / CW;
            if (c >= 0 && c < STEPS && r >= 0 && r < ROWS) { cells[c][r] ^= 1; dirty = 1; }
        }
        prevb = b;

        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        else if (k == ' ') { playing = !playing; if (playing) { step = 0; last = now; beepstep(0); } dirty = 1; }
        else if (k == 'c' || k == 'C') { for (int c = 0; c < STEPS; c++) for (int r = 0; r < ROWS; r++) cells[c][r] = 0; dirty = 1; }
        else if ((k == 'w' || k == 0x11) && interval > 60) { interval -= 20; dirty = 1; }
        else if ((k == 's' || k == 0x12) && interval < 400) { interval += 20; dirty = 1; }
        sys_sleep(playing ? 18 : 60);
    }
    return 0;
}
