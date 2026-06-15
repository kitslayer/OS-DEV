/*
 * c4.c — Connect Four against an AI.
 *
 * Drop discs into a 7-wide, 6-tall grid with the number keys 1-7; they fall to
 * the lowest empty cell. First to line up four (horizontal, vertical, or either
 * diagonal) wins. You are O; the AI is X and plays a 1-ply strategy: take a
 * winning drop if it has one, otherwise block yours, otherwise favour the
 * centre. A small strategic game to complement Tic-Tac-Toe's trivial board.
 */
#include "ulib.h"

#define W 7
#define H 6

static int b[H][W];                 /* 0 empty, 1 you (O), 2 AI (X) */
static int over;
static const char *msg;

static int drop(int c, int p) {     /* place p in column c; return the row, or -1 if full */
    if (c < 0 || c >= W) return -1;
    for (int r = H - 1; r >= 0; r--) if (b[r][c] == 0) { b[r][c] = p; return r; }
    return -1;
}
static int full(void) { for (int c = 0; c < W; c++) if (b[0][c] == 0) return 0; return 1; }

static int check4(int p) {          /* any four-in-a-row for p? */
    for (int r = 0; r < H; r++) for (int c = 0; c < W; c++) {
        if (c + 3 < W && b[r][c]==p && b[r][c+1]==p && b[r][c+2]==p && b[r][c+3]==p) return 1;
        if (r + 3 < H && b[r][c]==p && b[r+1][c]==p && b[r+2][c]==p && b[r+3][c]==p) return 1;
        if (r + 3 < H && c + 3 < W && b[r][c]==p && b[r+1][c+1]==p && b[r+2][c+2]==p && b[r+3][c+3]==p) return 1;
        if (r + 3 < H && c - 3 >= 0 && b[r][c]==p && b[r+1][c-1]==p && b[r+2][c-2]==p && b[r+3][c-3]==p) return 1;
    }
    return 0;
}

static int ai_move(void) {          /* the column the AI should play */
    for (int c = 0; c < W; c++) { int r = drop(c, 2); if (r >= 0) { int w = check4(2); b[r][c] = 0; if (w) return c; } }  /* win now */
    for (int c = 0; c < W; c++) { int r = drop(c, 1); if (r >= 0) { int w = check4(1); b[r][c] = 0; if (w) return c; } }  /* block you */
    static const int order[W] = { 3, 2, 4, 1, 5, 0, 6 };                          /* else favour the centre */
    for (int i = 0; i < W; i++) if (b[0][order[i]] == 0) return order[i];
    return -1;
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("\n  Connect Four"); sys_setcolor(0); print("    you=O   AI=X\n\n");
    for (int r = 0; r < H; r++) {
        print("   ");
        for (int c = 0; c < W; c++) {
            int v = b[r][c];
            sys_setcolor(v == 1 ? 2 : (v == 2 ? 3 : 8));      /* O red, X yellow, . grey */
            print(v == 1 ? " O " : (v == 2 ? " X " : " . "));
        }
        sys_setcolor(0); print("\n");
    }
    sys_setcolor(8); print("    1  2  3  4  5  6  7\n"); sys_setcolor(0);
    print("\n  "); print(msg);
    print("\n  1-7 drop   r reset   q quit\n");
}

static void reset(void) {
    for (int r = 0; r < H; r++) for (int c = 0; c < W; c++) b[r][c] = 0;
    over = 0; msg = "Your move (drop 1-7)";
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
        if (k >= '1' && k <= '7') {
            int c = k - '1';
            if (drop(c, 1) < 0) continue;                      /* column full */
            if (check4(1)) { over = 1; sys_beep(880, 150); msg = "You WIN!   (r to replay)"; render(); continue; }
            if (full())    { over = 1; msg = "Draw.   (r to replay)"; render(); continue; }
            int ac = ai_move();
            if (ac >= 0) {
                drop(ac, 2);
                if (check4(2)) { over = 1; sys_beep(196, 220); msg = "AI wins.   (r to replay)"; }
                else if (full()) { over = 1; msg = "Draw.   (r to replay)"; }
                else msg = "Your move (drop 1-7)";
            }
            render();
        }
    }
    return 0;
}
