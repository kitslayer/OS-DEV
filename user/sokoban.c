/*
 * sokoban.c — Sokoban (crate-pushing puzzle).
 *
 * Push every crate ($) onto a goal (.). You can only push (never pull) one
 * crate at a time, and not into a wall or another crate, so think before you
 * shove. A crate on a goal shows as *. Clear all goals to finish the level.
 *
 * Arrows move/push, r restarts the level, n skips to the next, q quits.
 * The bundled levels use open rooms (no corner traps) so each is solvable.
 */
#include "ulib.h"

#define MAXW 16
#define MAXH 12

/* Tiles: # wall, . goal, $ crate, * crate-on-goal, @ player, + player-on-goal */
static const char *L1[] = { "######", "#@ $.#", "######", 0 };
static const char *L2[] = { "########", "#      #", "#@$  . #", "# $  . #", "#      #", "########", 0 };
static const char *L3[] = { "#######", "#   . #", "# $   #", "#  @  #", "#######", 0 };
static const char **levels[] = { L1, L2, L3 };
#define NLEV 3

static int wall[MAXH][MAXW], goal[MAXH][MAXW], box[MAXH][MAXW];
static int W, H, pr, pc, lev, won, pushes;

static void putn(int n) {
    char t[8]; int i = 0;
    if (n == 0) { print("0"); return; }
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    char s[8]; int j = 0; while (i) s[j++] = t[--i]; s[j] = 0; print(s);
}

static void load(int n) {
    const char **rows = levels[n];
    H = 0; W = 0;
    for (int r = 0; rows[r]; r++) {
        H++;
        int len = (int)ustrlen(rows[r]);
        if (len > W) W = len;
    }
    for (int r = 0; r < MAXH; r++) for (int c = 0; c < MAXW; c++) wall[r][c] = goal[r][c] = box[r][c] = 0;
    for (int r = 0; r < H; r++) {
        const char *s = rows[r];
        for (int c = 0; s[c]; c++) {
            switch (s[c]) {
            case '#': wall[r][c] = 1; break;
            case '.': goal[r][c] = 1; break;
            case '$': box[r][c] = 1; break;
            case '*': box[r][c] = goal[r][c] = 1; break;
            case '@': pr = r; pc = c; break;
            case '+': pr = r; pc = c; goal[r][c] = 1; break;
            default: break;
            }
        }
    }
    pushes = 0; won = 0;
}

static int solved(void) {
    for (int r = 0; r < H; r++) for (int c = 0; c < W; c++) if (box[r][c] && !goal[r][c]) return 0;
    return 1;
}

static void move(int dr, int dc) {
    int nr = pr + dr, nc = pc + dc;
    if (nr < 0 || nr >= H || nc < 0 || nc >= W || wall[nr][nc]) return;
    if (box[nr][nc]) {
        int br = nr + dr, bc = nc + dc;
        if (br < 0 || br >= H || bc < 0 || bc >= W || wall[br][bc] || box[br][bc]) return;
        box[nr][nc] = 0; box[br][bc] = 1; pushes++;
        if (goal[br][bc]) sys_beep(660, 25);
    }
    pr = nr; pc = nc;
    if (solved()) { won = 1; sys_beep(880, 120); sys_beep(1320, 140); }
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("\n  Sokoban"); sys_setcolor(0);
    print("    level "); putn(lev + 1); print("/"); putn(NLEV);
    print("   pushes "); putn(pushes); print("\n\n");
    for (int r = 0; r < H; r++) {
        print("    ");
        for (int c = 0; c < W; c++) {
            char ch; int col;
            if (wall[r][c])              { ch = '#'; col = 8; }
            else if (box[r][c] && goal[r][c]) { ch = '*'; col = 10; }
            else if (box[r][c])          { ch = '$'; col = 3; }
            else if (pr == r && pc == c) { ch = '@'; col = 11; }
            else if (goal[r][c])         { ch = '.'; col = 2; }
            else                          { ch = ' '; col = 0; }
            sys_setcolor(col);
            char s[2]; s[0] = ch; s[1] = 0; print(s);
        }
        sys_setcolor(0); print("\n");
    }
    print("\n  ");
    if (won) { sys_setcolor(10); print("Level solved!  (n = next level)"); sys_setcolor(0); }
    else print("arrows push   r restart   n next");
    print("\n  q quit\n");
}

int main(void) {
    lev = 0; load(lev); render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 'Q') break;
        if (k == 'r' || k == 'R') { load(lev); render(); continue; }
        if (k == 'n' || k == 'N') { lev = (lev + 1) % NLEV; load(lev); render(); continue; }
        if (won) continue;
        if      (k == 0x11) { move(-1, 0); render(); }
        else if (k == 0x12) { move( 1, 0); render(); }
        else if (k == 0x13) { move(0, -1); render(); }
        else if (k == 0x14) { move(0,  1); render(); }
    }
    return 0;
}
