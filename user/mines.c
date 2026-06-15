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

static int t_start, best;       /* clear-time in seconds; best (fastest) persists in MINES.HI */
static int now_secs(void) {     /* "YYYY-MM-DD HH:MM:SS" -> seconds of day */
    char b[40]; sys_time(b, sizeof(b));
    int hh = (b[11]-'0')*10 + (b[12]-'0'), mm = (b[14]-'0')*10 + (b[15]-'0'), ss = (b[17]-'0')*10 + (b[18]-'0');
    return hh*3600 + mm*60 + ss;
}
static void load_best(void) {
    char b[16]; long n = sys_readfile("MINES.HI", b, sizeof(b) - 1);
    best = 0;
    for (long i = 0; i < n; i++) { if (b[i] < '0' || b[i] > '9') break; best = best*10 + (b[i]-'0'); if (best > 100000) { best = 100000; break; } }
}
static void save_best(void) {
    char b[12], t[12]; int k = 0, v = best;
    if (!v) t[k++] = '0'; while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    int i = 0; while (k) b[i++] = t[--k]; b[i] = 0;
    sys_writefile("MINES.HI", b, (unsigned long)i);
}

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

/* palette colour for a board cell (cursor highlighted; classic Minesweeper number hues) */
static int cell_color(char c, int cursor) {
    if (cursor) return 3;          /* cursor: yellow */
    switch (c) {
        case 'F': return 2;        /* flag: red */
        case '#': return 8;        /* hidden: grey */
        case '*': return 13;       /* mine: coral */
        case '.': return 8;        /* empty: grey */
        case '1': return 6;        /* blue  */
        case '2': return 9;        /* green */
        case '3': return 2;        /* red   */
        case '4': return 11;       /* violet */
        case '5': return 13;       /* coral */
        case '6': return 10;       /* teal  */
        case '7': return 12;       /* gold  */
        case '8': return 8;        /* grey  */
        default:  return 0;
    }
}

static void render(void) {
    sys_clear();
    int flags = 0;
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) if (st[y][x]==2) flags++;
    char hdr[64]; int p = 0;
    const char *t = "  Mines  left: ";
    while (*t) hdr[p++] = *t++;
    int left = MINES - flags; if (left < 0) left = 0;
    hdr[p++] = (char)('0' + left/10); hdr[p++] = (char)('0' + left%10);
    t = "  best: "; while (*t) hdr[p++] = *t++;
    if (best <= 0) { hdr[p++] = '-'; hdr[p++] = '-'; }
    else { char nb[8]; int k = 0, v = best; while (v) { nb[k++] = (char)('0' + v % 10); v /= 10; } while (k) hdr[p++] = nb[--k]; hdr[p++] = 's'; }
    hdr[p] = 0;
    sys_setcolor(8); print(hdr); print("\n\n");
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            char c;
            if (st[y][x] == 2)               c = 'F';                 /* flag */
            else if (st[y][x] == 0)          c = (dead && mine[y][x]) ? '*' : '#';
            else if (mine[y][x])             c = '*';                 /* revealed mine (loss) */
            else if (adj[y][x] == 0)         c = '.';
            else                             c = (char)('0' + adj[y][x]);
            int cur = (x == cx && y == cy);
            char cb[4];
            if (cur) { cb[0]='['; cb[1]=c; cb[2]=']'; } else { cb[0]=' '; cb[1]=c; cb[2]=' '; }
            cb[3] = 0;
            sys_setcolor(cell_color(c, cur)); print(cb);
        }
        print("\n");
    }
    sys_setcolor(0);
    print("\n  arrows move, space reveal, f flag, r restart, q quit");
    if (dead)      print("\n  *** BOOM - you hit a mine! (r to retry) ***");
    else if (won)  print("\n  *** cleared! you win! (r to play again) ***");
}

int main(void) {
    char tb[40]; long tn = sys_time(tb, sizeof(tb));   /* seed from the clock for variety */
    rng = 0x2545F491u;
    for (long i = 0; i < tn; i++) rng = rng * 31 + (unsigned char)tb[i];
    if (!rng) rng = 12345u;
    load_best();
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
            if (st[cy][cx] != 2) {
                if (!started) { started = 1; t_start = now_secs(); }
                reveal(cx, cy);
                if (!dead && check_win()) {
                    won = 1;
                    int el = now_secs() - t_start; if (el < 0) el += 86400; if (el < 1) el = 1;
                    if (best == 0 || el < best) { best = el; save_best(); }   /* faster = new record */
                }
            }
        }
        else if (k == 'f') {
            if (st[cy][cx] == 0) st[cy][cx] = 2;
            else if (st[cy][cx] == 2) st[cy][cx] = 0;
        }
        else continue;
        render();
    }
}
