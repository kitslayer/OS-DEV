/*
 * gems.c — Gems, a match-3 puzzle (Bejeweled/Columns style) userspace game.
 *
 * An 8x8 grid of six coloured gems. Move a cursor with the arrows; grab a gem
 * (space/Enter) then push an arrow to swap it with its neighbour. A swap only
 * sticks if it makes a line of >=3 of one colour — those clear, the gems above
 * fall into the gaps, fresh gems drop in from the top, and the board re-scans,
 * so a single swap can chain into cascades (each step scores more). Like the
 * other games it runs ring-3, draws into the app text grid, reads keys
 * non-blocking, and changes no kernel code.
 */
#include "ulib.h"

#define ROWS 8
#define COLS 8
#define NCOL 6          /* number of distinct gem colours */

static int board[ROWS][COLS];   /* each cell holds a colour 0..NCOL-1 */
static int cx, cy;              /* cursor position (column, row) */
static int grabbed;             /* 1 once a gem is grabbed and awaiting a swap arrow */
static unsigned score;
static unsigned best;           /* high score, persisted to GEMS.HI */
static const char *msg;         /* a one-line status cue under the board, or 0 */

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
    char b[16]; long n = sys_readfile("GEMS.HI", b, sizeof(b) - 1);
    best = 0;
    for (long i = 0; i < n; i++) { if (b[i] < '0' || b[i] > '9' || best >= 100000000u) break; best = best * 10 + (unsigned)(b[i] - '0'); }   /* cap: a corrupt .HI can't wrap the unsigned score */
}
static void save_best(void) {
    char b[12]; itoa_u(best, b);
    int n = 0; while (b[n]) n++;
    sys_writefile("GEMS.HI", b, (unsigned long)n);
}

/* The six gem colours: each is a palette index (see app_palette in kernel/app.c)
 * and a distinct letter, so the board reads clearly even where two hues are
 * close. Index into both by the cell's colour value 0..NCOL-1. */
static const int  gem_color[NCOL] = { 2, 3, 9, 4, 6, 5 };   /* red, yellow, lime, cyan, blue, pink */
static const char gem_char[NCOL]  = { 'R', 'Y', 'G', 'C', 'B', 'P' };

/* Would placing colour `c` at (x,y) complete a run of three with the two cells
 * already filled to its left or above? Used during the initial fill so the
 * starting board never contains a pre-existing match (the player begins stable).
 * Only the left/up neighbours are inspected because the board fills in row-major
 * order, so cells to the right/below aren't chosen yet. */
static int makes_run(int x, int y, int c) {
    if (x >= 2 && board[y][x-1] == c && board[y][x-2] == c) return 1;
    if (y >= 2 && board[y-1][x] == c && board[y-2][x] == c) return 1;
    return 0;
}

/* Fill the whole board with random gems, rejecting any colour that would form a
 * 3-in-a-row as we go — so there are no matches to clear before the first move. */
static void fill_board(void) {
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) {
            int c;
            do { c = (int)(rnd() % NCOL); } while (makes_run(x, y, c));   /* always >=1 safe colour exists with 6 hues and only 2 forbidden lines */
            board[y][x] = c;
        }
}

/* Mark every cell that belongs to a horizontal or vertical run of >=3 same-colour
 * gems. Writes 1 into clr[][] for cells to clear (cleared by the caller) and
 * returns how many cells were marked. Bounded scans: a horizontal run can only
 * start at x in [0,COLS-3], a vertical at y in [0,ROWS-3]. */
static int mark_matches(int clr[ROWS][COLS]) {
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) clr[y][x] = 0;
    int found = 0;
    /* horizontal runs */
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; ) {
            int c = board[y][x], run = 1;
            while (x + run < COLS && board[y][x+run] == c) run++;
            if (run >= 3) for (int k = 0; k < run; k++) clr[y][x+k] = 1;
            x += run;                                   /* skip the run we just measured */
        }
    /* vertical runs */
    for (int x = 0; x < COLS; x++)
        for (int y = 0; y < ROWS; ) {
            int c = board[y][x], run = 1;
            while (y + run < ROWS && board[y+run][x] == c) run++;
            if (run >= 3) for (int k = 0; k < run; k++) clr[y+k][x] = 1;
            y += run;
        }
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) if (clr[y][x]) found++;
    return found;
}

/* Apply gravity in each column: surviving gems (where clr==0) fall to the bottom,
 * the gaps left at the top refill with new random gems. Operates one column at a
 * time into a compact buffer, so every write stays in [0,ROWS-1]. */
static void collapse(int clr[ROWS][COLS]) {
    for (int x = 0; x < COLS; x++) {
        int keep[ROWS], n = 0;
        for (int y = ROWS - 1; y >= 0; y--)             /* bottom-up: survivors keep their order */
            if (!clr[y][x]) keep[n++] = board[y][x];
        for (int y = ROWS - 1; y >= 0; y--) {
            int idx = (ROWS - 1) - y;                   /* 0 at the bottom row */
            board[y][x] = (idx < n) ? keep[idx] : (int)(rnd() % NCOL);   /* refill the top with new gems */
        }
    }
}

static void render(void);                               /* fwd decl for resolve_matches's animation */

/* Resolve all matches starting from the current board: repeatedly mark+score+
 * clear+collapse while runs keep forming (cascades). The score per cleared gem
 * rises with each cascade step (chain multiplier), so longer chains pay more.
 * `animate` re-renders between steps with a short pause for visible cascades.
 * Returns the number of cascade steps that cleared something (0 = no match). */
static int resolve_matches(int animate) {
    int clr[ROWS][COLS];
    int step = 0;
    while (mark_matches(clr) > 0) {
        int cleared = 0;
        for (int y = 0; y < ROWS; y++)
            for (int x = 0; x < COLS; x++) if (clr[y][x]) cleared++;
        step++;
        score += (unsigned)(cleared * 10 * step);       /* 10 pts/gem, x step = rising cascade multiplier */
        if (animate) {                                  /* briefly show the matched gems blanked, then drop */
            for (int y = 0; y < ROWS; y++)
                for (int x = 0; x < COLS; x++) if (clr[y][x]) board[y][x] = -1;   /* -1 renders as a gap */
            render();
            sys_sleep(90);
            for (int y = 0; y < ROWS; y++)
                for (int x = 0; x < COLS; x++) if (board[y][x] == -1) board[y][x] = 0;   /* tidy before collapse reads it (clr still marks them) */
        }
        collapse(clr);
        if (animate) { render(); sys_sleep(70); }
    }
    return step;
}

/* Swap (x0,y0) with (x1,y1). Both are assumed adjacent + in range (the caller
 * only ever swaps the cursor with an in-range neighbour). */
static void swap_cells(int x0, int y0, int x1, int y1) {
    int t = board[y0][x0]; board[y0][x0] = board[y1][x1]; board[y1][x1] = t;
}

/* Is there ANY legal move — a single adjacent swap that creates a match? Tries
 * each cell's swap with its right and down neighbour (every adjacency is covered
 * once across the board), testing with mark_matches and undoing. Lets us detect
 * a dead board and reshuffle so the game never deadlocks. */
static int has_move(void) {
    int clr[ROWS][COLS];
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) {
            if (x + 1 < COLS) {
                swap_cells(x, y, x+1, y);
                int hit = mark_matches(clr) > 0;
                swap_cells(x, y, x+1, y);               /* undo */
                if (hit) return 1;
            }
            if (y + 1 < ROWS) {
                swap_cells(x, y, x, y+1);
                int hit = mark_matches(clr) > 0;
                swap_cells(x, y, x, y+1);               /* undo */
                if (hit) return 1;
            }
        }
    return 0;
}

/* Start a fresh board: random, match-free, and guaranteed to have a legal move
 * (reshuffle until both hold). */
static void new_board(void) {
    do { fill_board(); } while (!has_move());           /* fill_board already guarantees no initial match */
    cx = cy = 0; grabbed = 0;
    score = 0; msg = 0;
}

/* A dead board (no possible match): reshuffle in place, keeping the score, until
 * a legal move exists (fill_board already guarantees no immediate match). */
static void reshuffle(void) {
    do { fill_board(); } while (!has_move());
    grabbed = 0;
    msg = "no moves - reshuffled";
}

static void render(void) {
    sys_clear();
    char hdr[64]; int p = 0;
    const char *t = "  Gems   score ";
    while (*t) hdr[p++] = *t++;
    char num[12]; itoa_u(score, num); for (int i = 0; num[i]; i++) hdr[p++] = num[i];
    t = "   best "; while (*t) hdr[p++] = *t++;
    itoa_u(best, num); for (int i = 0; num[i]; i++) hdr[p++] = num[i];
    hdr[p] = 0;
    sys_setcolor(8); print(hdr); print("\n\n");          /* header in grey */

    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            int v = board[y][x];
            char ch = (v >= 0 && v < NCOL) ? gem_char[v] : ' ';   /* -1 (mid-clear) renders blank */
            int cur = (x == cx && y == cy);
            char cb[4];
            /* '[ ]' marks the cursor, '( )' a grabbed gem awaiting its swap arrow */
            if (cur && grabbed) { cb[0]='('; cb[1]=ch; cb[2]=')'; }
            else if (cur)       { cb[0]='['; cb[1]=ch; cb[2]=']'; }
            else                { cb[0]=' '; cb[1]=ch; cb[2]=' '; }
            cb[3] = 0;
            sys_setcolor(cur ? 1 : ((v >= 0 && v < NCOL) ? gem_color[v] : 0));   /* cursor cell white, else the gem's hue */
            print(cb);
        }
        print("\n");
    }
    sys_setcolor(0);
    print("\n  arrows move, space grab+swap, n new, q quit");
    if (msg) { print("\n  "); print(msg); }
}

/* Try to swap the cursor gem in direction (dx,dy). Commit only if it forms a
 * match (then resolve cascades + score); otherwise swap back and cue "no match".
 * Bounds-checked: an off-board target is rejected outright. */
static void try_swap(int dx, int dy) {
    int nx = cx + dx, ny = cy + dy;
    grabbed = 0;
    if (nx < 0 || nx >= COLS || ny < 0 || ny >= ROWS) { msg = "edge - no swap"; return; }
    swap_cells(cx, cy, nx, ny);
    int clr[ROWS][COLS];
    if (mark_matches(clr) > 0) {                        /* legal: the swap created a match */
        render(); sys_sleep(60);                        /* show the swap before it resolves */
        int steps = resolve_matches(1);
        if (score > best) { best = score; save_best(); }
        msg = (steps > 1) ? "cascade!" : "match!";
        if (!has_move()) reshuffle();                   /* swap left a dead board: keep the game alive */
    } else {                                            /* illegal: revert the swap */
        swap_cells(cx, cy, nx, ny);
        msg = "no match";
    }
}

int main(void) {
    rng = (unsigned)sys_uptime_ms();                    /* seed the PRNG from the boot clock */
    rng ^= 0x9E3779B9u; if (!rng) rng = 0xC0FFEEu;
    for (int i = 0; i < 8; i++) rnd();                  /* stir before first use */
    load_best();
    new_board();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 27) return 0;              /* q or Esc: quit */
        if (k == 'n') { new_board(); render(); continue; }
        if (grabbed) {                                  /* a gem is grabbed: an arrow now swaps it */
            if      (k == 0x11) { try_swap(0, -1); }    /* up    */
            else if (k == 0x12) { try_swap(0,  1); }    /* down  */
            else if (k == 0x13) { try_swap(-1, 0); }    /* left  */
            else if (k == 0x14) { try_swap( 1, 0); }    /* right */
            else if (k == ' ' || k == '\n') { grabbed = 0; msg = 0; }   /* re-press: ungrab */
            else continue;
            render();
            continue;
        }
        /* not grabbed: arrows move the cursor, space/Enter grabs the gem under it */
        if      (k == 0x11) { if (cy > 0)      cy--; }      /* up    */
        else if (k == 0x12) { if (cy < ROWS-1) cy++; }      /* down  */
        else if (k == 0x13) { if (cx > 0)      cx--; }      /* left  */
        else if (k == 0x14) { if (cx < COLS-1) cx++; }      /* right */
        else if (k == ' ' || k == '\n') { grabbed = 1; msg = "swap with an arrow"; }
        else continue;
        render();
    }
}
