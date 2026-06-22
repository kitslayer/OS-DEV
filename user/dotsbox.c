/*
 * dotsbox.c — Dots and Boxes against the computer.
 *
 * Take turns drawing the edges between dots. Complete the fourth side of a box
 * and you claim it (Y) and move again; otherwise play passes to the CPU (C).
 * When every edge is drawn, whoever owns more boxes wins.
 *
 * The board is a (2N+1)x(2N+1) grid: dots at even/even, horizontal edges at
 * even/odd, vertical edges at odd/even, box centres at odd/odd. Move the cursor
 * (+) with the arrows and press Space to draw the edge under it. r restarts,
 * q quits.
 */
#include "ulib.h"

#define N 4                 /* boxes per side */
#define G (2*N+1)           /* grid dimension (9) */

static char g[G][G];        /* edges: 0/1 drawn; box centres: 0 none, 1 you, 2 cpu */
static int  cr, cc;         /* cursor in grid coordinates */
static int  sy, sc, over;
static const char *msg;

static int is_edge(int r, int c) { return (r & 1) != (c & 1); }

static void reset(void) {
    for (int r = 0; r < G; r++) for (int c = 0; c < G; c++) g[r][c] = 0;
    cr = 0; cc = 1; sy = sc = 0; over = 0; msg = "Your move: draw an edge.";
}

static int box_edges(int br, int bc) {      /* drawn sides of box (br,bc) */
    int r = 2*br + 1, c = 2*bc + 1, n = 0;
    if (g[r-1][c]) n++;
    if (g[r+1][c]) n++;
    if (g[r][c-1]) n++;
    if (g[r][c+1]) n++;
    return n;
}
static int is_side(int er, int ec, int br, int bc) {
    int r = 2*br + 1, c = 2*bc + 1;
    return (er == r-1 && ec == c) || (er == r+1 && ec == c) ||
           (er == r && ec == c-1) || (er == r && ec == c+1);
}
/* Claim any boxes the just-drawn edge (er,ec) completes for player p; count them. */
static int claim(int er, int ec, int p) {
    int got = 0;
    for (int br = 0; br < N; br++) for (int bc = 0; bc < N; bc++) {
        if (is_side(er, ec, br, bc) && g[2*br+1][2*bc+1] == 0 && box_edges(br, bc) == 4) {
            g[2*br+1][2*bc+1] = (char)p; got++; if (p == 1) sy++; else sc++;
        }
    }
    return got;
}
static int edges_left(void) {
    for (int r = 0; r < G; r++) for (int c = 0; c < G; c++)
        if (is_edge(r, c) && !g[r][c]) return 1;
    return 0;
}
static void finish(void) {
    over = 1;
    msg = (sy > sc) ? "All drawn - you win!" : (sy < sc) ? "All drawn - CPU wins." : "All drawn - a tie.";
}
/* Would drawing (er,ec) hand a box away (leave an adjacent box with 3 sides)? */
static int unsafe(int er, int ec) {
    g[er][ec] = 1; int bad = 0;
    for (int br = 0; br < N && !bad; br++) for (int bc = 0; bc < N && !bad; bc++)
        if (is_side(er, ec, br, bc) && box_edges(br, bc) == 3) bad = 1;
    g[er][ec] = 0;
    return bad;
}
/* Does drawing (er,ec) complete a box right now? */
static int completes(int er, int ec) {
    g[er][ec] = 1; int yes = 0;
    for (int br = 0; br < N && !yes; br++) for (int bc = 0; bc < N && !yes; bc++)
        if (is_side(er, ec, br, bc) && box_edges(br, bc) == 4) yes = 1;
    g[er][ec] = 0;
    return yes;
}
/* CPU: take a completing move (and go again), else a safe edge, else any edge. */
static void cpu_turn(void) {
    for (;;) {
        if (over) return;
        int er = -1, ec = -1;
        for (int r = 0; r < G && er < 0; r++) for (int c = 0; c < G && er < 0; c++)
            if (is_edge(r, c) && !g[r][c] && completes(r, c)) { er = r; ec = c; }
        if (er < 0)
            for (int r = 0; r < G && er < 0; r++) for (int c = 0; c < G && er < 0; c++)
                if (is_edge(r, c) && !g[r][c] && !unsafe(r, c)) { er = r; ec = c; }
        if (er < 0)
            for (int r = 0; r < G && er < 0; r++) for (int c = 0; c < G && er < 0; c++)
                if (is_edge(r, c) && !g[r][c]) { er = r; ec = c; }
        if (er < 0) return;
        g[er][ec] = 1; int got = claim(er, ec, 2); sys_beep(330, 40);
        if (!edges_left()) { finish(); return; }
        if (!got) { msg = "Your move: draw an edge."; return; }
        msg = "CPU claimed a box - again...";
    }
}

static void putn(int v) {
    char t[6]; int i = 0; if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    char s[6]; int j = 0; while (i) s[j++] = t[--i]; s[j] = 0; print(s);
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("  Dots and Boxes"); sys_setcolor(0);
    print("   you "); sys_setcolor(2); putn(sy);
    sys_setcolor(0); print("  cpu "); sys_setcolor(12); putn(sc);
    sys_setcolor(0); print("\n\n");
    for (int r = 0; r < G; r++) {
        print("   ");
        for (int c = 0; c < G; c++) {
            int cur = (r == cr && c == cc);
            char ch;
            if (!(r & 1) && !(c & 1)) ch = '.';                 /* dot */
            else if (is_edge(r, c))   ch = g[r][c] ? ((r & 1) ? '|' : '-') : ' ';
            else                      ch = g[r][c] == 1 ? 'Y' : g[r][c] == 2 ? 'C' : ' ';   /* box */
            if (cur) { sys_setcolor(14); if (ch == ' ') ch = '+'; }
            else if (ch == 'Y') sys_setcolor(2);
            else if (ch == 'C') sys_setcolor(12);
            else if (ch == '.') sys_setcolor(8);
            else sys_setcolor(7);
            char s[2]; s[0] = ch; s[1] = 0; print(s);
            sys_setcolor(0); print(" ");
        }
        print("\n");
    }
    print("\n  ");
    sys_setcolor(over ? 2 : 0); print(msg); sys_setcolor(0);
    print("\n  arrows move  space draw  r q\n");
}

int main(void) {
    reset();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 'Q') return 0;
        if (k == 'r' || k == 'R') { reset(); render(); continue; }
        if (over) continue;
        if      (k == 0x11 && cr > 0)     cr--;
        else if (k == 0x12 && cr < G - 1) cr++;
        else if (k == 0x13 && cc > 0)     cc--;
        else if (k == 0x14 && cc < G - 1) cc++;
        else if (k == ' ') {
            if (is_edge(cr, cc) && !g[cr][cc]) {
                g[cr][cc] = 1; int got = claim(cr, cc, 1); sys_beep(660, 40);
                if (!edges_left()) finish();
                else if (got) msg = "You claimed a box - go again!";
                else { sys_sleep(150); cpu_turn(); }
            } else sys_beep(160, 80);   /* not an edge, or already drawn */
        }
        render();
    }
}
