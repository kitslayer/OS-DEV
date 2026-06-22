/*
 * columns.c — Columns, the Sega falling-block classic (a userspace game).
 *
 * A vertical column of three coloured gems falls down a narrow well. You slide
 * it left/right, soft-drop it, and — the signature move — *cycle the three gems'
 * colours within the column* (the piece keeps its 1x3 shape, only the colours
 * rotate). When it lands and locks, every run of >=3 same-colour gems in a line
 * clears — horizontally, vertically, OR along either diagonal. Cleared gems
 * vanish, the gems above fall to fill the gaps (gravity per column), and the
 * well is re-scanned, so one landing can chain into cascades (each step scores
 * more). It's Tetris's fall/timing married to Gems's match-clear-cascade.
 *
 * Like the other games it runs ring-3, draws into the app text grid, polls keys
 * non-blocking, and changes no kernel code. High score persists to COLUMNS.HI.
 */
#include "ulib.h"

#define COLS 6              /* well width  */
#define ROWS 13             /* well height (fits the 17-row grid with header+footer) */
#define NCOL 6              /* number of distinct gem colours */
#define PIECE 3             /* gems in the falling column */
#define EMPTY (-1)          /* an empty well cell */

static int well[ROWS][COLS];        /* each cell holds a colour 0..NCOL-1, or EMPTY */
static int px, py;                  /* falling column: top gem at (px, py); the 3 gems occupy rows py..py+2 */
static int pcol[PIECE];             /* the 3 colours, top..bottom */
static unsigned score;              /* current score */
static unsigned cleared_total;      /* gems cleared so far (drives the level) */
static int level;                   /* speeds the fall; rises every LEVEL_STEP clears */
static int over;                    /* set when a new column can't enter */
static unsigned best;               /* high score, persisted to COLUMNS.HI */
static const char *msg;             /* a one-line status cue under the well, or 0 */

#define LEVEL_STEP 15               /* clears needed to advance one level */

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void itoa_u(unsigned v, char *o) {
    char t[12]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0; while (i) o[j++] = t[--i];
    o[j] = 0;
}

static void load_best(void) {
    char b[16]; long n = sys_readfile("COLUMNS.HI", b, sizeof(b) - 1);
    best = 0;
    for (long i = 0; i < n; i++) { if (b[i] < '0' || b[i] > '9' || best >= 100000000u) break; best = best * 10 + (unsigned)(b[i] - '0'); }   /* cap: a corrupt .HI can't wrap the unsigned score */
}
static void save_best(void) {
    char b[12]; itoa_u(best, b);
    int n = 0; while (b[n]) n++;
    sys_writefile("COLUMNS.HI", b, (unsigned long)n);
}

/* The six gem colours: each is a palette index (see app_palette in kernel/app.c)
 * and a distinct letter, so the well reads clearly even where two hues are close.
 * Index into both by the cell's colour value 0..NCOL-1. */
static const int  gem_color[NCOL] = { 2, 3, 9, 4, 6, 5 };   /* red, yellow, lime, cyan, blue, pink */
static const char gem_char[NCOL]  = { 'R', 'Y', 'G', 'C', 'B', 'P' };

/* Spawn a fresh falling column at the top-centre with three random colours. If
 * its cells are already occupied, the well is full -> game over. */
static void spawn(void) {
    px = COLS / 2;
    py = 0;                                     /* top gem on the top row; gems at py,py+1,py+2 */
    for (int i = 0; i < PIECE; i++) pcol[i] = (int)(rnd() % NCOL);
    for (int i = 0; i < PIECE; i++) {
        int y = py + i;
        if (y >= 0 && y < ROWS && well[y][px] != EMPTY) { over = 1; return; }
    }
}

/* Can the falling column sit with its top gem at (nx,ny)? Off-well or onto an
 * occupied cell -> no. (Each of the 3 gems is checked; ny+2 must stay < ROWS.) */
static int fits(int nx, int ny) {
    if (nx < 0 || nx >= COLS) return 0;
    for (int i = 0; i < PIECE; i++) {
        int y = ny + i;
        if (y >= ROWS) return 0;                /* bottom of the well */
        if (y >= 0 && well[y][nx] != EMPTY) return 0;   /* lands on a stack */
    }
    return 1;
}

/* Stamp the three falling gems into the well at their current position. Every
 * write is bounded to [0,ROWS-1] x px. */
static void lock_piece(void) {
    for (int i = 0; i < PIECE; i++) {
        int y = py + i;
        if (y >= 0 && y < ROWS && px >= 0 && px < COLS) well[y][px] = pcol[i];
    }
}

/* The four match directions, each as a (dx,dy) step: horizontal, vertical, and
 * the two diagonals. A run is counted by walking from each cell in +dir; we
 * dedupe by only *starting* a run where the previous cell (in -dir) differs or
 * is off-well, so each maximal run is found exactly once. */
static const int DIR[4][2] = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 1, -1 } };

/* Mark every cell in a run of >=3 same-colour gems along any of the four
 * directions. Writes 1 into clr[][] for cells to clear and returns the count.
 *
 * Bounds: a cell (x,y) only *starts* a run if stepping back by -dir lands off
 * the well or on a different colour. We then walk forward while the next cell is
 * in range AND the same colour. Both the back-step probe and the forward walk
 * are range-checked every step, so the diagonal offsets (the easy place to go
 * out of range) can never read or write outside [0,ROWS-1] x [0,COLS-1]. */
static int mark_matches(int clr[ROWS][COLS]) {
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) clr[y][x] = 0;

    for (int d = 0; d < 4; d++) {
        int dx = DIR[d][0], dy = DIR[d][1];
        for (int y = 0; y < ROWS; y++)
            for (int x = 0; x < COLS; x++) {
                int c = well[y][x];
                if (c == EMPTY) continue;
                /* only start a run at the head: the cell behind us (in -dir) is
                 * off-well or a different colour */
                int bx = x - dx, by = y - dy;
                if (bx >= 0 && bx < COLS && by >= 0 && by < ROWS && well[by][bx] == c) continue;
                /* walk forward counting same-colour gems, every step in-range */
                int run = 0, nx = x, ny = y;
                while (nx >= 0 && nx < COLS && ny >= 0 && ny < ROWS && well[ny][nx] == c) {
                    run++; nx += dx; ny += dy;
                }
                if (run >= 3) {                 /* mark exactly the run we counted */
                    int mx = x, my = y;
                    for (int k = 0; k < run; k++) { clr[my][mx] = 1; mx += dx; my += dy; }
                }
            }
    }
    int found = 0;
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) if (clr[y][x]) found++;
    return found;
}

/* Apply gravity in each column: surviving gems (clr==0) fall to the bottom,
 * the gaps left above become EMPTY. Operates one column at a time into a compact
 * buffer, so every write stays in [0,ROWS-1]. (Unlike Gems, nothing refills from
 * the top — in Columns new gems arrive only as falling pieces.) */
static void collapse(int clr[ROWS][COLS]) {
    for (int x = 0; x < COLS; x++) {
        int keep[ROWS], n = 0;
        for (int y = ROWS - 1; y >= 0; y--)             /* bottom-up: survivors keep their order */
            if (!clr[y][x] && well[y][x] != EMPTY) keep[n++] = well[y][x];
        for (int y = ROWS - 1; y >= 0; y--) {
            int idx = (ROWS - 1) - y;                   /* 0 at the bottom row */
            well[y][x] = (idx < n) ? keep[idx] : EMPTY; /* gaps above become empty */
        }
    }
}

static void render(void);                               /* fwd decl for resolve_matches's animation */

/* Resolve all matches from the current well: repeatedly mark+score+clear+collapse
 * while runs keep forming (cascades). The score per cleared gem rises with each
 * cascade step (chain multiplier), so longer chains pay much more. Each clear
 * also counts toward the level. `animate` re-renders between steps with a short
 * pause so cascades are visible. Returns the number of cascade steps that
 * cleared something (0 = the landing made no match). */
static int resolve_matches(int animate) {
    int clr[ROWS][COLS];
    int step = 0;
    while (mark_matches(clr) > 0) {
        int cleared = 0;
        for (int y = 0; y < ROWS; y++)
            for (int x = 0; x < COLS; x++) if (clr[y][x]) cleared++;
        step++;
        score += (unsigned)(cleared * 10 * step);       /* 10 pts/gem, x step = rising cascade multiplier */
        cleared_total += (unsigned)cleared;
        if ((int)(cleared_total / LEVEL_STEP) > level) level = (int)(cleared_total / LEVEL_STEP);
        if (animate) {                                  /* briefly show the matched gems blanked, then drop */
            for (int y = 0; y < ROWS; y++)
                for (int x = 0; x < COLS; x++) if (clr[y][x]) well[y][x] = EMPTY;   /* renders as a gap */
            render();
            sys_sleep(110);
        }
        collapse(clr);
        if (animate) { render(); sys_sleep(80); }
    }
    return step;
}

/* Lock the falling column, resolve any matches+cascades (scoring), persist a new
 * best, then spawn the next column (which sets `over` if the well is full). */
static void land(void) {
    lock_piece();
    int steps = resolve_matches(1);
    if (score > best) { best = score; save_best(); }
    if (steps > 1) msg = "cascade!";
    else if (steps == 1) msg = "match!";
    else msg = 0;
    spawn();
}

/* One gravity step: drop the column a row, or land+lock it if it can't fall. */
static void step_down(void) {
    if (fits(px, py + 1)) py++;
    else land();
}

/* Cycle the three gems' colours downward within the column (the signature
 * Columns rotate): bottom wraps to top. The column's shape never changes, so no
 * collision check is needed. */
static void rotate(void) {
    int b = pcol[PIECE - 1];
    for (int i = PIECE - 1; i > 0; i--) pcol[i] = pcol[i - 1];
    pcol[0] = b;
}

static void render(void) {
    sys_clear();
    /* header: title, score, best, level */
    char hdr[64]; int p = 0;
    const char *t = "Columns  ";
    while (*t) hdr[p++] = *t++;
    char num[12];
    t = "sc "; while (*t) hdr[p++] = *t++;
    itoa_u(score, num); for (int i = 0; num[i]; i++) hdr[p++] = num[i];
    t = "  lv "; while (*t) hdr[p++] = *t++;
    itoa_u((unsigned)(level + 1), num); for (int i = 0; num[i]; i++) hdr[p++] = num[i];
    t = "  hi "; while (*t) hdr[p++] = *t++;
    itoa_u(best, num); for (int i = 0; num[i]; i++) hdr[p++] = num[i];
    hdr[p] = 0;
    sys_setcolor(8); print(hdr); print("\n");           /* header in grey */

    /* Compose the well + the in-flight falling column into a colour grid. Each
     * cell is drawn as 2 chars ("R "...) so a 6-wide well reads clearly. */
    static int g[ROWS][COLS];
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) g[y][x] = well[y][x];
    if (!over)
        for (int i = 0; i < PIECE; i++) {               /* overlay the falling gems */
            int y = py + i;
            if (y >= 0 && y < ROWS && px >= 0 && px < COLS) g[y][px] = pcol[i];
        }

    for (int y = 0; y < ROWS; y++) {
        sys_setcolor(8); print("|");                    /* left wall, grey */
        for (int x = 0; x < COLS; x++) {
            int v = g[y][x];
            if (v >= 0 && v < NCOL) {
                char cb[3]; cb[0] = gem_char[v]; cb[1] = ' '; cb[2] = 0;
                sys_setcolor(gem_color[v]); print(cb);  /* each gem its own hue */
            } else { sys_setcolor(0); print("  "); }    /* empty cell */
        }
        sys_setcolor(8); print(y < ROWS - 1 ? "|\n" : "|");   /* no trailing newline on the last row, or the header scrolls off the 17-row grid */
    }
    sys_setcolor(0);
    if (over) print("\n GAME OVER - n new, q quit");
    else {
        print("\n arrows move/drop, space rotate, n/q");
        if (msg) { print("  "); sys_setcolor(1); print(msg); sys_setcolor(0); }
    }
}

/* How long between gravity ticks at this level, in 40ms loop iterations. Starts
 * slow and quickens, with a floor so the highest levels stay playable. */
static int drop_ticks(void) {
    int t = 12 - level;                 /* lv1 ~ 480ms/row, dropping ~40ms/level */
    if (t < 2) t = 2;                   /* floor: ~80ms/row */
    return t;
}

static void new_game(void) {
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) well[y][x] = EMPTY;
    score = cleared_total = 0; level = 0; over = 0; msg = 0;
    spawn();
}

int main(void) {
    rng = (unsigned)sys_uptime_ms();                    /* seed the PRNG from the boot clock */
    rng ^= 0x9E3779B9u; if (!rng) rng = 0xC0FFEEu;
    for (int i = 0; i < 8; i++) rnd();                  /* stir before first use */
    load_best();
    new_game();
    render();

    int tick = 0;
    for (;;) {
        int k;
        while ((k = sys_pollkey()) >= 0) {
            if (k == 'q' || k == 27) return 0;          /* q or Esc: quit */
            if (over) {                                 /* only `n` (new game) is live after game over */
                if (k == 'n') { new_game(); render(); }
                continue;
            }
            if (k == 'n') { new_game(); render(); continue; }
            else if (k == 0x13) { if (fits(px - 1, py)) px--; }         /* left  */
            else if (k == 0x14) { if (fits(px + 1, py)) px++; }         /* right */
            else if (k == 0x12) { step_down(); tick = 0; }             /* down: soft-drop a row */
            else if (k == 0x11 || k == ' ') { rotate(); }             /* up/space: cycle the 3 colours */
            else continue;
            if (over) break;
            render();
        }
        if (over) { sys_sleep(40); continue; }          /* idle until `n`/`q` at the game-over screen */
        if (++tick >= drop_ticks()) { tick = 0; step_down(); render(); }
        sys_sleep(40);
    }
}
