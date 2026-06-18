/*
 * fifteen.c — the 15-puzzle (sliding tiles).
 *
 * A 4x4 grid of tiles 1..15 and one gap. Slide a tile into the gap to work the
 * board back into numerical order. Arrows slide the tile adjacent to the gap in
 * the arrow's direction (so Up moves the tile below the gap up into it). The
 * board is shuffled by random legal slides, so it is always solvable.
 */
#include "ulib.h"

#define N 4
static int g[N][N];                 /* 0 = the gap */
static int br, bc, moves, won;
static int best;                    /* fewest moves to solve, persisted (FIFTEEN.HI); 0 = none yet */

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void putn(int n) {
    char t[8]; int i = 0;
    if (n == 0) { print("0"); return; }
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    char s[8]; int j = 0; while (i) s[j++] = t[--i]; s[j] = 0; print(s);
}

static void load_best(void) {
    char b[16]; long n = sys_readfile("FIFTEEN.HI", b, sizeof(b) - 1);
    best = 0; for (long i = 0; i < n; i++) { if (b[i] < '0' || b[i] > '9') break; best = best * 10 + (b[i] - '0'); }
}
static void save_best(void) {
    char t[12], b[12]; int i = 0, n = 0, v = best;
    if (v == 0) t[i++] = '0'; while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) b[n++] = t[--i];
    sys_writefile("FIFTEEN.HI", b, (unsigned long)n);
}

static int solved(void) {
    int want = 1;
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) {
        if (r == N - 1 && c == N - 1) return g[r][c] == 0;
        if (g[r][c] != want++) return 0;
    }
    return 1;
}

/* swap the gap with the neighbour at (nr,nc) if it's on the board */
static int slide_gap_to(int nr, int nc) {
    if (nr < 0 || nr >= N || nc < 0 || nc >= N) return 0;
    g[br][bc] = g[nr][nc]; g[nr][nc] = 0; br = nr; bc = nc; return 1;
}

static void shuffle(void) {
    int want = 1;
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) g[r][c] = (r == N-1 && c == N-1) ? 0 : want++;
    br = bc = N - 1;
    static const int dr[4] = { -1, 1, 0, 0 }, dc[4] = { 0, 0, -1, 1 };
    for (int i = 0; i < 400; i++) { int d = (int)(rnd() & 3); slide_gap_to(br + dr[d], bc + dc[d]); }
    moves = 0; won = solved();
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("\n  15-Puzzle"); sys_setcolor(0);
    print("    moves="); putn(moves);
    print("  best="); if (best) putn(best); else print("-"); print("\n\n");
    for (int r = 0; r < N; r++) {
        print("     ");
        for (int c = 0; c < N; c++) {
            if (g[r][c] == 0) { sys_setcolor(0); print("    "); }
            else {
                sys_setcolor(g[r][c] & 1 ? 11 : 14);
                if (g[r][c] < 10) print("  ");
                else print(" ");
                putn(g[r][c]); print(" ");
            }
        }
        sys_setcolor(0); print("\n\n");
    }
    print("  ");
    if (won) { sys_setcolor(10); print("Solved in "); putn(moves); print(" moves!  (n = new)"); sys_setcolor(0); }
    else print("arrows slide a tile into the gap");
    print("\n  n new   q quit\n");
}

int main(void) {
    rng = (unsigned)sys_uptime_ms() | 1u;
    load_best();
    shuffle();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 'Q') break;
        if (k == 'n' || k == 'N') { shuffle(); render(); continue; }
        if (won) continue;
        int did = 0;
        if      (k == 0x11) did = slide_gap_to(br + 1, bc);   /* Up: tile below moves up */
        else if (k == 0x12) did = slide_gap_to(br - 1, bc);   /* Down */
        else if (k == 0x13) did = slide_gap_to(br, bc + 1);   /* Left */
        else if (k == 0x14) did = slide_gap_to(br, bc - 1);   /* Right */
        if (did) {
            moves++;
            if (solved()) { won = 1; if (best == 0 || moves < best) { best = moves; save_best(); } sys_beep(880, 120); sys_beep(1320, 140); }
            render();
        }
    }
    return 0;
}
