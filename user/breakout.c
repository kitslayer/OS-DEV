/*
 * breakout.c — Breakout, a userspace program (9th app).
 *
 * A paddle at the bottom, a bouncing ball, and rows of bricks to clear. The
 * ball steps one cell per tick (velocity +/-1 on each axis), bouncing off the
 * walls, the paddle, and bricks (each hit removes a brick). Lose a life if the
 * ball passes the paddle; win when all bricks are gone. Non-blocking input, like
 * the other games.
 */
#include "ulib.h"

#define W 40
#define H 14
#define BRICK_ROWS 3
#define PADW 7

static char brick[BRICK_ROWS][W];     /* 1 = present */
static int bricks_left;

static void render(int px, int bx, int by, int lives, const char *msg) {
    static char g[H][W];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) g[y][x] = ' ';
    for (int r = 0; r < BRICK_ROWS; r++) for (int x = 0; x < W; x++) if (brick[r][x]) g[r][x] = '#';
    for (int x = 0; x < PADW; x++) if (px + x < W) g[H-1][px + x] = '=';
    if (by >= 0 && by < H && bx >= 0 && bx < W) g[by][bx] = 'o';

    sys_clear();
    char sl[48]; int p = 0;
    const char *a = "BREAKOUT  lives "; while (*a) sl[p++] = *a++;
    sl[p++] = (char)('0' + lives);
    a = "  bricks "; while (*a) sl[p++] = *a++;
    int v = bricks_left, k = 0; char t[8]; if (!v) t[k++]='0'; while (v){t[k++]='0'+v%10;v/=10;} while (k) sl[p++]=t[--k];
    sl[p] = 0; sys_setcolor(8); print(sl); print("\n");

    static const unsigned char BRICK_COL[3] = { 2, 7, 3 };   /* row colours: red, orange, yellow */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            char ch = g[y][x]; int col = 0;
            if (ch == '#')      col = BRICK_COL[y < BRICK_ROWS ? y : BRICK_ROWS - 1];  /* brick: by row */
            else if (ch == '=') col = 4;        /* paddle: cyan */
            else if (ch == 'o') col = 1;        /* ball: white */
            sys_setcolor(col);
            char cb[2] = { ch, 0 }; print(cb);
        }
        sys_setcolor(0); print("\n");
    }
    if (msg) print(msg);
}

int main(void) {
    int px, bx, by, dx, dy, lives;
restart:
    for (int r = 0; r < BRICK_ROWS; r++) for (int x = 0; x < W; x++) brick[r][x] = 1;
    bricks_left = BRICK_ROWS * W;
    lives = 3; px = (W - PADW) / 2;
serve:
    bx = W / 2; by = H - 2; dx = 1; dy = -1;
    render(px, bx, by, lives, 0);
    for (;;) {
        int k;
        while ((k = sys_pollkey()) >= 0) {
            if (k == 'q') return 0;
            else if (k == 0x13) { if (px > 0) px--; }            /* left  */
            else if (k == 0x14) { if (px < W - PADW) px++; }     /* right */
        }
        int nx = bx + dx, ny = by + dy;
        if (nx < 0 || nx >= W) { dx = -dx; nx = bx + dx; }       /* side walls */
        if (ny < 0)            { dy = -dy; ny = by + dy; }       /* top wall */
        /* brick hit? */
        if (ny >= 0 && ny < BRICK_ROWS && nx >= 0 && nx < W && brick[ny][nx]) {
            brick[ny][nx] = 0; bricks_left--; dy = -dy; ny = by + dy;
            if (bricks_left == 0) { render(px, bx, by, lives, "\n YOU WIN! any key to replay, q quit\n");
                                    for (;;){int c=sys_pollkey(); if(c=='q')return 0; if(c>=0)goto restart; sys_sleep(50);} }
        }
        /* paddle / bottom */
        if (ny >= H - 1) {
            if (nx >= px && nx < px + PADW) { dy = -dy; ny = by + dy;  /* bounce off paddle */
                int hit = nx - px; if (hit < 2 && dx > 0) dx = -1; else if (hit > PADW-3 && dx < 0) dx = 1; }
            else {                                                /* missed: lose a life */
                if (--lives <= 0) { render(px, bx, by, 0, "\n GAME OVER - any key to retry, q quit\n");
                                    for (;;){int c=sys_pollkey(); if(c=='q')return 0; if(c>=0)goto restart; sys_sleep(50);} }
                goto serve;
            }
        }
        bx = nx; by = ny;
        render(px, bx, by, lives, 0);
        sys_sleep(90);
    }
}
