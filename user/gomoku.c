/*
 * gomoku.c — Gomoku (Five in a Row) against the computer.
 *
 * Place stones on an 11x11 board; first to get five in a row (horizontal,
 * vertical, or either diagonal) wins. You are X, the CPU is O. The CPU plays a
 * heuristic: for every empty point it weighs the line it would build for itself
 * against the line it would deny you, and takes the best — so it both attacks
 * and blocks.
 *
 * Arrows move the cursor, Space places a stone, r restarts, q quits.
 */
#include "ulib.h"

#define N 11
static int b[N][N];                 /* 0 empty, 1 you (X), 2 cpu (O) */
static int cr = N/2, cc = N/2, over, winner;
static const char *msg;

static const int DR[4] = { 0, 1, 1, 1 };
static const int DC[4] = { 1, 0, 1, -1 };

/* longest run of `p` through (r,c) along direction d (counting both ways, +1 for the cell itself) */
static int run_len(int r, int c, int p, int d) {
    int len = 1;
    for (int s = -1; s <= 1; s += 2) {
        int rr = r + s*DR[d], cc2 = c + s*DC[d];
        while (rr >= 0 && rr < N && cc2 >= 0 && cc2 < N && b[rr][cc2] == p) { len++; rr += s*DR[d]; cc2 += s*DC[d]; }
    }
    return len;
}
static int wins_at(int r, int c, int p) {
    for (int d = 0; d < 4; d++) if (run_len(r, c, p, d) >= 5) return 1;
    return 0;
}

/* heuristic value of `p` playing (r,c): reward the longest line it makes */
static int line_score(int r, int c, int p) {
    int best = 0;
    for (int d = 0; d < 4; d++) { int l = run_len(r, c, p, d); if (l > best) best = l; }
    if (best >= 5) return 100000;
    return best * best;             /* 1,4,9,16 for runs of 1..4 */
}

static void cpu_move(void) {
    /* an immediate win always takes priority over blocking (or the softer
     * mine/block heuristic below) -- checked first and separately, matching
     * c4.c's ai_move() structure, rather than folded into the weighted score
     * below, where block's higher weight (x5) silently outscored an available
     * win (x4: both line_score to 100000 for completing a line, win or block
     * alike, so mine*4=400000 < block*5=500000 even when a win exists). */
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) {
        if (b[r][c]) continue;
        b[r][c] = 2;
        if (wins_at(r, c, 2)) { over = 1; winner = 2; msg = "CPU wins.  (r replay)"; sys_beep(165, 250); return; }
        b[r][c] = 0;
    }
    int br = -1, bc = -1, best = -1;
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) {
        if (b[r][c]) continue;
        int mine = line_score(r, c, 2);            /* what O builds here */
        int block = line_score(r, c, 1);           /* what it denies X here */
        int cen = (N - (r < N-r ? N-1-2*r : 2*r-(N-1)) - (c < N-c ? N-1-2*c : 2*c-(N-1)));  /* centre bias */
        int score = mine * 4 + block * 5 + cen;    /* slightly prefer blocking; nudge to centre */
        if (score > best) { best = score; br = r; bc = c; }
    }
    if (br < 0) return;
    b[br][bc] = 2;
    if (wins_at(br, bc, 2)) { over = 1; winner = 2; msg = "CPU wins.  (r replay)"; sys_beep(165, 250); }
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("  Gomoku"); sys_setcolor(0);
    print("   you="); sys_setcolor(2); print("X"); sys_setcolor(0);
    print(" cpu="); sys_setcolor(3); print("O"); sys_setcolor(0); print("   five in a row\n");
    for (int r = 0; r < N; r++) {
        print("  ");
        for (int c = 0; c < N; c++) {
            int v = b[r][c], cur = (r == cr && c == cc);
            char ch = v == 1 ? 'X' : v == 2 ? 'O' : '.';
            sys_setcolor(v == 1 ? 2 : v == 2 ? 3 : 8);
            char cell[4]; cell[0] = cur ? '[' : ' '; cell[1] = ch; cell[2] = cur ? ']' : ' '; cell[3] = 0;
            print(cell);
        }
        sys_setcolor(0); print("\n");
    }
    print("  "); print(msg); print("\n");
    print("  arrows move  space place  r reset  q quit\n");
}

static void reset(void) {
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) b[r][c] = 0;
    cr = cc = N/2; over = 0; winner = 0; msg = "Your move (X)";
}

int main(void) {
    reset(); render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 'Q') break;
        if (k == 'r' || k == 'R') { reset(); render(); continue; }
        if (over) continue;
        if      (k == 0x11) { if (cr > 0) cr--; render(); }
        else if (k == 0x12) { if (cr < N-1) cr++; render(); }
        else if (k == 0x13) { if (cc > 0) cc--; render(); }
        else if (k == 0x14) { if (cc < N-1) cc++; render(); }
        else if (k == ' ' || k == '\n' || k == '\r') {
            if (b[cr][cc]) continue;
            b[cr][cc] = 1;
            if (wins_at(cr, cc, 1)) { over = 1; winner = 1; msg = "You WIN!  (r replay)"; sys_beep(880,150); sys_beep(1320,150); }
            else { int full = 1; for (int r=0;r<N;r++) for (int c=0;c<N;c++) if(!b[r][c]) full=0;
                   if (full) { over = 1; msg = "Draw.  (r replay)"; }
                   else { cpu_move(); if (!over) msg = "Your move (X)"; } }
            render();
        }
    }
    return 0;
}
