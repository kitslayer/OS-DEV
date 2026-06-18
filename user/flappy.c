/*
 * flappy.c — Flappy Bird.
 *
 * Tap Space to flap upward against gravity and thread the bird through the gaps
 * in the scrolling pipes. Touch a pipe, the ceiling, or the ground and it's
 * over. One point per pipe cleared; the best is saved to FLAPPY.HI.
 *
 * Space (or Up) flaps, r restarts after a crash, q quits. Real-time: integer
 * "sub-row" physics (no floating point), paced by a short sleep each frame.
 */
#include "ulib.h"

#define W 40
#define H 12                        /* field rows; header+field+footer must fit the 17-row grid */
#define BX 8                        /* bird column (fixed) */
#define SUB 8                       /* sub-rows per row (integer physics scale) */
#define GAP 5                       /* pipe gap height (rows) */
#define NPIPE 4

static int by, bv;                  /* bird vertical position + velocity, in sub-rows */
static int px[NPIPE], pgap[NPIPE], pon[NPIPE];   /* pipe x, gap-top row, active */
static int score, hi, dead, started;
static unsigned long t_pipe;

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void putn(int n) {
    char t[8]; int i = 0;
    if (n == 0) { print("0"); return; }
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    char s[8]; int j = 0; while (i) s[j++] = t[--i]; s[j] = 0; print(s);
}

static void load_hi(void) {
    char b[16]; long n = sys_readfile("FLAPPY.HI", b, sizeof(b) - 1);
    hi = 0; for (long i = 0; i < n; i++) { if (b[i] < '0' || b[i] > '9') break; hi = hi * 10 + (b[i] - '0'); }
}
static void save_hi(void) {
    char t[12], b[12]; int i = 0, n = 0, v = hi;
    if (v == 0) t[i++] = '0'; while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) b[n++] = t[--i];
    sys_writefile("FLAPPY.HI", b, (unsigned long)n);
}

static void reset(void) {
    by = (H / 2) * SUB; bv = 0;
    for (int i = 0; i < NPIPE; i++) pon[i] = 0;
    score = 0; dead = 0; started = 0;
    t_pipe = sys_uptime_ms();
}

static void spawn_pipe(void) {
    for (int i = 0; i < NPIPE; i++) if (!pon[i]) {
        pon[i] = 1; px[i] = W - 1;
        pgap[i] = 1 + (int)(rnd() % (H - GAP - 2));   /* gap top, leaving room top+bottom */
        return;
    }
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("\n  Flappy"); sys_setcolor(0);
    print("     score "); sys_setcolor(3); putn(score); sys_setcolor(0);
    print("   hi "); sys_setcolor(14); putn(hi); sys_setcolor(0); print("\n\n");
    int brow = by / SUB;
    static char f[H][W + 1];
    for (int y = 0; y < H; y++) { for (int x = 0; x < W; x++) f[y][x] = ' '; f[y][W] = 0; }
    for (int i = 0; i < NPIPE; i++) if (pon[i] && px[i] >= 0 && px[i] < W)
        for (int y = 0; y < H; y++) if (y < pgap[i] || y >= pgap[i] + GAP) f[y][px[i]] = '|';
    if (brow >= 0 && brow < H) f[brow][BX] = dead ? 'X' : '>';
    for (int y = 0; y < H; y++) {
        print("  ");
        for (int x = 0; x < W; x++) {
            char c = f[y][x];
            sys_setcolor(c == '>' ? 14 : c == 'X' ? 2 : c == '|' ? 10 : 0);
            char s[2]; s[0] = c == ' ' ? '.' : c; s[1] = 0;
            if (c == ' ') { sys_setcolor(8); print(" "); } else print(s);
        }
        sys_setcolor(0); print("\n");
    }
    print("  ");
    if (dead) { sys_setcolor(2); print("CRASH! score "); putn(score); print("   r restart  q quit"); sys_setcolor(0); }
    else if (!started) print("press Space to start  (Space flaps)");
    else print("Space flap   q quit");
    print("\n");
}

int main(void) {
    rng = (unsigned)sys_uptime_ms() | 1u;
    load_hi();
    reset();
    render();
    for (;;) {
        int k, flap = 0;
        while ((k = sys_pollkey()) >= 0) {
            if (k == 'q' || k == 'Q') return 0;
            if (dead) { if (k == 'r' || k == 'R') { reset(); render(); } }
            else if (k == ' ' || k == 0x11) { flap = 1; started = 1; }
        }
        if (!dead && started) {
            if (flap) bv = -SUB - SUB/2;            /* an upward kick */
            bv += 2;                                /* gravity (sub-rows / frame^2) */
            by += bv;
            int brow = by / SUB;
            if (brow < 0) { by = 0; bv = 0; }
            if (brow >= H) { dead = 1; }            /* hit the ground */

            for (int i = 0; i < NPIPE; i++) if (pon[i]) {
                int oldx = px[i]; px[i]--;
                if (px[i] < 0) { pon[i] = 0; continue; }
                if (oldx > BX && px[i] <= BX) score++;   /* cleared a pipe */
                if (px[i] == BX) {                       /* at the bird column: gap check */
                    int r = by / SUB;
                    if (r < pgap[i] || r >= pgap[i] + GAP) dead = 1;
                }
            }
            unsigned long now = sys_uptime_ms();
            if (now - t_pipe >= 1100) { spawn_pipe(); t_pipe = now; }

            if (dead) { if (score > hi) { hi = score; save_hi(); } sys_beep(160, 200); }
            render();
        }
        sys_sleep(80);
    }
}
