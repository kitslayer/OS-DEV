/*
 * spaceinv.c — Space Invaders.
 *
 * A block of invaders marches left and right, dropping a row and reversing each
 * time it reaches an edge, and speeding up as you thin them out. Slide your
 * cannon along the bottom and shoot them all before they reach you (or pick you
 * off with their own shots). Three lives.
 *
 * Left/right move, Space fires, r restarts, q quits.
 */
#include "ulib.h"

#define W 40
#define H 12                        /* field rows; header+field+footer must fit the 17-row grid */
#define IR 3
#define IC 8

static int alive[IR][IC], nalive;
static int bx, by, bdir;            /* invader block top-left + direction */
static int px;                      /* cannon x (on row H-1) */
static int pbx, pby, pbon;          /* player bullet */
static int ibx[6], iby[6], ibn;     /* invader bullets */
static int score, lives, over, won;
static int hi, saved;               /* persisted high score (SPACEINV.HI) */
static unsigned long t_inv, t_fire;

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void putn(int n) {
    char t[8]; int i = 0;
    if (n == 0) { print("0"); return; }
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    char s[8]; int j = 0; while (i) s[j++] = t[--i]; s[j] = 0; print(s);
}

static void load_hi(void) {
    char b[16]; long n = sys_readfile("SPACEINV.HI", b, sizeof(b) - 1);
    hi = 0; for (long i = 0; i < n; i++) { if (b[i] < '0' || b[i] > '9') break; hi = hi * 10 + (b[i] - '0'); }
}
static void save_hi(void) {
    char t[12], b[12]; int i = 0, n = 0, v = hi;
    if (v == 0) t[i++] = '0'; while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) b[n++] = t[--i];
    sys_writefile("SPACEINV.HI", b, (unsigned long)n);
}

static void reset(void) {
    for (int r = 0; r < IR; r++) for (int c = 0; c < IC; c++) alive[r][c] = 1;
    nalive = IR * IC;
    bx = 2; by = 1; bdir = 1; px = W/2; pbon = 0; ibn = 0;
    score = 0; lives = 3; over = 0; won = 0; saved = 0;
    t_inv = t_fire = sys_uptime_ms();
}

static int inv_x(int c) { return bx + c * 2; }
static int inv_y(int r) { return by + r; }

static void step_invaders(void) {
    int minx = 999, maxx = -1, maxy = -1;
    for (int r = 0; r < IR; r++) for (int c = 0; c < IC; c++) if (alive[r][c]) {
        int x = inv_x(c), y = inv_y(r);
        if (x < minx) minx = x; if (x > maxx) maxx = x; if (y > maxy) maxy = y;
    }
    if (nalive == 0) return;
    if ((bdir > 0 && maxx + 1 >= W - 1) || (bdir < 0 && minx - 1 <= 0)) { by++; bdir = -bdir; }
    else bx += bdir;
    if (maxy + 1 >= H - 1) { over = 1; won = 0; }       /* reached the cannon line */
}

static void fire_invader(void) {
    if (ibn >= 6 || nalive == 0) return;
    int tries = 8;
    while (tries--) {
        int c = rnd() % IC, br = -1;
        for (int r = IR - 1; r >= 0; r--) if (alive[r][c]) { br = r; break; }
        if (br >= 0) { ibx[ibn] = inv_x(c); iby[ibn] = inv_y(br) + 1; ibn++; return; }
    }
}

static int hit_invader(int x, int y) {
    for (int r = 0; r < IR; r++) for (int c = 0; c < IC; c++)
        if (alive[r][c] && inv_x(c) == x && inv_y(r) == y) {
            alive[r][c] = 0; nalive--; score += 10; sys_beep(990, 25);
            if (nalive == 0) { over = 1; won = 1; sys_beep(1320, 200); }
            return 1;
        }
    return 0;
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("\n  Space Invaders"); sys_setcolor(0);
    print("  score "); sys_setcolor(3); putn(score); sys_setcolor(0);
    print("  hi "); sys_setcolor(14); putn(hi); sys_setcolor(0);
    print("  lives "); sys_setcolor(2); putn(lives); sys_setcolor(0); print("\n\n");
    static char fld[H][W + 1];
    for (int y = 0; y < H; y++) { for (int x = 0; x < W; x++) fld[y][x] = ' '; fld[y][W] = 0; }
    for (int r = 0; r < IR; r++) for (int c = 0; c < IC; c++) if (alive[r][c]) {
        int x = inv_x(c), y = inv_y(r); if (x >= 0 && x < W && y >= 0 && y < H) fld[y][x] = 'W';
    }
    if (pbon && pby >= 0 && pby < H) fld[pby][pbx] = '|';
    for (int i = 0; i < ibn; i++) if (iby[i] >= 0 && iby[i] < H) fld[iby[i]][ibx[i]] = '!';
    fld[H-1][px] = '^';
    for (int y = 0; y < H; y++) {
        print("  ");
        for (int x = 0; x < W; x++) {
            char ch = fld[y][x];
            sys_setcolor(ch == 'W' ? 10 : ch == '^' ? 11 : ch == '|' ? 14 : ch == '!' ? 2 : 0);
            char s[2]; s[0] = ch; s[1] = 0; print(s);
        }
        print("\n");
    }
    print("  ");
    if (over) { sys_setcolor(won ? 10 : 2); print(won ? "EARTH SAVED!  (r replay)" : "Game over.  (r replay)"); sys_setcolor(0); }
    else { sys_setcolor(0); print("left/right move   space fire"); }
    print("\n");
}

int main(void) {
    rng = (unsigned)sys_uptime_ms() | 1u;
    load_hi();
    reset();
    render();
    for (;;) {
        int k;
        while ((k = sys_pollkey()) >= 0) {
            if (k == 'q' || k == 'Q') return 0;
            if (k == 'r' || k == 'R') { reset(); render(); }
            else if (!over) {
                if      (k == 0x13 && px > 0)     px--;
                else if (k == 0x14 && px < W - 1) px++;
                else if (k == ' ' && !pbon)       { pbon = 1; pbx = px; pby = H - 2; }
            }
        }
        if (!over) {
            unsigned long now = sys_uptime_ms();
            /* player bullet: fast */
            if (pbon) { pby--; if (pby < 0) pbon = 0; else if (hit_invader(pbx, pby)) pbon = 0; }
            /* invader bullets: fall */
            for (int i = 0; i < ibn; i++) {
                iby[i]++;
                if (iby[i] == H - 1 && ibx[i] == px) { lives--; sys_beep(150, 120); ibx[i] = -9; if (lives <= 0) { over = 1; won = 0; } }
                if (iby[i] >= H) { ibn--; ibx[i] = ibx[ibn]; iby[i] = iby[ibn]; i--; }
            }
            int period = 250 + nalive * 12;             /* faster as fewer remain */
            if (now - t_inv >= (unsigned long)period) { step_invaders(); t_inv = now; }
            if (now - t_fire >= 900) { fire_invader(); t_fire = now; }
            render();
        } else if (!saved) {                 /* game just ended: record a new high score */
            saved = 1;
            if (score > hi) { hi = score; save_hi(); }
            render();
        }
        sys_sleep(45);
    }
}
