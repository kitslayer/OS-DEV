/*
 * snake.c — a real-time arrow-key game, as a userspace program.
 *
 * Demonstrates non-blocking input (sys_pollkey) driving a game loop: read any
 * pending keys, advance the snake, redraw the board, sleep, repeat. It's the
 * first program that reacts to input *without blocking* — the snake keeps
 * moving whether or not you press a key.
 */
#include "ulib.h"

#define W 40
#define H 14

static unsigned rng = 2463534242u;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void itoa_u(unsigned v, char *o) {
    char t[12]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0; while (i) o[j++] = t[--i];
    o[j] = 0;
}

static int sx[W * H], sy[W * H];
static unsigned best;          /* high score, persisted to SNAKE.HI */
static void load_best(void) {
    char b[16]; long n = sys_readfile("SNAKE.HI", b, sizeof(b) - 1);
    best = 0; for (long i = 0; i < n; i++) { if (b[i] < '0' || b[i] > '9') break; best = best * 10 + (b[i] - '0'); }
}
static void save_best(void) {
    char b[12]; itoa_u(best, b); int n = 0; while (b[n]) n++; sys_writefile("SNAKE.HI", b, (unsigned long)n);
}

static void render(int len, int fx, int fy, unsigned score, const char *msg) {
    static char g[H][W];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) g[y][x] = ' ';
    g[fy][fx] = '*';
    for (int i = 0; i < len; i++) g[sy[i]][sx[i]] = (i == 0) ? '@' : 'o';

    sys_clear();
    char sl[48]; int p = 0;
    const char *a = "  snake   score "; while (*a) sl[p++] = *a++;
    char num[12]; itoa_u(score, num); for (int i = 0; num[i]; i++) sl[p++] = num[i];
    a = "   best "; while (*a) sl[p++] = *a++;
    itoa_u(best, num); for (int i = 0; num[i]; i++) sl[p++] = num[i];
    sl[p] = 0;
    sys_setcolor(8); print(sl); print("\n");           /* header in grey */

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            char ch = g[y][x]; int col = 0;
            if (ch == '*')      col = 2;   /* food: red    */
            else if (ch == '@') col = 3;   /* head: yellow */
            else if (ch == 'o') col = 9;   /* body: lime   */
            sys_setcolor(col);
            char cb[2] = { ch, 0 }; print(cb);
        }
        print("\n");
    }
    sys_setcolor(0);
    if (msg) { print(msg); }
}

int main(void) {
    int len, dx, dy, fx, fy; unsigned score;
    load_best();
restart:
    len = 3; dx = 1; dy = 0; score = 0;
    sx[0] = W/2; sy[0] = H/2;
    sx[1] = sx[0]-1; sy[1] = sy[0];
    sx[2] = sx[0]-2; sy[2] = sy[0];
    fx = (int)(rnd() % W); fy = (int)(rnd() % H);

    for (;;) {
        int k;
        while ((k = sys_pollkey()) >= 0) {
            if      (k == 0x11 && dy != 1)  { dx = 0; dy = -1; }   /* up    */
            else if (k == 0x12 && dy != -1) { dx = 0; dy = 1;  }   /* down  */
            else if (k == 0x13 && dx != 1)  { dx = -1; dy = 0; }   /* left  */
            else if (k == 0x14 && dx != -1) { dx = 1; dy = 0;  }   /* right */
            else if (k == 'q') return 0;
        }
        int nx = sx[0] + dx, ny = sy[0] + dy;
        int dead = (nx < 0 || nx >= W || ny < 0 || ny >= H);
        int eating = (nx == fx && ny == fy);
        /* the tail (index len-1) vacates this same tick (see the shift below) UNLESS
         * eating grows the snake, in which case a new segment refills that exact
         * cell -- so moving there is only safe when NOT eating (M1633). */
        int check_len = eating ? len : len - 1;
        for (int i = 0; i < check_len && !dead; i++) if (sx[i] == nx && sy[i] == ny) dead = 1;
        if (dead) break;

        int tx = sx[len-1], ty = sy[len-1];
        for (int i = len - 1; i > 0; i--) { sx[i] = sx[i-1]; sy[i] = sy[i-1]; }
        sx[0] = nx; sy[0] = ny;
        if (nx == fx && ny == fy) {                 /* ate the food: grow */
            if (len < W*H) { sx[len] = tx; sy[len] = ty; len++; }
            score += 10;
            if (score > best) { best = score; save_best(); }
            fx = (int)(rnd() % W); fy = (int)(rnd() % H);
        }
        render(len, fx, fy, score, 0);
        sys_sleep(110);
    }

    render(len, fx, fy, score, "GAME OVER - any key to retry, q to quit\n");
    for (;;) {
        int k = sys_pollkey();
        if (k == 'q') return 0;
        if (k >= 0) goto restart;
        sys_sleep(50);
    }
}
