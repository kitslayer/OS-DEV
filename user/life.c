/*
 * life.c — Conway's Game of Life, a userspace program.
 *
 * A cellular automaton on a toroidal grid: each generation a live cell survives
 * with 2 or 3 live neighbours, and a dead cell with exactly 3 neighbours is
 * born. Runs autonomously (sys_pollkey + sys_sleep), like the other games.
 */
#include "ulib.h"

#define W 40
#define H 14

static unsigned rng = 88172645u;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static char cur[H][W], nxt[H][W];

static void randomize(void) { for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) cur[y][x] = (rnd() & 3) == 0; }
static void clear_board(void) { for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) cur[y][x] = 0; }

static void step(void) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int n = 0;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            n += cur[(y+dy+H)%H][(x+dx+W)%W];                  /* wrap toroidally */
        }
        nxt[y][x] = cur[y][x] ? (n == 2 || n == 3) : (n == 3);
    }
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) cur[y][x] = nxt[y][x];
}

static void render(unsigned gen, int paused) {
    sys_clear();
    char sl[56]; int p = 0;
    const char *a = "  life  gen "; while (*a) sl[p++] = *a++;
    char t[12]; int i = 0; unsigned v = gen; if (!v) t[i++] = '0'; while (v) { t[i++] = '0' + v % 10; v /= 10; }
    while (i) sl[p++] = t[--i];
    a = paused ? "  [paused]" : "  [run]"; while (*a) sl[p++] = *a++;
    sl[p] = 0; sys_setcolor(8); print(sl); print("\n");          /* status: grey */

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (cur[y][x]) { sys_setcolor(9); print("#"); }       /* live cell: lime */
            else           { sys_setcolor(0); print(" "); }
        }
        print("\n");
    }
    sys_setcolor(8); print("space=pause r=rand c=clear s=step q=quit\n"); sys_setcolor(0);
}

int main(void) {
    randomize();
    unsigned gen = 0; int paused = 0;
    render(gen, paused);
    for (;;) {
        int k;
        while ((k = sys_pollkey()) >= 0) {
            if (k == 'q' || k == 27) return 0;
            else if (k == ' ') paused = !paused;
            else if (k == 'r') { randomize(); gen = 0; }
            else if (k == 'c') { clear_board(); paused = 1; gen = 0; }
            else if (k == 's' && paused) { step(); gen++; }
            render(gen, paused);
        }
        if (!paused) { step(); gen++; render(gen, paused); sys_sleep(150); }
        else sys_sleep(40);
    }
}
