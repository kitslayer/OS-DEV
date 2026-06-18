/*
 * frogger.c — cross the traffic (a Frogger-style dodge game).
 *
 * Hop your frog upward across lanes of cars (each lane scrolls at its own speed
 * and direction) to reach the goal at the top. Touch a car and you lose a life;
 * reach the top to score and start again from the bottom. Three lives; best is
 * saved to FROGGER.HI.
 *
 * Arrows hop, r restarts, q quits. Real-time: lanes scroll on a tick, you hop
 * on a keypress.
 */
#include "ulib.h"

#define W 34
#define H 12                        /* row 0 = goal, 1..4 + 6..10 = roads, 5 + 11 = safe */
static char lane[H][W];             /* 1 = car */
static int ldir[H];                 /* per-row scroll dir: -1, +1, or 0 (safe) */
static int frow, fcol, lives, score, hi, over;
static const char *msg;

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void putn(int n) {
    char t[8]; int i = 0; if (n == 0) { print("0"); return; }
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    char s[8]; int j = 0; while (i) s[j++] = t[--i]; s[j] = 0; print(s);
}
static void load_hi(void){ char b[16]; long n=sys_readfile("FROGGER.HI",b,15); hi=0; for(long i=0;i<n;i++){ if(b[i]<'0'||b[i]>'9')break; hi=hi*10+(b[i]-'0'); } }
static void save_hi(void){ char t[12],b[12]; int i=0,n=0,v=hi; if(v==0)t[i++]='0'; while(v){t[i++]=(char)('0'+v%10);v/=10;} while(i)b[n++]=t[--i]; sys_writefile("FROGGER.HI",b,(unsigned long)n); }

static int is_road(int r) { return r != 0 && r != 5 && r != 11; }

static void build_lanes(void) {
    for (int r = 0; r < H; r++) {
        for (int c = 0; c < W; c++) lane[r][c] = 0;
        ldir[r] = 0;
        if (is_road(r)) {
            ldir[r] = (r & 1) ? 1 : -1;                 /* alternate direction */
            int gap = 4 + (int)(rnd() % 4), len = 2 + (int)(rnd() % 2), c = (int)(rnd() % gap);
            while (c < W) { for (int i = 0; i < len && c + i < W; i++) lane[r][c+i] = 1; c += len + gap; }
        }
    }
}
static void reset_frog(void) { frow = 11; fcol = W/2; }
static void reset(void) { build_lanes(); reset_frog(); lives = 3; score = 0; over = 0; msg = "Hop up! (arrows)"; }

static void scroll_lanes(void) {
    for (int r = 0; r < H; r++) {
        if (!ldir[r]) continue;
        if (ldir[r] > 0) { char last = lane[r][W-1]; for (int c = W-1; c > 0; c--) lane[r][c] = lane[r][c-1]; lane[r][0] = last; }
        else            { char first = lane[r][0]; for (int c = 0; c < W-1; c++) lane[r][c] = lane[r][c+1]; lane[r][W-1] = first; }
    }
}
static void hit_check(void) {
    if (is_road(frow) && lane[frow][fcol]) {            /* run over */
        lives--; sys_beep(150, 150);
        if (lives <= 0) { over = 1; msg = "Game over.  (r replay)"; if (score > hi) { hi = score; save_hi(); } sys_beep(110,300); }
        else { reset_frog(); msg = "Squashed! hop up"; }
    }
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("  Frogger"); sys_setcolor(0);
    print("   score "); sys_setcolor(2); putn(score); sys_setcolor(0);
    print("  hi "); sys_setcolor(14); putn(hi); sys_setcolor(0);
    print("  lives "); sys_setcolor(2); putn(lives); sys_setcolor(0); print("\n");
    for (int r = 0; r < H; r++) {
        print("  ");
        for (int c = 0; c < W; c++) {
            if (r == frow && c == fcol) { sys_setcolor(10); print("@"); }
            else if (r == 0)            { sys_setcolor(10); print("="); }   /* goal bank */
            else if (lane[r][c])        { sys_setcolor(r&1?3:13); print("#"); }   /* car */
            else if (is_road(r))        { sys_setcolor(8); print("."); }
            else                        { sys_setcolor(2); print(":"); }    /* safe grass */
        }
        sys_setcolor(0); print("\n");
    }
    print("  ");
    if (over) { sys_setcolor(2); print(msg); } else { sys_setcolor(0); print(msg); }
    sys_setcolor(0); print("   r reset  q quit\n");
}

int main(void) {
    rng = (unsigned)sys_uptime_ms() | 1u;
    load_hi(); reset(); render();
    unsigned long last = sys_uptime_ms();
    for (;;) {
        int k;
        while ((k = sys_pollkey()) >= 0) {
            if (k == 'q' || k == 'Q') return 0;
            if (k == 'r' || k == 'R') { reset(); render(); last = sys_uptime_ms(); }
            else if (!over) {
                if      (k == 0x11 && frow > 0)   frow--;
                else if (k == 0x12 && frow < H-1) frow++;
                else if (k == 0x13 && fcol > 0)   fcol--;
                else if (k == 0x14 && fcol < W-1) fcol++;
                if (frow == 0) { score += 10; sys_beep(880,80); sys_beep(1320,80); reset_frog(); msg = "Made it! +10"; }
                else hit_check();
                render();
            }
        }
        unsigned long now = sys_uptime_ms();
        if (!over && now - last >= 180) { scroll_lanes(); hit_check(); render(); last = now; }
        sys_sleep(20);
    }
}
