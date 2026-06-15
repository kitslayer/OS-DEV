/*
 * matrix.c — a "digital rain" screensaver, a userspace program.
 *
 * Columns of random glyphs fall down the screen: a bright white head, a green
 * trail that fades behind it. Ambient (no input but q to quit) — the first
 * non-interactive animated app. Uses the per-cell colour palette (M311). Ring-3.
 */
#include "ulib.h"

#define W 43            /* one less than the 44-col grid (a full row + '\n' would wrap) */
#define H 16
#define TRAIL 6

static unsigned rng = 2166136261u;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static char randch(void) {
    static const char S[] = "abcdefghijklmnopqrstuvwxyz0123456789@#$%&*+=<>?/";
    return S[rnd() % (sizeof(S) - 1)];
}

static int  drop[W];           /* head row of each column (may start above the top) */
static char g[H][W];
static unsigned char gc[H][W];

int main(void) {
    char tb[24]; sys_time(tb, sizeof(tb));
    for (int i = 0; tb[i]; i++) rng = rng * 31 + (unsigned char)tb[i];
    for (int x = 0; x < W; x++) drop[x] = -(int)(rnd() % H);   /* stagger starts */

    for (;;) {
        int k = sys_pollkey();
        if (k == 'q' || k == 27) return 0;

        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { g[y][x] = ' '; gc[y][x] = 0; }
        for (int x = 0; x < W; x++) {
            for (int i = 0; i < TRAIL; i++) {
                int yy = drop[x] - i;
                if (yy >= 0 && yy < H) {
                    g[yy][x] = randch();
                    gc[yy][x] = (i == 0) ? 1 : (i < 3 ? 9 : 0);   /* head white, then lime, then green */
                }
            }
            drop[x]++;
            if (drop[x] - TRAIL >= H) drop[x] = -(int)(rnd() % H);  /* recycle above the top */
        }

        sys_clear();
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                sys_setcolor(g[y][x] == ' ' ? 0 : gc[y][x]);
                char cb[2] = { g[y][x], 0 }; print(cb);
            }
            print("\n");
        }
        sys_sleep(90);
    }
}
