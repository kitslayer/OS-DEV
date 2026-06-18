/*
 * pacman.c — a Pac-Man-style maze chase.
 *
 * Eat every dot (.) while dodging the ghosts. Grab a power pellet (O) and the
 * ghosts turn blue for a few seconds — touch them then and they flee back to
 * the pen for points. Clear all the dots to win; lose all three lives and it's
 * over. Arrows steer (the turn takes effect as soon as the way is clear),
 * r restarts, q quits.
 *
 * The maze uses regular wall pillars so every corridor is connected (no dot can
 * be stranded). Real-time: everything advances on a tick, you steer on a key.
 */
#include "ulib.h"

#define ROWS 13
#define COLS 19
#define NG 4

/* '#' wall, '.' dot, 'O' power pellet, ' ' empty path, 'P' pac start, 'G' ghost start */
static const char *MAP[ROWS] = {
    "###################",
    "#O...............O#",
    "#.##.##.###.##.##.#",
    "#.................#",
    "#.##.#.#####.#.##.#",
    "#....#..GGG..#....#",
    "#.##.#.#####.#.##.#",
    "#.................#",
    "#.##.##.###.##.##.#",
    "#.................#",
    "#.##.##.....##.##.#",
    "#O.......P.......O#",
    "###################",
};

static char mz[ROWS][COLS];
static int  pr, pc, pdx, pdy, wdx, wdy;            /* pac pos, dir, wanted dir */
static int  gr[NG], gc[NG], gdx[NG], gdy[NG];      /* ghost pos + dir */
static int  ghr[NG], ghc[NG];                      /* each ghost's home (pen) cell */
static int  score, lives, dots, fright, tick, over, won, best;
static const char *msg;
static unsigned rng;
static unsigned rnd(void){ rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }

static void load_hi(void){ char b[16]; long n=sys_readfile("PACMAN.HI",b,15); best=0; for(long i=0;i<n;i++){ if(b[i]<'0'||b[i]>'9')break; best=best*10+(b[i]-'0'); } }
static void save_hi(void){ char t[12],b[12]; int i=0,n=0,v=best; if(!v)t[i++]='0'; while(v){t[i++]=(char)('0'+v%10);v/=10;} while(i)b[n++]=t[--i]; sys_writefile("PACMAN.HI",b,(unsigned long)n); }

static int wall(int r, int c){ return r<0||r>=ROWS||c<0||c>=COLS||mz[r][c]=='#'; }

static void load(void) {
    int g = 0;
    score = 0; lives = 3; dots = 0; fright = 0; tick = 0; over = 0; won = 0;
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) {
        char ch = MAP[r][c];
        if (ch == 'P') { pr = r; pc = c; mz[r][c] = ' '; }
        else if (ch == 'G') { if (g < NG) { gr[g] = r; gc[g] = c; ghr[g] = r; ghc[g] = c; g++; } mz[r][c] = ' '; }
        else { mz[r][c] = ch; if (ch == '.' || ch == 'O') dots++; }
    }
    while (g < NG) { gr[g] = 5; gc[g] = 9; ghr[g] = 5; ghc[g] = 9; g++; }   /* pad if map had <NG */
    pdx = pdy = wdx = wdy = 0;
    for (int i = 0; i < NG; i++) { gdx[i] = (i & 1) ? 1 : -1; gdy[i] = 0; }
    msg = "Eat the dots! (arrows)";
}

static void reset_actors(void) {       /* after losing a life: send everyone home */
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) if (MAP[r][c] == 'P') { pr = r; pc = c; }
    pdx = pdy = wdx = wdy = 0; fright = 0;
    for (int i = 0; i < NG; i++) { gr[i] = ghr[i]; gc[i] = ghc[i]; gdx[i] = (i&1)?1:-1; gdy[i] = 0; }
}

static void lose_life(void) {
    lives--; sys_beep(140, 250);
    if (lives <= 0) { over = 1; won = 0; msg = "Game over.  r = play again"; }
    else { msg = "Caught!  "; reset_actors(); }
}

static void move_pac(void) {
    if (!wall(pr + wdy, pc + wdx) && (wdx || wdy)) { pdx = wdx; pdy = wdy; }   /* apply queued turn */
    if ((pdx || pdy) && !wall(pr + pdy, pc + pdx)) {
        pr += pdy; pc += pdx;
        char ch = mz[pr][pc];
        if (ch == '.') { mz[pr][pc] = ' '; score += 10; dots--; sys_beep(900, 8); }
        else if (ch == 'O') { mz[pr][pc] = ' '; score += 50; dots--; fright = 45; sys_beep(500, 40); }
        if (dots <= 0) { over = 1; won = 1; msg = "You cleared the maze - YOU WIN!"; }
    }
}

/* one ghost step: pick the non-reversing open direction that best chases (or, when
 * frightened, flees) pac; fall back to any open direction. */
static void step_ghost(int i) {
    static const int dx[4] = {1,-1,0,0}, dy[4] = {0,0,1,-1};
    int best = -1, bestscore = -1000000000;
    for (int d = 0; d < 4; d++) {
        int nx = gc[i] + dx[d], ny = gr[i] + dy[d];
        if (wall(ny, nx)) continue;
        if (dx[d] == -gdx[i] && dy[d] == -gdy[i]) continue;     /* don't reverse */
        int dist = (nx - pc)*(nx - pc) + (ny - pr)*(ny - pr);
        int sc = fright ? dist : -dist;                          /* flee when frightened, else chase */
        sc += (int)(rnd() % 3);                                  /* a little wobble */
        if (sc > bestscore) { bestscore = sc; best = d; }
    }
    if (best < 0) for (int d = 0; d < 4; d++) {                  /* dead end: allow reverse */
        int nx = gc[i] + dx[d], ny = gr[i] + dy[d];
        if (!wall(ny, nx)) { best = d; break; }
    }
    if (best >= 0) { gdx[i] = dx[best]; gdy[i] = dy[best]; gr[i] += dy[best]; gc[i] += dx[best]; }
}

static void collide(void) {
    for (int i = 0; i < NG; i++) if (gr[i] == pr && gc[i] == pc) {
        if (fright) { gr[i] = ghr[i]; gc[i] = ghc[i]; gdx[i] = 1; gdy[i] = 0; score += 200; sys_beep(1400, 60); }
        else { lose_life(); return; }
    }
}

static void putn(int n){ char t[10]; int i=0; if(!n){print("0");return;} while(n){t[i++]=(char)('0'+n%10);n/=10;} char s[10]; int j=0; while(i)s[j++]=t[--i]; s[j]=0; print(s); }

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("  Pac-Man"); sys_setcolor(0);
    print("  score "); sys_setcolor(2); putn(score); sys_setcolor(0);
    print("  lives "); sys_setcolor(12); putn(lives);
    sys_setcolor(0); print("  best "); sys_setcolor(14); putn(best); sys_setcolor(0); print("\n");
    for (int r = 0; r < ROWS; r++) {
        print(" ");
        for (int c = 0; c < COLS; c++) {
            int gi = -1; for (int i = 0; i < NG; i++) if (gr[i] == r && gc[i] == c) { gi = i; break; }
            if (r == pr && c == pc) { sys_setcolor(14); print("C"); }            /* pac */
            else if (gi >= 0) { sys_setcolor(fright ? 9 : 12); print(fright ? "&" : "M"); }   /* ghost */
            else {
                char ch = mz[r][c];
                if (ch == '#') { sys_setcolor(1); print("#"); }
                else if (ch == '.') { sys_setcolor(8); print("."); }
                else if (ch == 'O') { sys_setcolor(15); print("O"); }
                else { print(" "); }
            }
        }
        sys_setcolor(0); print("\n");
    }
    sys_setcolor(over ? 2 : 0); print("  "); print(msg); sys_setcolor(0);
    print("\n  arrows steer   r reset   q quit\n");
}

int main(void) {
    rng = (unsigned)sys_uptime_ms() | 1u;
    load_hi(); load(); render();
    unsigned long last = sys_uptime_ms();
    for (;;) {
        int k;
        while ((k = sys_pollkey()) >= 0) {
            if (k == 'q' || k == 'Q') return 0;
            if (k == 'r' || k == 'R') { load(); render(); last = sys_uptime_ms(); }
            else if (!over) {
                if      (k == 0x11) { wdx = 0; wdy = -1; }
                else if (k == 0x12) { wdx = 0; wdy = 1; }
                else if (k == 0x13) { wdx = -1; wdy = 0; }
                else if (k == 0x14) { wdx = 1; wdy = 0; }
            }
        }
        unsigned long now = sys_uptime_ms();
        if (!over && now - last >= 150) {
            last = now; tick++;
            move_pac();
            if (!over) collide();
            if (!over) {
                int gstep = fright ? (tick % 2 == 0) : (tick % 4 != 0);   /* ghosts ~75% speed (half when frightened) so you can escape */
                if (gstep) for (int i = 0; i < NG; i++) step_ghost(i);
                collide();
                if (fright > 0) fright--;
            }
            if (over && score > best) { best = score; save_hi(); }
            render();
        }
        sys_sleep(15);
    }
}
