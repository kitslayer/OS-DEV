/*
 * tron.c — Tron light cycles against the computer.
 *
 * You (cyan) and the CPU (magenta) race around an arena leaving solid trails.
 * Run into any trail or the wall and you crash. Outlast the CPU. The CPU steers
 * greedily: it keeps going straight while that's safe, otherwise it turns to an
 * open side.
 *
 * Arrows steer (no instant reverse), r restarts, q quits.
 */
#include "ulib.h"

#define W 40
#define H 15
static int g[H][W];                 /* 0 empty, 1 you, 2 cpu */
static int px, py, pdx, pdy;        /* you */
static int ax, ay, adx, ady;        /* cpu */
static int over, youwin;
static const char *msg;

static void reset(void) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) g[y][x] = 0;
    px = 6;  py = H/2; pdx = 1;  pdy = 0;
    ax = W-7; ay = H/2; adx = -1; ady = 0;
    g[py][px] = 1; g[ay][ax] = 2;
    over = 0; youwin = 0; msg = "Arrows steer";
}

static int safe(int x, int y) { return x >= 0 && x < W && y >= 0 && y < H && g[y][x] == 0; }

static void ai_steer(void) {
    int opt[3][2] = { { adx, ady }, { -ady, adx }, { ady, -adx } };   /* straight, then the two turns */
    for (int i = 0; i < 3; i++) if (safe(ax + opt[i][0], ay + opt[i][1])) { adx = opt[i][0]; ady = opt[i][1]; return; }
    /* boxed in: keep going (will crash) */
}

static void step(void) {
    ai_steer();
    int nx = px + pdx, ny = py + pdy;
    int mx = ax + adx, my = ay + ady;
    int pdead = !safe(nx, ny), adead = !safe(mx, my);
    if (!pdead && !adead && nx == mx && ny == my) { pdead = adead = 1; }   /* head-on */
    if (pdead || adead) {
        over = 1;
        youwin = (adead && !pdead);
        msg = pdead && adead ? "Crash - a draw!  (r replay)" :
              youwin ? "CPU crashed - you WIN!  (r replay)" : "You crashed.  (r replay)";
        sys_beep(youwin ? 988 : 165, 200);
        if (!pdead) { px = nx; py = ny; g[py][px] = 1; }
        if (!adead) { ax = mx; ay = my; g[ay][ax] = 2; }
        return;
    }
    px = nx; py = ny; g[py][px] = 1;
    ax = mx; ay = my; g[ay][ax] = 2;
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("\n  Tron"); sys_setcolor(11); print("  you"); sys_setcolor(0); print(" vs "); sys_setcolor(13); print("cpu\n\n"); sys_setcolor(0);
    for (int y = 0; y < H; y++) {
        print("   ");
        for (int x = 0; x < W; x++) {
            int v = g[y][x];
            int head = (x == px && y == py) || (x == ax && y == ay);
            sys_setcolor(v == 1 ? (x==px&&y==py?15:11) : v == 2 ? (x==ax&&y==ay?15:13) : 8);
            print(v == 1 ? (head ? "@" : "o") : v == 2 ? (head ? "@" : "o") : ".");
        }
        sys_setcolor(0); print("\n");
    }
    print("\n  "); print(msg); print("\n  r reset   q quit\n");
}

int main(void) {
    reset();
    render();
    unsigned long last = sys_uptime_ms();
    for (;;) {
        int k;
        while ((k = sys_pollkey()) >= 0) {
            if (k == 'q' || k == 'Q') return 0;
            if (k == 'r' || k == 'R') { reset(); render(); last = sys_uptime_ms(); }
            else if (!over) {
                if      (k == 0x11 && pdy == 0) { pdx = 0;  pdy = -1; }
                else if (k == 0x12 && pdy == 0) { pdx = 0;  pdy = 1;  }
                else if (k == 0x13 && pdx == 0) { pdx = -1; pdy = 0;  }
                else if (k == 0x14 && pdx == 0) { pdx = 1;  pdy = 0;  }
            }
        }
        unsigned long now = sys_uptime_ms();
        if (!over && now - last >= 90) { step(); render(); last = now; }
        sys_sleep(15);
    }
}
