/*
 * sudoku.c — a Sudoku puzzle, a userspace program.
 *
 * A logic puzzle to complement the arcade games: move the cursor with the arrow
 * keys, type 1-9 to fill the highlighted cell, 0/space to clear it. Clue cells
 * (the puzzle's givens) can't be edited. Cells that clash with another in the
 * same row, column, or 3x3 box are shown in (parens); the header counts them and
 * announces a solve. Runs ring-3 on the app text grid, like the other games.
 */
#include "ulib.h"

/* the classic example puzzle; '.' = blank */
/* a few puzzles; n cycles through them. Each givens-set is conflict-free
 * (the classic Wikipedia puzzle, Inkala's 2012 hard puzzle, and one carved from
 * a valid solved grid). '.' = blank. */
static const char *PUZZLES[] = {
    "53..7...." "6..195..." ".98....6." "8...6...3" "4..8.3..1" "7...2...6" ".6....28." "...419..5" "....8..79",
    "8........" "..36....." ".7..9.2.." ".5...7..." "....457.." "...1...3." "..1....68" "..85...1." ".9....4..",
    "5.4.7.9.2" ".7.1.5.4." "1.8.4.5.7" ".5.7.1.2." "4.6.5.7.1" ".1.9.4.5." "9.1.3.2.4" ".8.4.9.3." "3.5.8.1.9",
};
#define NPUZ ((int)(sizeof(PUZZLES) / sizeof(PUZZLES[0])))
static int cur_puzzle = 0;

static char cell[9][9];    /* current value, 0 = empty */
static char given[9][9];   /* 1 = a fixed clue */
static int  cx = 0, cy = 0;
static int  announced = 0; /* so the solved chime plays once */

static void load(void) {
    for (int r = 0; r < 9; r++) for (int c = 0; c < 9; c++) {
        char ch = PUZZLES[cur_puzzle][r * 9 + c];
        if (ch >= '1' && ch <= '9') { cell[r][c] = (char)(ch - '0'); given[r][c] = 1; }
        else { cell[r][c] = 0; given[r][c] = 0; }
    }
    cx = cy = 0; announced = 0;
}

/* does (r,c)'s value repeat in its row / column / 3x3 box?  (empty never clashes) */
static int conflict(int r, int c) {
    int v = cell[r][c]; if (!v) return 0;
    for (int i = 0; i < 9; i++) {
        if (i != c && cell[r][i] == v) return 1;
        if (i != r && cell[i][c] == v) return 1;
    }
    int br = (r / 3) * 3, bc = (c / 3) * 3;
    for (int dr = 0; dr < 3; dr++) for (int dc = 0; dc < 3; dc++) {
        int rr = br + dr, cc = bc + dc;
        if ((rr != r || cc != c) && cell[rr][cc] == v) return 1;
    }
    return 0;
}
static int count_conflicts(void) {
    int n = 0; for (int r = 0; r < 9; r++) for (int c = 0; c < 9; c++) if (conflict(r, c)) n++; return n;
}
static int filled(void) {
    for (int r = 0; r < 9; r++) for (int c = 0; c < 9; c++) if (!cell[r][c]) return 0;
    return 1;
}

static void puts_(char *buf, int *p, const char *s) { while (*s) buf[(*p)++] = *s++; }
static void putn_(char *buf, int *p, int v) {
    char t[8]; int i = 0; if (!v) t[i++] = '0'; while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) buf[(*p)++] = t[--i];
}

static const char *SEP = "+---------+---------+---------+";

/* returns 1 if the board is solved */
static int render(void) {
    sys_clear();
    int conf = count_conflicts();
    int solved = filled() && conf == 0;
    char line[48]; int p;

    p = 0; puts_(line, &p, "  Sudoku    ");
    if (solved)     puts_(line, &p, "** SOLVED! **");
    else          { puts_(line, &p, "conflicts: "); putn_(line, &p, conf); }
    line[p] = 0;
    sys_setcolor(solved ? 9 : 8);            /* header: lime when solved, else grey */
    print(line); print("\n");

    for (int r = 0; r < 9; r++) {
        if (r % 3 == 0) { sys_setcolor(8); print(SEP); print("\n"); }
        for (int c = 0; c < 9; c++) {
            if (c % 3 == 0) { sys_setcolor(8); print("|"); }
            char d = cell[r][c] ? (char)('0' + cell[r][c]) : '.';
            char l = ' ', rt = ' '; int col = 0;                       /* entry / empty: green */
            if (r == cy && c == cx)  { l = '['; rt = ']'; col = 4; }   /* cursor: cyan */
            else if (conflict(r, c)) { l = '('; rt = ')'; col = 2; }   /* clash: red */
            else if (given[r][c])    col = 1;                          /* clue: white */
            char cb[4]; cb[0] = l; cb[1] = d; cb[2] = rt; cb[3] = 0;
            sys_setcolor(col); print(cb);
        }
        sys_setcolor(8); print("|\n");
    }
    sys_setcolor(8); print(SEP); print("\n");
    sys_setcolor(0); print(" arrows move  1-9 set  0 clr  n new  q quit\n");
    return solved;
}

int main(void) {
    load();
    int solved = render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 27) return 0;
        if (k == 'n') { cur_puzzle = (cur_puzzle + 1) % NPUZ; load(); render(); continue; }   /* next puzzle */
        if      (k == 0x11) { if (cy > 0) cy--; }                 /* up    */
        else if (k == 0x12) { if (cy < 8) cy++; }                 /* down  */
        else if (k == 0x13) { if (cx > 0) cx--; }                 /* left  */
        else if (k == 0x14) { if (cx < 8) cx++; }                 /* right */
        else if (k >= '1' && k <= '9') {
            if (given[cy][cx]) sys_beep(220, 60);                 /* a clue is fixed */
            else cell[cy][cx] = (char)(k - '0');
        }
        else if (k == '0' || k == ' ' || k == 8 || k == 127) {    /* clear (0 / space / backspace) */
            if (given[cy][cx]) sys_beep(220, 60);
            else cell[cy][cx] = 0;
        }
        else continue;
        solved = render();
        if (solved) { if (!announced) { sys_beep(660, 90); sys_beep(880, 140); announced = 1; } }
        else announced = 0;
    }
}
