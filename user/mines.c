/*
 * mines.c — Minesweeper, a userspace puzzle game (move a cursor, reveal cells,
 * flag mines). Like the other games it runs ring-3, drawing into the app text
 * grid and reading keys non-blocking. Pure userspace — no kernel changes.
 */
#include "ulib.h"

#define N 9
#define MINES 10

static int mine[N][N];      /* 1 if a mine */
static int adj[N][N];       /* adjacent mine count */
static int st[N][N];        /* 0 hidden, 1 revealed, 2 flagged */
static int cx, cy;          /* cursor */
static int dead, won, started;
static unsigned rng;

static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void reset(void) {
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) { mine[y][x]=adj[y][x]=st[y][x]=0; }
    int placed = 0;
    while (placed < MINES) {
        int x = rnd() % N, y = rnd() % N;
        if (!mine[y][x]) { mine[y][x] = 1; placed++; }
    }
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        int c = 0;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int ny = y+dy, nx = x+dx;
            if (ny>=0&&ny<N&&nx>=0&&nx<N&&mine[ny][nx]) c++;
        }
        adj[y][x] = c;
    }
    cx = cy = N/2; dead = won = started = 0;
}

static void reveal(int x, int y) {
    if (x<0||x>=N||y<0||y>=N) return;
    if (st[y][x] != 0) return;                 /* already revealed or flagged */
    st[y][x] = 1;
    if (mine[y][x]) { dead = 1; return; }
    if (adj[y][x] == 0)                          /* flood-fill empty region */
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
            reveal(x+dx, y+dy);
}

static int check_win(void) {
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++)
        if (!mine[y][x] && st[y][x] != 1) return 0;
    return 1;
}

static void render(void) {
    sys_clear();
    int flags = 0;
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) if (st[y][x]==2) flags++;
    char hdr[48]; int p = 0;
    const char *t = "  Minesweeper   mines left: ";
    while (*t) hdr[p++] = *t++;
    int left = MINES - flags; if (left < 0) left = 0;
    hdr[p++] = (char)('0' + left/10); hdr[p++] = (char)('0' + left%10); hdr[p] = 0;
    print(hdr); print("\n\n");
    for (int y = 0; y < N; y++) {
        char row[64]; int q = 0;
        for (int x = 0; x < N; x++) {
            char c;
            if (st[y][x] == 2)               c = 'F';                 /* flag */
            else if (st[y][x] == 0)          c = (dead && mine[y][x]) ? '*' : '#';
            else if (mine[y][x])             c = '*';                 /* revealed mine (loss) */
            else if (adj[y][x] == 0)         c = '.';
            else                             c = (char)('0' + adj[y][x]);
            if (x == cx && y == cy) { row[q++]='['; row[q++]=c; row[q++]=']'; }
            else                    { row[q++]=' '; row[q++]=c; row[q++]=' '; }
        }
        row[q] = 0; print(row); print("\n");
    }
    print("\n  arrows move, space reveal, f flag, r restart, q quit");
    if (dead)      print("\n  *** BOOM - you hit a mine! (r to retry) ***");
    else if (won)  print("\n  *** cleared! you win! (r to play again) ***");
}

int main(void) {
    char tb[40]; long tn = sys_time(tb, sizeof(tb));   /* seed from the clock for variety */
    rng = 0x2545F491u;
    for (long i = 0; i < tn; i++) rng = rng * 31 + (unsigned char)tb[i];
    if (!rng) rng = 12345u;
    reset();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q') return 0;
        if (k == 'r') { reset(); render(); continue; }
        if (dead || won) continue;             /* only r/q after the game ends */
        if      (k == 0x11) { if (cy > 0)   cy--; }   /* up    */
        else if (k == 0x12) { if (cy < N-1) cy++; }   /* down  */
        else if (k == 0x13) { if (cx > 0)   cx--; }   /* left  */
        else if (k == 0x14) { if (cx < N-1) cx++; }   /* right */
        else if (k == ' ' || k == '\n') {
            if (st[cy][cx] != 2) { reveal(cx, cy); started = 1; if (!dead && check_win()) won = 1; }
        }
        else if (k == 'f') {
            if (st[cy][cx] == 0) st[cy][cx] = 2;
            else if (st[cy][cx] == 2) st[cy][cx] = 0;
        }
        else continue;
        render();
    }
}
