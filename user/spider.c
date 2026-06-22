/*
 * spider.c — Spider Solitaire (the one-suit "patience of patience").
 *
 * Spider is played with TWO 52-card decks — 104 cards. This is the classic
 * ONE-SUIT game: every card is a Spade, so the two decks are eight cards of
 * each rank A..K (8*13 = 104). Fifty-four cards are dealt into ten tableau
 * columns — the first four columns get six cards, the other six get five
 * (4*6 + 6*5 = 54) — with only the top card of each column face-up. The
 * remaining fifty cards form the STOCK, dealt ten at a time (five deals).
 *
 * The OS font has no suit glyphs, so the suit is a letter — S — drawn grey,
 * exactly like Klondike Solitaire (solitaire.c) and FreeCell (freecell.c)
 * render their cards. (Hearts/Diamonds would be red, but one-suit Spider is
 * all Spades.)
 *
 * Build DOWN within a column regardless of suit, but you can only MOVE a run
 * of cards together if it is same-suit and strictly descending. In one-suit
 * every face-up descending run is same-suit by definition, so any descending
 * face-up run is movable. A run lands on a column whose top card is exactly
 * one rank higher (any card, or run, may go onto an empty column). When the
 * cards above a face-down card are removed, that card auto-flips face-up. When
 * a full K->A same-suit run forms at the top of a column it is removed
 * automatically to the COMPLETED area; assembling all eight K->A runs wins.
 *
 * Moves are made source-then-destination, with pile keys:
 *   1-9, 0    pick a tableau column as the source (0 = column 10). It
 *             auto-selects the topmost valid descending run.
 *   then, with a source picked:
 *     1-9, 0      drop the picked run onto that column (top one rank higher,
 *                 or any column that is empty)
 *   space / d   deal one card face-up onto every column (only when no column
 *               is empty — the standard Spider rule); five deals total
 *   c           cancel the current selection
 *   n           deal a new game
 *   q / Esc     quit  (Esc first cancels a selection, if one is active)
 *
 * Every move is validated before it is applied — an illegal move is rejected
 * with a brief message and changes nothing, so an illegal stack can never
 * form. Each of the ten columns is bounded by the 104-card double deck
 * (col[10][104]) and every access is range-checked, so no array can be
 * overrun even when a column grows very deep.
 */
#include "ulib.h"

/* The deck is 104 cards: id 0..103. With one suit (Spades) the rank is id%13
 * (0=A,1=2,..,9=10,10=J,11=Q,12=K) and the suit is fixed at 0 (Spades). We keep
 * the same rank_of/suit_of/is_red model the other solitaires use so the card
 * rendering matches; a multi-suit variant would only change suit_of/is_red. */
static int rank_of(int c) { return c % 13; }
static int suit_of(int c) { (void)c; return 0; }            /* one-suit: always Spades */
static int is_red(int c)  { int s = suit_of(c); return s == 1 || s == 2; }   /* Hearts/Diamonds (never, here) */

static const char *RANK[13] = { "A","2","3","4","5","6","7","8","9","10","J","Q","K" };
static const char  SUIT[4]  = { 'S','H','D','C' };

#define NCOL  10                 /* ten tableau columns                  */
#define NCARD 104                /* two decks = 104 cards (the ceiling)  */
                                 /* (the 50-card stock makes five deals of ten) */

/* ---- piles (all bounded by the 104-card double deck) ---- */
static int col[NCOL][NCARD];     /* tableau columns: card values, bottom..top */
static int cup[NCOL][NCARD];     /* 1 = that column position is face-up       */
static int cn[NCOL];             /* number of cards in each column            */
static int stock[NCARD], sn;     /* the stock (face-down deal pile)           */
static int completed;            /* number of finished K->A runs (8 = win)    */

/* current source selection */
static int sel;                  /* -1 none, else 0..NCOL-1 the picked column */
static int seli;                 /* index of the bottom card of the picked run */
static const char *msg;          /* one-line status / rejection message       */
static int won;

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* ---- deal ---- */
static void deal(void) {
    /* Build the 104-card double deck: two single-suit (Spades) decks. With one
     * suit the card value is just its rank repeated; ranks 0..12 each appear
     * eight times. We store rank as the value (0..12) so rank_of works, and use
     * distinct array slots — eight copies of each rank fill 104 entries. */
    int deck[NCARD];
    for (int i = 0; i < NCARD; i++) deck[i] = i % 13;        /* rank only; suit is always Spades */
    for (int i = NCARD - 1; i > 0; i--) {                    /* Fisher-Yates shuffle */
        int j = (int)(rnd() % (unsigned)(i + 1));
        int t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
    for (int c = 0; c < NCOL; c++) cn[c] = 0;
    completed = 0;
    /* Deal 54 cards: columns 0..3 get six, columns 4..9 get five. Only the top
     * card of each column is face-up. */
    int p = 0;
    for (int c = 0; c < NCOL; c++) {
        int want = (c < 4) ? 6 : 5;
        for (int r = 0; r < want; r++) {
            col[c][cn[c]] = deck[p];
            cup[c][cn[c]] = (r == want - 1);                 /* only the last (top) card face-up */
            cn[c]++; p++;
        }
    }
    sn = 0;                                                  /* remaining 50 cards -> stock */
    while (p < NCARD) stock[sn++] = deck[p++];
    sel = -1; seli = 0; won = 0;
    msg = "Pick a column (1-9,0), space=deal.";
}

/* ---- rendering ---- */
/* Draw one card (rank+suit) in its suit colour; every card occupies 4 columns
 * ("10S" is 3 chars + 1 trailing space; "QS" is 2 chars + 2 trailing spaces). */
static void putcard(int c, int highlight) {
    sys_setcolor(highlight ? 3 : (is_red(c) ? 2 : 1));       /* selected = yellow, else red/white */
    print(RANK[rank_of(c)]);
    char sc[2] = { SUIT[suit_of(c)], 0 }; print(sc);
    sys_setcolor(0);
    print(rank_of(c) == 9 ? " " : "  ");                     /* pad: rank "10" is 2 chars, others 1 */
}
static void putdown(void) { sys_setcolor(8); print("## ");  sys_setcolor(0); }  /* a face-down card */
static void putempty(void){ sys_setcolor(8); print("--  "); sys_setcolor(0); }  /* an empty slot   */

/* Print a small non-negative integer (the stock count is at most 50). */
static void putnum(int n) {
    char b[12];
    int i = 11;
    b[i] = 0;
    if (n == 0) b[--i] = '0';
    while (n > 0) { b[--i] = (char)('0' + n % 10); n /= 10; }
    print(b + i);
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print(" Spider Solitaire"); sys_setcolor(0);
    if (won) { sys_setcolor(10); print("   YOU WIN!"); sys_setcolor(0); }
    print("\n");

    /* Top row: stock (cards remaining to deal) and the completed-set count. */
    print(" Stock:"); sys_setcolor(7); putnum(sn); sys_setcolor(0);
    print(" ("); putnum(sn / NCOL); print(" deals)  Done:");
    sys_setcolor(10); putnum(completed); print("/8"); sys_setcolor(0);
    print("\n");

    /* Column headers (1..9,0), the picked source column marked with '>'. */
    print(" ");
    for (int c = 0; c < NCOL; c++) {
        char lbl = (c == 9) ? '0' : (char)('1' + c);
        char h[4]; h[0] = (sel == c) ? '>' : ' '; h[1] = lbl; h[2] = ' '; h[3] = 0;
        sys_setcolor(sel == c ? 3 : 0); print(h); sys_setcolor(0);
    }
    print("\n");

    /* Tableau: one card per row, columns side by side. Find the tallest column,
     * but cap the rendered depth so the header/footer never scroll off the
     * 17-row text grid (data is never lost — only very deep stacks are clipped). */
    int maxn = 1;
    for (int c = 0; c < NCOL; c++) if (cn[c] > maxn) maxn = cn[c];
    int rows = maxn, clipped = 0;
    if (rows > 11) { rows = 11; clipped = 1; }               /* leave room for title/top/header/status/help */
    for (int r = 0; r < rows; r++) {
        print(" ");
        for (int c = 0; c < NCOL; c++) {
            if (r < cn[c]) {
                if (!cup[c][r]) putdown();
                else putcard(col[c][r], sel == c && r >= seli);   /* highlight the picked run */
            } else if (r == 0 && cn[c] == 0) {
                putempty();                                  /* an empty column shows one slot */
            } else {
                print("    ");                               /* nothing here: 4 blank cols */
            }
        }
        print("\n");
    }
    if (clipped) { sys_setcolor(8); print(" (deep columns clipped)\n"); sys_setcolor(0); }

    /* Footer: status line + the key help. */
    print(" "); sys_setcolor(7); print(msg); sys_setcolor(0);
    print("\n 1-9,0 column  space deal  c cancel\n n new  q quit\n");
}

/* ---- run / move legality ---- */

/* Is col[c][i..top] a contiguous descending SAME-SUIT run? (one-suit Spider:
 * same-suit is automatic, so this just checks each step descends by one rank.) */
static int is_run(int c, int i) {
    if (i < 0 || i >= cn[c]) return 0;
    for (int k = i; k + 1 < cn[c]; k++) {
        int a = col[c][k], b = col[c][k + 1];
        if (!(rank_of(b) == rank_of(a) - 1 && suit_of(a) == suit_of(b))) return 0;
    }
    return 1;
}

/* Can `card` (the bottom of a moved run) land on column `c`? An empty column
 * takes any card; otherwise the top card must be exactly one rank higher.
 * (Spider builds down regardless of suit — only the moved RUN must be same-suit,
 * which is enforced by is_run, not here.) */
static int can_to_col(int card, int c) {
    if (cn[c] == 0) return 1;                                /* any card/run onto an empty column */
    int top = col[c][cn[c] - 1];
    return rank_of(card) == rank_of(top) - 1;
}

/* After cards are removed from the top of column `c`, flip the newly-exposed
 * card face-up if it was face-down. */
static void flip_expose(int c) {
    if (cn[c] > 0 && !cup[c][cn[c] - 1]) cup[c][cn[c] - 1] = 1;
}

/* If the top of column `c` is a complete face-up K..A same-suit run (13 cards),
 * remove it to the completed area and flip whatever it exposed. Returns 1 if a
 * set was completed. */
static int try_complete(int c) {
    if (cn[c] < 13) return 0;
    int base = cn[c] - 13;                                   /* the King's position if this is a full run */
    if (!cup[c][base]) return 0;                             /* the whole run must be face-up */
    if (rank_of(col[c][base]) != 12) return 0;               /* top-of-run must be a King */
    if (!is_run(c, base)) return 0;                          /* and a clean K,Q,...,A descending run */
    cn[c] = base;                                            /* lift the 13 cards off the column */
    completed++;
    flip_expose(c);
    return 1;
}

static void check_win(void) {
    if (completed >= 8) {
        won = 1; msg = "All 8 sequences home — you win!  (n = new game)";
        sys_beep(880, 120); sys_beep(1175, 120); sys_beep(1568, 160);
    }
}

/* ---- moves (each validates, then applies; returns 1 if the move happened) ---- */

/* Move the picked run col[sel][seli..top] onto column `dst`. */
static int move_to_col(int dst) {
    int src = sel;
    if (src < 0 || src >= NCOL) { msg = "Pick a source first."; return 0; }
    if (src == dst) { msg = "Pick a different destination."; return 0; }
    if (cn[src] == 0) { msg = "That column is empty."; return 0; }
    if (!cup[src][seli]) { msg = "Not a movable run."; return 0; }
    if (!is_run(src, seli)) { msg = "Not a movable run."; return 0; }   /* must be descending same-suit */
    int bottom = col[src][seli];                             /* the card that lands on dst */
    if (!can_to_col(bottom, dst)) { msg = "Illegal move."; return 0; }
    int cnt = cn[src] - seli;                                /* how many cards in the picked run */
    if (cn[dst] + cnt > NCARD) { msg = "Illegal move."; return 0; }     /* can't happen, but bound it */
    for (int i = 0; i < cnt; i++) {                          /* copy the run across, keeping order */
        col[dst][cn[dst]] = col[src][seli + i];
        cup[dst][cn[dst]] = 1;
        cn[dst]++;
    }
    cn[src] = seli;                                          /* shrink the source column */
    flip_expose(src);
    /* A move can complete a sequence on either the destination (a new K..A run)
     * or, in odd cases, expose one on the source — check both. */
    if (try_complete(dst)) { msg = "Sequence completed!"; }
    else if (try_complete(src)) { msg = "Sequence completed!"; }
    else msg = "Moved.";
    return 1;
}

/* Deal one card face-up onto every column. Standard Spider forbids this while
 * any column is empty. Five deals empty the 50-card stock. */
static void do_deal(void) {
    if (sn < NCOL) { msg = "The stock is empty."; sel = -1; return; }
    for (int c = 0; c < NCOL; c++)
        if (cn[c] == 0) { msg = "Fill every empty column before dealing."; sel = -1; return; }
    for (int c = 0; c < NCOL; c++) {
        col[c][cn[c]] = stock[--sn];
        cup[c][cn[c]] = 1;
        cn[c]++;
    }
    /* A fresh card can complete a run at the top of a column. */
    int any = 0;
    for (int c = 0; c < NCOL; c++) if (try_complete(c)) any = 1;
    msg = any ? "Dealt a row — sequence completed!" : "Dealt a card to every column.";
    sel = -1;
}

/* Pick a source column: auto-select the topmost valid descending same-suit run
 * (extend the run downward from the top while each step still descends by one). */
static void pick(int c) {
    if (cn[c] == 0) { msg = "That column is empty."; sel = -1; return; }
    int i = cn[c] - 1;                                       /* start at the top face-up card */
    if (!cup[c][i]) { msg = "That column's top card is face-down."; sel = -1; return; }
    while (i > 0 && cup[c][i - 1]) {                         /* extend down while still a run */
        int a = col[c][i - 1], b = col[c][i];
        if (rank_of(b) == rank_of(a) - 1 && suit_of(a) == suit_of(b)) i--;
        else break;
    }
    sel = c; seli = i;
    msg = "Column picked. Destination: 1-9,0 (c=cancel).";
}

/* Map a column key 1-9,0 to a column index 0..9 (0 selects column 10). */
static int col_key_idx(int k) {
    if (k >= '1' && k <= '9') return k - '1';
    if (k == '0') return 9;
    return -1;
}

int main(void) {
    rng = (unsigned)sys_uptime_ms() | 1u;
    deal();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 'Q') break;
        if (k == 27) {                                       /* Esc: cancel a selection, else quit */
            if (sel != -1) { sel = -1; msg = "Selection cancelled."; render(); continue; }
            break;
        }
        if (k == 'n' || k == 'N') { deal(); render(); continue; }
        if (won) continue;                                   /* game over: only n / q do anything */

        if (k == 'c' || k == 'C') { sel = -1; msg = "Selection cancelled."; render(); continue; }
        if (k == ' ' || k == 'd' || k == 'D') { do_deal(); check_win(); render(); continue; }

        int c = col_key_idx(k);
        if (c >= 0) {
            if (sel == -1) pick(c);                          /* no source yet: pick this column */
            else { if (move_to_col(c)) { sel = -1; check_win(); } }   /* else it's the destination */
            render(); continue;
        }
    }
    return 0;
}
