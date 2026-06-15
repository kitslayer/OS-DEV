/*
 * ttt.c — Tic-Tac-Toe against an UNBEATABLE AI.
 *
 * A different genre from the rest of the app suite (action games, puzzles): an
 * adversarial turn-based game whose opponent plays PERFECTLY via the classic
 * minimax algorithm — full game-tree search (the board is tiny, so no depth
 * limit needed), scoring a win sooner as better. You play X with the number
 * keys 1-9 (phone-keypad layout); the AI plays O and never loses, so the best
 * you can do is force a draw. A small, self-contained showcase of a real game
 * AI running as an isolated ring-3 program.
 */
#include "ulib.h"

static char bd[9];                       /* ' ' empty, 'X' player, 'O' the AI */
static int wins, draws, losses;          /* session record (persists across 'r') */

/* The 8 winning lines (3 rows, 3 cols, 2 diagonals). */
static const int LINES[8][3] = {
    {0,1,2},{3,4,5},{6,7,8}, {0,3,6},{1,4,7},{2,5,8}, {0,4,8},{2,4,6}
};

static char winner(void) {               /* 'X' / 'O' if a line is taken, else 0 */
    for (int i = 0; i < 8; i++) {
        char a = bd[LINES[i][0]];
        if (a != ' ' && a == bd[LINES[i][1]] && a == bd[LINES[i][2]]) return a;
    }
    return 0;
}
static int full(void) { for (int i = 0; i < 9; i++) if (bd[i] == ' ') return 0; return 1; }

/* Minimax. o_turn = it is O's (the AI's) move. Returns the value of the
 * position to O: +ve good for O, -ve good for X; a faster win scores higher
 * (10-depth) so the AI finishes quickly and stalls a loss as long as it can. */
static int minimax(int o_turn, int depth) {
    char w = winner();
    if (w == 'O') return 10 - depth;
    if (w == 'X') return depth - 10;
    if (full()) return 0;
    int best = o_turn ? -100 : 100;
    for (int i = 0; i < 9; i++) if (bd[i] == ' ') {
        bd[i] = o_turn ? 'O' : 'X';
        int s = minimax(!o_turn, depth + 1);
        bd[i] = ' ';
        if (o_turn) { if (s > best) best = s; }
        else        { if (s < best) best = s; }
    }
    return best;
}
static int ai_move(void) {               /* the best empty cell for O, or -1 */
    int best = -1000, cell = -1;
    for (int i = 0; i < 9; i++) if (bd[i] == ' ') {
        bd[i] = 'O';
        int s = minimax(0, 1);           /* after O plays, it is X's move */
        bd[i] = ' ';
        if (s > best) { best = s; cell = i; }
    }
    return cell;
}

static void printnum(int v) {            /* small non-negative int -> screen */
    char t[12]; int i = 0, n = v;
    if (n == 0) t[i++] = '0';
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    char o[12]; int j = 0;
    while (i) o[j++] = t[--i];
    o[j] = 0;
    print(o);
}

static void put_cell(int i) {
    if (bd[i] == 'X')      { sys_setcolor(3); print(" X "); sys_setcolor(0); }   /* yellow */
    else if (bd[i] == 'O') { sys_setcolor(2); print(" O "); sys_setcolor(0); }   /* red    */
    else { sys_setcolor(8); char s[4] = { ' ', (char)('1' + i), ' ', 0 }; print(s); sys_setcolor(0); }
}

static void draw(const char *msg) {
    sys_clear();
    sys_setcolor(4); print("\n  Tic-Tac-Toe"); sys_setcolor(0); print("    you=X   AI=O\n\n");
    for (int r = 0; r < 3; r++) {
        print("       ");
        put_cell(r*3+0); print("|"); put_cell(r*3+1); print("|"); put_cell(r*3+2); print("\n");
        if (r < 2) print("       ---+---+---\n");
    }
    print("\n  won "); printnum(wins);
    print("   drew "); printnum(draws);
    print("   lost "); printnum(losses);
    print("\n  "); print(msg);
    print("\n  1-9 play    r reset    q quit\n");
}

int main(void) {
    for (int i = 0; i < 9; i++) bd[i] = ' ';
    wins = draws = losses = 0;
    draw("Your move!");
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }        /* no key (iq_get returns -1): yield */
        if (k == 'q' || k == 'Q') break;
        if (k == 'r' || k == 'R') {
            for (int i = 0; i < 9; i++) bd[i] = ' ';
            draw("New game - your move!");
            continue;
        }
        if (k >= '1' && k <= '9') {
            if (winner() || full()) continue;          /* game over: wait for reset */
            int c = k - '1';
            if (bd[c] != ' ') continue;                /* cell taken */
            bd[c] = 'X';
            if (winner() == 'X') { wins++;  sys_beep(880, 120); draw("You WIN!   (r to replay)"); continue; }
            if (full())          { draws++;                  draw("Draw.   (r to replay)");     continue; }
            int m = ai_move();
            if (m >= 0) bd[m] = 'O';
            if (winner() == 'O') { losses++; sys_beep(196, 220); draw("AI wins.   (r to replay)"); continue; }
            if (full())          { draws++;                  draw("Draw.   (r to replay)");     continue; }
            draw("Your move!");
        }
    }
    return 0;
}
