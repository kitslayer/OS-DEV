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
static const char *PUZZLE =
    "53..7...."
    "6..195..."
    ".98....6."
    "8...6...3"
    "4..8.3..1"
    "7...2...6"
    ".6....28."
    "...419..5"
    "....8..79";

static char cell[9][9];    /* current value, 0 = empty */
static char given[9][9];   /* 1 = a fixed clue */
static int  cx = 0, cy = 0;
static int  announced = 0; /* so the solved chime plays once */

static void load(void) {
    for (int r = 0; r < 9; r++) for (int c = 0; c < 9; c++) {
        char ch = PUZZLE[r * 9 + c];
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
    line[p] = 0; print(line); print("\n");

    for (int r = 0; r < 9; r++) {
        if (r % 3 == 0) { print(SEP); print("\n"); }
        p = 0; line[p++] = '|';
        for (int c = 0; c < 9; c++) {
            char d = cell[r][c] ? (char)('0' + cell[r][c]) : '.';
            char l = ' ', rt = ' ';
            if (r == cy && c == cx)      { l = '['; rt = ']'; }   /* cursor */
            else if (conflict(r, c))     { l = '('; rt = ')'; }   /* clashes */
            line[p++] = l; line[p++] = d; line[p++] = rt;
            if (c % 3 == 2) line[p++] = '|';
        }
        line[p] = 0; print(line); print("\n");
    }
    print(SEP); print("\n");
    print(" arrows move  1-9 set  0 clr  n new  q quit\n");
    return solved;
}

int main(void) {
    load();
    int solved = render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 27) return 0;
        if (k == 'n') { load(); render(); continue; }
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
