/*
 * g2048.c — the game 2048, a turn-based arrow-key puzzle (6th userspace program).
 *
 * Slide the 4x4 board with the arrows; equal tiles merge and double. Shows that
 * the OS hosts varied interactive programs, and exercises non-blocking input
 * plus the app text grid.
 */
#include "ulib.h"

static unsigned rng = 88172645u;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int g[4][4];
static unsigned score;
static int won;

static void itoa_u(unsigned v, char *o) {
    char t[12]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0; while (i) o[j++] = t[--i];
    o[j] = 0;
}

static void add_tile(void) {
    int empty[16], n = 0;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) if (!g[y][x]) empty[n++] = y*4+x;
    if (!n) return;
    int c = empty[rnd() % n];
    g[c/4][c%4] = (rnd() % 10 == 0) ? 4 : 2;
}

/* slide a 4-cell line toward index 0, merging equal neighbours once. */
static int slide4(int *a) {
    int tmp[4], n = 0, changed = 0;
    for (int i = 0; i < 4; i++) if (a[i]) tmp[n++] = a[i];
    while (n < 4) tmp[n++] = 0;
    for (int i = 0; i < 3; i++) {
        if (tmp[i] && tmp[i] == tmp[i+1]) {
            tmp[i] *= 2; score += tmp[i]; if (tmp[i] == 2048) won = 1;
            for (int j = i+1; j < 3; j++) tmp[j] = tmp[j+1];
            tmp[3] = 0;
        }
    }
    for (int i = 0; i < 4; i++) { if (a[i] != tmp[i]) changed = 1; a[i] = tmp[i]; }
    return changed;
}

static int move(int dir) {              /* 0=left 1=right 2=up 3=down */
    int moved = 0;
    for (int k = 0; k < 4; k++) {
        int line[4];
        for (int i = 0; i < 4; i++) {
            int idx = (dir==1||dir==3) ? 3-i : i;        /* reversed for right/down */
            line[i] = (dir<2) ? g[k][idx] : g[idx][k];
        }
        if (slide4(line)) moved = 1;
        for (int i = 0; i < 4; i++) {
            int idx = (dir==1||dir==3) ? 3-i : i;
            if (dir < 2) g[k][idx] = line[i]; else g[idx][k] = line[i];
        }
    }
    return moved;
}

static int can_move(void) {
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        if (!g[y][x]) return 1;
        if (x < 3 && g[y][x] == g[y][x+1]) return 1;
        if (y < 3 && g[y][x] == g[y+1][x]) return 1;
    }
    return 0;
}

/* palette colour for a tile value (like the real 2048's escalating hues) */
static int tile_color(unsigned v) {
    switch (v) {
        case 0:    return 0;    /* empty '.': green   */
        case 2:    return 1;    /* white   */
        case 4:    return 3;    /* yellow  */
        case 8:    return 7;    /* orange  */
        case 16:   return 13;   /* coral   */
        case 32:   return 2;    /* red     */
        case 64:   return 5;    /* pink    */
        case 128:  return 11;   /* violet  */
        case 256:  return 6;    /* blue    */
        case 512:  return 4;    /* cyan    */
        case 1024: return 10;   /* teal    */
        default:   return 9;    /* 2048+: lime */
    }
}

static void render(const char *msg) {
    sys_clear();
    char sl[40]; int p = 0;
    const char *a = "  2048    score "; while (*a) sl[p++] = *a++;
    char num[12]; itoa_u(score, num); for (int i = 0; num[i]; i++) sl[p++] = num[i];
    sl[p] = 0;
    sys_setcolor(8); print(sl); print("\n\n");           /* header in grey */

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            char cell[8];
            if (g[y][x] == 0) { cell[0]='.'; cell[1]=0; }
            else itoa_u((unsigned)g[y][x], cell);
            int len = 0; while (cell[len]) len++;
            char out[8]; int q = 0;
            for (int s = 0; s < 6 - len; s++) out[q++] = ' ';   /* right-align in 6 */
            for (int i = 0; cell[i]; i++) out[q++] = cell[i];
            out[q] = 0;
            sys_setcolor(tile_color((unsigned)g[y][x]));         /* each tile its own colour */
            print(out);
        }
        print("\n");
    }
    sys_setcolor(0);
    print("\n  arrows to move, q to quit");
    if (msg) { print("\n  "); print(msg); }
}

int main(void) {
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) g[y][x] = 0;
    score = 0; won = 0;
    add_tile(); add_tile();
    render(0);
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q') return 0;
        int dir = -1;
        if (k == 0x13) dir = 0; else if (k == 0x14) dir = 1;
        else if (k == 0x11) dir = 2; else if (k == 0x12) dir = 3;
        if (dir < 0) continue;
        if (move(dir)) { add_tile(); }
        if (won)            render("you reached 2048!");
        else if (!can_move()) render("game over - q to quit");
        else                render(0);
    }
}
