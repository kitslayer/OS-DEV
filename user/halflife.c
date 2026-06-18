/*
 * halflife.c — "Half-Life: Black Mesa", a tribute survival shooter.
 *
 * NOT the real game (Valve's Half-Life is a closed GoldSrc/Win32 title that
 * needs a GPU and proprietary assets — it can't run here; Quake is this OS's
 * GoldSrc-family ceiling). This is an original homage built on the OS's own
 * text grid: after the resonance cascade, you're a HEV-suited scientist trying
 * to survive Black Mesa. Headcrabs ('c') swarm in from the edges and chase you;
 * shoot them before they drain your suit.
 *
 * Arrows move (and aim). Space fires in your facing direction. Survive; each
 * kill scores. q quits, r restarts after a flatline.
 */
#include "ulib.h"

#define W 40
#define H 11
#define MAXC 40
#define MAXB 12

static int px, py, dx, dy;          /* player + facing */
static int hx[MAXC], hy[MAXC], nh;
static int bx[MAXB], by[MAXB], bdx[MAXB], bdy[MAXB], nb;
static int hp, score, over;
static unsigned long t_enemy, t_spawn, last;

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void putn(int n) {
    char t[8]; int i = 0;
    if (n == 0) { print("0"); return; }
    if (n < 0) { print("-"); n = -n; }
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    char s[8]; int j = 0; while (i) s[j++] = t[--i]; s[j] = 0; print(s);
}

static void reset(void) {
    px = W / 2; py = H / 2; dx = 0; dy = -1;
    nh = nb = 0; hp = 100; score = 0; over = 0;
    last = sys_uptime_ms(); t_enemy = t_spawn = last;
}

static void spawn(void) {
    if (nh >= MAXC) return;
    int e = (int)(rnd() & 3), x, y;
    if (e == 0) { x = 0; y = (int)(rnd() % H); }
    else if (e == 1) { x = W - 1; y = (int)(rnd() % H); }
    else if (e == 2) { x = (int)(rnd() % W); y = 0; }
    else { x = (int)(rnd() % W); y = H - 1; }
    hx[nh] = x; hy[nh] = y; nh++;
}

static void kill_crab(int i) { nh--; hx[i] = hx[nh]; hy[i] = hy[nh]; }
static void kill_bullet(int i) { nb--; bx[i] = bx[nb]; by[i] = by[nb]; bdx[i] = bdx[nb]; bdy[i] = bdy[nb]; }

static void hurt(void) {
    hp -= 20; sys_beep(160, 80);
    if (hp <= 0) { hp = 0; over = 1; sys_beep(110, 400); }
}

static void enemy_step(void) {
    for (int i = 0; i < nh; i++) {
        int ddx = px - hx[i], ddy = py - hy[i];
        int ax = ddx < 0 ? -ddx : ddx, ay = ddy < 0 ? -ddy : ddy;
        if (ax >= ay) hx[i] += ddx > 0 ? 1 : -1;     /* close the larger gap */
        else          hy[i] += ddy > 0 ? 1 : -1;
        if (hx[i] == px && hy[i] == py) { kill_crab(i); i--; hurt(); }   /* reached you */
    }
}

static void bullet_step(void) {
    for (int i = 0; i < nb; i++) {
        for (int step = 0; step < 2; step++) {        /* bullets are fast */
            bx[i] += bdx[i]; by[i] += bdy[i];
            int hit = -1;
            for (int j = 0; j < nh; j++) if (hx[j] == bx[i] && hy[j] == by[i]) { hit = j; break; }
            if (hit >= 0) { kill_crab(hit); score++; sys_beep(990, 25); kill_bullet(i); i--; goto next; }
            if (bx[i] < 0 || bx[i] >= W || by[i] < 0 || by[i] >= H) { kill_bullet(i); i--; goto next; }
        }
        next:;
    }
}

static void fire(void) {
    if (nb >= MAXB || (dx == 0 && dy == 0)) return;
    bx[nb] = px + dx; by[nb] = py + dy; bdx[nb] = dx; bdy[nb] = dy; nb++;
    sys_beep(1320, 15);
}

static void move(int ndx, int ndy) {
    dx = ndx; dy = ndy;
    int nx = px + ndx, ny = py + ndy;
    if (nx < 0 || nx >= W || ny < 0 || ny >= H) return;
    px = nx; py = ny;
    for (int j = 0; j < nh; j++) if (hx[j] == px && hy[j] == py) { kill_crab(j); j--; hurt(); }  /* walked into a crab */
}

static char field[H][W + 1];
static void render(void) {
    sys_clear();
    sys_setcolor(14); print("  BLACK MESA"); sys_setcolor(0); print("  HEV ");
    sys_setcolor(hp > 40 ? 10 : 2); print("[");
    for (int i = 0; i < 10; i++) print(i < (hp + 9) / 10 ? "|" : " ");
    print("] "); putn(hp); print("%"); sys_setcolor(0);
    print("  kills "); sys_setcolor(3); putn(score); sys_setcolor(0); print("\n\n");

    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) field[y][x] = ' ';
    for (int y = 0; y < H; y++) field[y][W] = 0;
    for (int i = 0; i < nb; i++) if (bx[i] >= 0 && bx[i] < W && by[i] >= 0 && by[i] < H) field[by[i]][bx[i]] = '.';
    for (int i = 0; i < nh; i++) field[hy[i]][hx[i]] = 'c';
    field[py][px] = over ? 'X' : '@';

    for (int y = 0; y < H; y++) {
        print("  ");
        for (int x = 0; x < W; x++) {
            char c = field[y][x];
            sys_setcolor(c == '@' ? 11 : c == 'c' ? 2 : c == '.' ? 14 : c == 'X' ? 4 : 8);
            if (c == ' ') print(" ");
            else { char s[2]; s[0] = c; s[1] = 0; print(s); }
        }
        print("\n");
    }
    print("\n  ");
    if (over) { sys_setcolor(2); print("FLATLINE. Score "); putn(score); print(".  r restart  q quit"); sys_setcolor(0); }
    else { sys_setcolor(0); print("arrows move/aim  space fire  q quit"); }
    print("\n");
}

int main(void) {
    rng = (unsigned)sys_uptime_ms() | 1u;
    reset();
    render();
    for (;;) {
        int k, moved = 0;
        while ((k = sys_pollkey()) >= 0) {
            if (k == 'q' || k == 'Q') return 0;
            if (over) { if (k == 'r' || k == 'R') { reset(); render(); } continue; }
            if      (k == 0x11) { move(0, -1); moved = 1; }
            else if (k == 0x12) { move(0,  1); moved = 1; }
            else if (k == 0x13) { move(-1, 0); moved = 1; }
            else if (k == 0x14) { move( 1, 0); moved = 1; }
            else if (k == ' ')  { fire(); moved = 1; }
        }
        if (!over) {
            unsigned long now = sys_uptime_ms();
            bullet_step();
            int ep = 380 - score * 4; if (ep < 140) ep = 140;       /* enemies speed up */
            int sp = 1400 - score * 15; if (sp < 450) sp = 450;     /* and arrive faster */
            if (now - t_enemy >= (unsigned long)ep) { enemy_step(); t_enemy = now; }
            if (now - t_spawn >= (unsigned long)sp) { spawn(); t_spawn = now; }
            render();
        } else if (moved) render();
        sys_sleep(45);
    }
}
