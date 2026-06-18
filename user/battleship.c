/*
 * battleship.c — Battleship against the computer.
 *
 * Two 8x8 seas. Both fleets (sizes 5,4,3,3,2) are placed at random. You fire at
 * the enemy sea (left, hidden); the CPU fires back at yours (right) using a
 * hunt-then-target AI: random shots until it hits, then it works the cells
 * around that hit. Sink the enemy's whole fleet (17 cells) before it sinks
 * yours. Arrows move the crosshair, Space fires.
 */
#include "ulib.h"

#define N 8
static const int ships[] = { 5, 4, 3, 3, 2 };
#define NSHIP 5
#define TOTAL 17                     /* 5+4+3+3+2 */

static int eship[N][N], eshot[N][N]; /* enemy sea: a ship cell? / have you fired here? */
static int yship[N][N], yshot[N][N]; /* your sea:  a ship cell? / has the CPU fired here? */
static int cr, cc;                   /* crosshair on the enemy sea */
static int ehits, yhits;             /* hits scored on enemy / on you */
static int over, youwin;
static const char *msg;

/* AI target stack (cells to try after a hit) */
static int tstk[64][2], tn;

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void putn(int n) {
    char t[8]; int i = 0;
    if (n == 0) { print("0"); return; }
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    char s[8]; int j = 0; while (i) s[j++] = t[--i]; s[j] = 0; print(s);
}

static void place(int ship[N][N]) {
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) ship[r][c] = 0;
    for (int s = 0; s < NSHIP; s++) {
        int len = ships[s];
        for (;;) {
            int horiz = (int)(rnd() & 1);
            int r = (int)(rnd() % N), c = (int)(rnd() % N);
            if (horiz && c + len > N) continue;
            if (!horiz && r + len > N) continue;
            int ok = 1;
            for (int i = 0; i < len; i++) { int rr = r + (horiz?0:i), cc2 = c + (horiz?i:0); if (ship[rr][cc2]) { ok = 0; break; } }
            if (!ok) continue;
            for (int i = 0; i < len; i++) { int rr = r + (horiz?0:i), cc2 = c + (horiz?i:0); ship[rr][cc2] = 1; }
            break;
        }
    }
}

static void reset(void) {
    place(eship); place(yship);
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) { eshot[r][c] = yshot[r][c] = 0; }
    cr = cc = 0; ehits = yhits = 0; over = youwin = 0; tn = 0;
    msg = "Fire! (arrows + space)";
}

static void push_target(int r, int c) {
    if (r < 0 || r >= N || c < 0 || c >= N || yshot[r][c]) return;
    if (tn < 64) { tstk[tn][0] = r; tstk[tn][1] = c; tn++; }
}

static void ai_fire(void) {
    int r = -1, c = -1;
    while (tn > 0) { tn--; int tr = tstk[tn][0], tc = tstk[tn][1]; if (!yshot[tr][tc]) { r = tr; c = tc; break; } }
    if (r < 0) {                                  /* hunt: random unshot cell */
        do { r = (int)(rnd() % N); c = (int)(rnd() % N); } while (yshot[r][c]);
    }
    yshot[r][c] = 1;
    if (yship[r][c]) {
        yhits++; sys_beep(220, 60);
        push_target(r-1, c); push_target(r+1, c); push_target(r, c-1); push_target(r, c+1);
        if (yhits >= TOTAL) { over = 1; youwin = 0; }
    }
}

static void fire(void) {
    if (eshot[cr][cc]) return;
    eshot[cr][cc] = 1;
    if (eship[cr][cc]) {
        ehits++; sys_beep(990, 60); msg = "HIT!";
        if (ehits >= TOTAL) { over = 1; youwin = 1; sys_beep(1320, 200); return; }
    } else { msg = "miss - CPU's turn"; }
    ai_fire();
    if (over) msg = "Your fleet is sunk.";
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("\n  Battleship"); sys_setcolor(0);
    print("   enemy "); sys_setcolor(2); putn(ehits); sys_setcolor(0);
    print("/17   you "); sys_setcolor(3); putn(yhits); sys_setcolor(0); print("/17\n\n");
    sys_setcolor(8); print("   ENEMY            YOU\n"); sys_setcolor(0);
    for (int r = 0; r < N; r++) {
        print("   ");
        for (int c = 0; c < N; c++) {                 /* enemy sea (hidden ships) */
            int cur = (r == cr && c == cc);
            char ch; int col;
            if (eshot[r][c] && eship[r][c]) { ch = 'X'; col = cur ? 15 : 2; }
            else if (eshot[r][c])           { ch = 'o'; col = cur ? 15 : 8; }
            else                            { ch = cur ? '+' : '~'; col = cur ? 14 : 4; }
            sys_setcolor(col);
            char s[3]; s[0] = ch; s[1] = ' '; s[2] = 0; print(s);
        }
        print("    ");
        for (int c = 0; c < N; c++) {                 /* your sea (ships visible) */
            char ch; int col;
            if (yshot[r][c] && yship[r][c]) { ch = 'X'; col = 2; }
            else if (yshot[r][c])           { ch = 'o'; col = 8; }
            else if (yship[r][c])           { ch = 'S'; col = 10; }
            else                            { ch = '~'; col = 4; }
            sys_setcolor(col); char s[3]; s[0] = ch; s[1] = ' '; s[2] = 0; print(s);
        }
        sys_setcolor(0); print("\n");
    }
    print("\n  ");
    if (over) { sys_setcolor(youwin?10:2); print(youwin ? "YOU WIN! Fleet destroyed. (r replay)" : "DEFEAT - fleet sunk. (r replay)"); sys_setcolor(0); }
    else print(msg);
    print("\n  arrows aim  space fire  r reset  q quit\n");
}

int main(void) {
    rng = (unsigned)sys_uptime_ms() | 1u;
    reset();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 'Q') break;
        if (k == 'r' || k == 'R') { reset(); render(); continue; }
        if (over) continue;
        if      (k == 0x11) { if (cr > 0) cr--; render(); }
        else if (k == 0x12) { if (cr < N - 1) cr++; render(); }
        else if (k == 0x13) { if (cc > 0) cc--; render(); }
        else if (k == 0x14) { if (cc < N - 1) cc++; render(); }
        else if (k == ' ' || k == '\n' || k == '\r') { fire(); render(); }
    }
    return 0;
}
