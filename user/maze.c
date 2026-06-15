/*
 * maze.c — a maze explorer, a userspace program.
 *
 * Generates a random perfect maze (iterative recursive-backtracker carving on
 * odd cells) and lets you walk from the top-left to the exit at the bottom-right
 * with the arrow keys. Walls are blue, the player yellow, the exit lime. n makes
 * a new maze, q quits. Runs ring-3 on the app text grid, like the games.
 */
#include "ulib.h"

#define GW 43               /* odd: 21 cells wide  */
#define GH 13               /* odd: 6 cells tall   */
#define EX (GW - 2)         /* exit cell (odd) */
#define EY (GH - 2)

static char wall[GH][GW];   /* 1 = wall, 0 = passage */
static int  px, py;         /* player */
static int  won;

static unsigned rng = 2463534242u;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void gen(void) {
    for (int y = 0; y < GH; y++) for (int x = 0; x < GW; x++) wall[y][x] = 1;
    int stkx[256], stky[256], sp = 0;
    wall[1][1] = 0; stkx[sp] = 1; stky[sp] = 1; sp++;
    const int dx4[4] = { 0, 0, -2, 2 }, dy4[4] = { -2, 2, 0, 0 };
    while (sp > 0) {
        int cx = stkx[sp-1], cy = stky[sp-1];
        int order[4] = { 0, 1, 2, 3 };
        for (int i = 3; i > 0; i--) { int j = rnd() % (i+1); int t = order[i]; order[i] = order[j]; order[j] = t; }
        int moved = 0;
        for (int d = 0; d < 4; d++) {
            int nx = cx + dx4[order[d]], ny = cy + dy4[order[d]];
            if (nx > 0 && nx < GW-1 && ny > 0 && ny < GH-1 && wall[ny][nx] == 1) {
                wall[ny][nx] = 0;
                wall[cy + (ny - cy)/2][cx + (nx - cx)/2] = 0;   /* knock out the wall between */
                stkx[sp] = nx; stky[sp] = ny; sp++;
                moved = 1; break;
            }
        }
        if (!moved) sp--;
    }
    px = 1; py = 1; won = 0;
}

static void render(void) {
    sys_clear();
    sys_setcolor(8);
    print(won ? "  Maze   ** reached the exit! **\n" : "  Maze   reach the lime E\n");
    for (int y = 0; y < GH; y++) {
        for (int x = 0; x < GW; x++) {
            char ch; int col;
            if (x == px && y == py)      { ch = '@'; col = 3; }   /* player: yellow */
            else if (x == EX && y == EY) { ch = 'E'; col = 9; }   /* exit: lime */
            else if (wall[y][x])         { ch = '#'; col = 6; }   /* wall: blue */
            else                         { ch = ' '; col = 0; }
            sys_setcolor(col);
            char cb[2] = { ch, 0 }; print(cb);
        }
        print("\n");
    }
    sys_setcolor(8);
    print(" arrows move   n new maze   q quit\n");
    sys_setcolor(0);
}

static void try_move(int nx, int ny) {
    if (nx < 0 || nx >= GW || ny < 0 || ny >= GH) return;
    if (wall[ny][nx]) return;                  /* can't walk through a wall */
    px = nx; py = ny;
    if (px == EX && py == EY) won = 1;
}

int main(void) {
    char tb[24]; sys_time(tb, sizeof(tb));     /* seed from the clock for variety */
    for (int i = 0; tb[i]; i++) rng = rng * 31 + (unsigned char)tb[i];
    gen();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 27) return 0;
        if (k == 'n') { gen(); render(); continue; }
        if (won) continue;                     /* solved: only n/q until a new maze */
        if      (k == 0x11) try_move(px, py-1);
        else if (k == 0x12) try_move(px, py+1);
        else if (k == 0x13) try_move(px-1, py);
        else if (k == 0x14) try_move(px+1, py);
        else continue;
        render();
    }
}
