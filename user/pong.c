/*
 * pong.c — Pong against the computer.
 *
 * You are the left paddle; the CPU is the right. Move with the Up/Down arrows
 * (or W/S). The ball speeds along, bouncing off the top and bottom walls and
 * the paddles; miss it and your opponent scores. First to 7 wins. A real-time
 * game (like Snake): non-blocking input each frame, then a short sleep to pace.
 */
#include "ulib.h"

#define W   40
#define H   16
#define PAD 4                       /* paddle height */
#define WINSCORE 7

static int py, ay;                  /* paddle tops */
static int bx, by, vx, vy;          /* ball pos + velocity */
static int ps, as;                  /* scores */
static int over;

static void putn(int n) {
    char t[8]; int i = 0;
    if (n == 0) { print("0"); return; }
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    char s[8]; int j = 0; while (i) s[j++] = t[--i]; s[j] = 0; print(s);
}

static void serve(int dir) {        /* launch the ball toward dir (-1 left / +1 right) */
    bx = W / 2; by = H / 2; vx = dir; vy = (bx + by) & 1 ? 1 : -1;
}

static void reset(void) {
    py = ay = (H - PAD) / 2; ps = as = 0; over = 0; serve(1);
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("\n  Pong"); sys_setcolor(0);
    print("     you "); sys_setcolor(2); putn(ps); sys_setcolor(0);
    print("  -  "); sys_setcolor(3); putn(as); sys_setcolor(0); print(" cpu\n\n");
    for (int y = 0; y < H; y++) {
        print("  ");
        char line[W + 2]; int p = 0;
        for (int x = 0; x < W; x++) {
            char ch = ' ';
            if (x == 0 && y >= py && y < py + PAD) ch = '|';
            else if (x == W - 1 && y >= ay && y < ay + PAD) ch = '|';
            else if (x == bx && y == by) ch = 'O';
            else if (y == 0 || y == H - 1) ch = '-';
            else if (x == W / 2) ch = ':';
            line[p++] = ch;
        }
        line[p] = 0;
        print(line); print("\n");
    }
    print("\n  ");
    if (over) { sys_setcolor(10); print(ps > as ? "You WIN!  (r replay)" : "CPU wins.  (r replay)"); sys_setcolor(0); }
    else print("up/down move   r reset   q quit");
    print("\n");
}

static void step(void) {
    /* CPU tracks the ball, one cell per frame (so it's beatable) */
    int amid = ay + PAD / 2;
    if (amid < by && ay + PAD < H - 1) ay++;
    else if (amid > by && ay > 1) ay--;

    int nx = bx + vx, ny = by + vy;
    if (ny <= 1) { ny = 1; vy = 1; }
    if (ny >= H - 2) { ny = H - 2; vy = -1; }

    if (nx <= 1) {                                  /* left paddle plane */
        if (ny >= py && ny < py + PAD) { nx = 1; vx = 1; vy += (ny - (py + PAD/2)) > 0 ? 1 : (ny - (py + PAD/2)) < 0 ? -1 : 0; if (vy>1) vy=1; if (vy<-1) vy=-1; sys_beep(660,15); }
        else { as++; if (as >= WINSCORE) { over = 1; sys_beep(196,250); } else serve(1); return; }
    } else if (nx >= W - 2) {                        /* right paddle plane */
        if (ny >= ay && ny < ay + PAD) { nx = W - 2; vx = -1; sys_beep(660,15); }
        else { ps++; if (ps >= WINSCORE) { over = 1; sys_beep(1046,180); } else serve(-1); return; }
    }
    bx = nx; by = ny;
}

int main(void) {
    reset();
    render();
    for (;;) {
        int k;
        while ((k = sys_pollkey()) >= 0) {
            if (k == 'q' || k == 'Q') return 0;
            if (k == 'r' || k == 'R') { reset(); render(); }
            else if (k == 0x11 || k == 'w' || k == 'W') { if (py > 1) py--; }
            else if (k == 0x12 || k == 's' || k == 'S') { if (py + PAD < H - 1) py++; }
        }
        if (!over) { step(); render(); }
        sys_sleep(70);
    }
}
