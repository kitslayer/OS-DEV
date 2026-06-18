/*
 * reversi.c — Reversi / Othello against an AI.
 *
 * The classic 8x8 disc-flipping game. You are O and move first; the AI is X.
 * Place a disc so it brackets one or more of the AI's discs in a straight line
 * (any of 8 directions) between the new disc and another of yours — every
 * bracketed disc flips to your colour. No legal move means you pass. When
 * neither side can move, the majority of discs wins. The AI plays a positional
 * greedy strategy (grab corners, avoid the squares next to them), which makes
 * for a genuine fight without a deep search.
 *
 * Arrows move the cursor, Space/Enter places. Legal moves are shown as '*'.
 */
#include "ulib.h"

#define N 8
static int b[N][N];                 /* 0 empty, 1 you (O), 2 AI (X) */
static int cr = 2, cc = 3;          /* cursor */
static int over;
static const char *msg;

/* Standard Othello positional weights: corners are gold, the cells orthogonally
 * and diagonally adjacent to a corner are traps (they hand the corner away). */
static const int wt[N][N] = {
    { 120, -20, 20,  5,  5, 20, -20, 120 },
    { -20, -40, -5, -5, -5, -5, -40, -20 },
    {  20,  -5, 15,  3,  3, 15,  -5,  20 },
    {   5,  -5,  3,  3,  3,  3,  -5,   5 },
    {   5,  -5,  3,  3,  3,  3,  -5,   5 },
    {  20,  -5, 15,  3,  3, 15,  -5,  20 },
    { -20, -40, -5, -5, -5, -5, -40, -20 },
    { 120, -20, 20,  5,  5, 20, -20, 120 },
};
static const int dr[8] = { -1,-1,-1, 0,0, 1,1,1 };
static const int dc[8] = { -1, 0, 1,-1,1,-1,0,1 };

static int opp(int p) { return p == 1 ? 2 : 1; }

/* discs that would flip if p plays (r,c), summed over all 8 directions */
static int gain(int r, int c, int p) {
    if (b[r][c]) return 0;
    int total = 0;
    for (int d = 0; d < 8; d++) {
        int rr = r + dr[d], cc2 = c + dc[d], run = 0;
        while (rr >= 0 && rr < N && cc2 >= 0 && cc2 < N && b[rr][cc2] == opp(p)) {
            run++; rr += dr[d]; cc2 += dc[d];
        }
        if (run && rr >= 0 && rr < N && cc2 >= 0 && cc2 < N && b[rr][cc2] == p) total += run;
    }
    return total;
}

static void place(int r, int c, int p) {     /* assumes legal: lay the disc + flip */
    b[r][c] = p;
    for (int d = 0; d < 8; d++) {
        int rr = r + dr[d], cc2 = c + dc[d], run = 0;
        while (rr >= 0 && rr < N && cc2 >= 0 && cc2 < N && b[rr][cc2] == opp(p)) {
            run++; rr += dr[d]; cc2 += dc[d];
        }
        if (run && rr >= 0 && rr < N && cc2 >= 0 && cc2 < N && b[rr][cc2] == p) {
            rr = r + dr[d]; cc2 = c + dc[d];
            while (run--) { b[rr][cc2] = p; rr += dr[d]; cc2 += dc[d]; }
        }
    }
}

static int has_move(int p) {
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) if (gain(r, c, p)) return 1;
    return 0;
}

static void counts(int *you, int *ai) {
    *you = *ai = 0;
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) {
        if (b[r][c] == 1) (*you)++; else if (b[r][c] == 2) (*ai)++;
    }
}

static void putn(int n) {
    char t[8]; int i = 0;
    if (n == 0) { print("0"); return; }
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    char s[8]; int j = 0; while (i) s[j++] = t[--i]; s[j] = 0; print(s);
}

/* the AI's best move by positional weight (corner-seeking, trap-avoiding) */
static void ai_play(void) {
    int br = -1, bc = -1, best = -100000;
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) {
        if (!gain(r, c, 2)) continue;
        int score = wt[r][c] + gain(r, c, 2);     /* prefer good squares, break ties by flips */
        if (score > best) { best = score; br = r; bc = c; }
    }
    if (br >= 0) place(br, bc, 2);
}

/* hand turns back and forth, passing a side with no move, until it's the
 * human's move again or the game ends */
static void settle(void) {
    for (;;) {
        if (!has_move(1) && !has_move(2)) {
            over = 1;
            int you, ai; counts(&you, &ai);
            msg = you > ai ? "You WIN!  (r to replay)" :
                  ai > you ? "AI wins.  (r to replay)" : "Draw.  (r to replay)";
            sys_beep(you >= ai ? 880 : 196, 180);
            return;
        }
        if (has_move(2)) { ai_play(); sys_beep(440, 30); }
        else             { msg = "AI passes - your move"; return; }
        if (has_move(1)) { msg = "Your move (* = legal)"; return; }
        /* you have no move: pass back to the AI and loop */
        msg = "No move - you pass";
    }
}

static void render(void) {
    sys_clear();
    int you, ai; counts(&you, &ai);
    sys_setcolor(4); print("\n  Reversi"); sys_setcolor(0);
    print("    O="); sys_setcolor(2); putn(you); sys_setcolor(0);
    print("  X="); sys_setcolor(3); putn(ai); sys_setcolor(0); print("\n\n");
    for (int r = 0; r < N; r++) {
        print("   ");
        for (int c = 0; c < N; c++) {
            int v = b[r][c];
            char ch = v == 1 ? 'O' : v == 2 ? 'X' : (!over && gain(r, c, 1) ? '*' : '.');
            sys_setcolor(v == 1 ? 2 : v == 2 ? 3 : ch == '*' ? 10 : 8);
            char cell[4];
            int cur = (r == cr && c == cc);
            cell[0] = cur ? '[' : ' '; cell[1] = ch; cell[2] = cur ? ']' : ' '; cell[3] = 0;
            print(cell);
        }
        sys_setcolor(0); print("\n");
    }
    print("\n  "); print(msg);
    print("\n  arrows move  space place  r reset  q quit\n");
}

static void reset(void) {
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) b[r][c] = 0;
    b[3][3] = b[4][4] = 2; b[3][4] = b[4][3] = 1;     /* standard opening */
    cr = 2; cc = 3; over = 0; msg = "Your move (* = legal)";
}

int main(void) {
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
        else if (k == ' ' || k == '\n' || k == '\r') {
            if (gain(cr, cc, 1)) { place(cr, cc, 1); settle(); render(); }
        }
    }
    return 0;
}
