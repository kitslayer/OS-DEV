/*
 * tetris.c — Tetris, a userspace program (8th app).
 *
 * The 7 tetrominoes are 4x4 bit-masks rotated at runtime. A gravity tick drops
 * the piece; on landing it locks into the board, full rows are cleared, and a
 * new piece spawns. Non-blocking input (arrows to move/rotate, space to drop)
 * drives the loop, like the other games. Board is 10 wide x 16 tall (fits the
 * window's text grid).
 */
#include "ulib.h"

#define BW 10
#define BH 16

static unsigned rng = 2463534242u;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* spawn masks for I,O,T,S,Z,J,L (4x4, bit = y*4+x) */
static const unsigned short PIECE[7] = { 0x00F0,0x0660,0x0270,0x0360,0x0630,0x0470,0x0170 };
static const unsigned char COLOR[7] = { 4, 3, 11, 9, 2, 6, 7 };  /* I cyan, O yellow, T violet, S green, Z red, J blue, L orange */

static char board[BH][BW];        /* 0 = empty, else the piece glyph */
static int curp, curx, cury;
static unsigned short curmask;
static unsigned score, lines;
static int over;
static unsigned best;          /* high score, persisted to TETRIS.HI */
static void itoa_u(unsigned v, char *o) { char t[12]; int i=0; if(!v)t[i++]='0'; while(v){t[i++]=(char)('0'+v%10);v/=10;} int j=0; while(i)o[j++]=t[--i]; o[j]=0; }
static void load_best(void) { char b[16]; long n=sys_readfile("TETRIS.HI",b,sizeof(b)-1); best=0; for(long i=0;i<n;i++){if(b[i]<'0'||b[i]>'9'||best>=100000000u)break;best=best*10+(b[i]-'0');} }   /* cap: a corrupt .HI can't wrap the unsigned score */
static void save_best(void) { char b[12]; itoa_u(best,b); int n=0; while(b[n])n++; sys_writefile("TETRIS.HI",b,(unsigned long)n); }

static unsigned short rot_cw(unsigned short m) {
    unsigned short r = 0;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
        if (m & (1 << (y*4 + x))) r |= 1 << (x*4 + (3 - y));
    return r;
}

/* does mask at (ox,oy) collide with walls/floor/locked cells? */
static int collide(unsigned short m, int ox, int oy) {
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        if (!(m & (1 << (y*4 + x)))) continue;
        int bx = ox + x, by = oy + y;
        if (bx < 0 || bx >= BW || by >= BH) return 1;
        if (by >= 0 && board[by][bx]) return 1;
    }
    return 0;
}

static void spawn(void) {
    curp = (int)(rnd() % 7);
    curmask = PIECE[curp];
    curx = 3; cury = -1;
    if (collide(curmask, curx, cury)) over = 1;
}

static void lock_and_clear(void) {
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
        if (curmask & (1 << (y*4 + x))) {
            int bx = curx + x, by = cury + y;
            if (by >= 0 && by < BH && bx >= 0 && bx < BW) board[by][bx] = (char)(curp + 1); /* piece index + 1 (nonzero) */
        }
    int cleared = 0;
    for (int y = BH - 1; y >= 0; ) {
        int full = 1;
        for (int x = 0; x < BW; x++) if (!board[y][x]) { full = 0; break; }
        if (full) {
            for (int yy = y; yy > 0; yy--) for (int x = 0; x < BW; x++) board[yy][x] = board[yy-1][x];
            for (int x = 0; x < BW; x++) board[0][x] = 0;
            cleared++;
        } else y--;
    }
    if (cleared) { lines += cleared; score += cleared * cleared * 100;
                   if (score > best) { best = score; save_best(); } }
    spawn();
}

static void step_down(void) {
    if (!collide(curmask, curx, cury + 1)) cury++;
    else lock_and_clear();
}

static void render(const char *msg) {
    sys_clear();
    char st[80]; int p = 0; char n[12];
    const char *a = "TETRIS  score "; while (*a) st[p++] = *a++;
    itoa_u(score, n); for (int i = 0; n[i]; i++) st[p++] = n[i];   /* unsigned (was an (int) cast that garbled >INT_MAX) */
    a = "  lines "; while (*a) st[p++] = *a++;
    itoa_u(lines, n); for (int i = 0; n[i]; i++) st[p++] = n[i];
    a = "  best "; while (*a) st[p++] = *a++;
    itoa_u(best, n); for (int i = 0; n[i]; i++) st[p++] = n[i];
    st[p] = 0; sys_setcolor(8); print(st); print("\n");

    /* compose board + current piece into render + colour grids */
    static char g[BH][BW]; static unsigned char gc[BH][BW];
    for (int y = 0; y < BH; y++) for (int x = 0; x < BW; x++) {
        if (board[y][x]) { g[y][x]='#'; gc[y][x]=COLOR[(board[y][x]-1) % 7]; }
        else             { g[y][x]=' '; gc[y][x]=0; }
    }
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
        if (curmask & (1 << (y*4 + x))) { int bx = curx+x, by = cury+y;
            if (by>=0&&by<BH&&bx>=0&&bx<BW) { g[by][bx]='#'; gc[by][bx]=COLOR[curp]; } }

    for (int y = 0; y < BH; y++) {
        sys_setcolor(8); print("|");                       /* walls in grey */
        for (int x = 0; x < BW; x++) {
            if (g[y][x] == ' ') { sys_setcolor(0); print(" "); }
            else { sys_setcolor(gc[y][x]); print("#"); }   /* each block its piece colour */
        }
        sys_setcolor(8); print(y < BH - 1 ? "|\n" : "|");   /* no trailing newline on the last row, or the header scrolls off the 17-row grid */
    }
    sys_setcolor(0);
    if (msg) print(msg);
}

int main(void) {
    load_best();
restart:
    for (int y = 0; y < BH; y++) for (int x = 0; x < BW; x++) board[y][x] = 0;
    score = lines = 0; over = 0;
    spawn();
    int tick = 0;
    for (;;) {
        int k;
        while ((k = sys_pollkey()) >= 0) {
            if (k == 'q') return 0;
            else if (k == 0x13) { if (!collide(curmask, curx-1, cury)) curx--; }      /* left  */
            else if (k == 0x14) { if (!collide(curmask, curx+1, cury)) curx++; }      /* right */
            else if (k == 0x12) step_down();                                          /* down  */
            else if (k == 0x11) { unsigned short r = rot_cw(curmask); if (!collide(r, curx, cury)) curmask = r; } /* rotate */
            else if (k == ' ') { while (!collide(curmask, curx, cury+1)) cury++; step_down(); }  /* hard drop */
            if (over) break;
            render(0);
        }
        if (over) break;
        if (++tick >= 5) { tick = 0; step_down(); render(0); }   /* gravity ~ every 5*40ms */
        sys_sleep(40);
    }
    render("\n GAME OVER - any key to retry, q to quit\n");
    for (;;) { int k = sys_pollkey(); if (k == 'q') return 0; if (k >= 0) goto restart; sys_sleep(50); }
}
