/*
 * freecell.c — FreeCell, the open-information solitaire.
 *
 * The whole 52-card deck is dealt FACE-UP into eight cascade columns (the first
 * four get seven cards, the last four get six — 4*7 + 4*6 = 52). There is no
 * stock and no hidden card: every card is visible from the start, which is why
 * almost every deal is winnable with enough thought. Above the cascades sit
 * four FREE CELLS (each parks at most one card) and four FOUNDATIONS (built up
 * by suit A->K). Win by moving all 52 cards onto the foundations.
 *
 * The OS font has no suit glyphs, so suits are letters — S/H/D/C — with Hearts
 * and Diamonds drawn red and Spades/Clubs grey, exactly like Klondike Solitaire
 * (solitaire.c), Blackjack (bj.c) and Video Poker (vpoker.c) render their cards.
 *
 * Moves are made source-then-destination. A source is a cascade or a free cell:
 *   1-8       pick a cascade column (source), then pick a destination
 *   q w e r   pick free cell 1/2/3/4 as the source
 *   then, with a source picked:
 *     1-8         move onto that cascade. A single card stacks descending and
 *                 alternating-colour (or any card onto an empty column). A
 *                 cascade source may move a whole descending alternating-colour
 *                 RUN as a supermove, up to (1 + free cells) * 2^(empty cols)
 *                 cards (the classic FreeCell limit).
 *     q w e r     park the source's single top card in that free cell (if empty)
 *     f           send the source's top card to its foundation (same suit, A->K)
 *   c             cancel the current selection
 *   a             auto-play every card that can go to a foundation
 *   n             deal a new game
 *   Q / Esc       quit  (Esc first cancels a selection, if one is active)
 *
 * Every move is validated before it is applied — an illegal move is rejected
 * with a brief message and changes nothing, so an illegal stack can never form.
 * All cascades, free cells and foundations are bounded by the 52-card deck, so
 * no array can be overrun.
 */
#include "ulib.h"

/* card 0..51: rank = c%13 (0=A,1=2,..,9=10,10=J,11=Q,12=K), suit = c/13 (0=S 1=H 2=D 3=C). */
static int rank_of(int c) { return c % 13; }
static int suit_of(int c) { return c / 13; }
static int is_red(int c)  { int s = suit_of(c); return s == 1 || s == 2; }   /* Hearts/Diamonds */

static const char *RANK[13] = { "A","2","3","4","5","6","7","8","9","10","J","Q","K" };
static const char  SUIT[4]  = { 'S','H','D','C' };

#define NCASC 8                 /* eight cascade columns           */
#define NFREE 4                 /* four free cells                 */
#define NFND  4                 /* four foundations (one per suit) */

/* ---- piles (all bounded by the 52-card deck) ---- */
static int casc[NCASC][52];     /* cascade columns: card values, bottom..top */
static int cn[NCASC];           /* number of cards in each cascade */
static int freec[NFREE];        /* free cells: a card value, or -1 if empty */
static int fnd[NFND];           /* foundation height per suit (0=empty; n => A..n on the pile) */

/* current source selection */
static int sel;                 /* -1 none, 0..NCASC-1 a cascade, NCASC..NCASC+NFREE-1 a free cell */
static int seli;                /* for a cascade source: index of the bottom card of the picked run */
static const char *msg;         /* one-line status / rejection message */
static int won;

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* selection helpers: a free-cell source is encoded as sel = NCASC + cell index */
static int sel_is_free(void) { return sel >= NCASC && sel < NCASC + NFREE; }
static int sel_free_idx(void){ return sel - NCASC; }

/* ---- deal ---- */
static void deal(void) {
    int deck[52];
    for (int i = 0; i < 52; i++) deck[i] = i;
    for (int i = 51; i > 0; i--) {                       /* Fisher-Yates shuffle */
        int j = (int)(rnd() % (unsigned)(i + 1));
        int t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
    for (int i = 0; i < NFND;  i++) fnd[i]   = 0;
    for (int i = 0; i < NFREE; i++) freec[i] = -1;
    for (int c = 0; c < NCASC; c++) cn[c] = 0;
    /* Deal round-robin across the 8 columns: dealing one card to each column in
     * turn leaves columns 0..3 with 7 cards and columns 4..7 with 6 (= 52). */
    int p = 0;
    while (p < 52) {
        int c = p % NCASC;
        casc[c][cn[c]] = deck[p];
        cn[c]++;
        p++;
    }
    sel = -1; seli = 0; won = 0;
    msg = "Pick a cascade (1-8) or a free cell (q w e r).";
}

/* ---- rendering ---- */
/* Draw one card (rank+suit) in its suit colour; every card occupies 4 columns
 * ("10H" is 3 chars + 1 trailing space; "QS" is 2 chars + 2 trailing spaces). */
static void putcard(int c, int highlight) {
    sys_setcolor(highlight ? 3 : (is_red(c) ? 2 : 1));   /* selected = yellow, else red/white */
    print(RANK[rank_of(c)]);
    char sc[2] = { SUIT[suit_of(c)], 0 }; print(sc);
    sys_setcolor(0);
    print(rank_of(c) == 9 ? " " : "  ");                 /* pad: rank "10" is 2 chars, others 1 */
}
static void putempty(void){ sys_setcolor(8); print("--  "); sys_setcolor(0); }  /* an empty slot */

static void render(void) {
    sys_clear();
    sys_setcolor(4); print(" FreeCell"); sys_setcolor(0);
    if (won) { sys_setcolor(10); print("   YOU WIN!"); sys_setcolor(0); }
    print("\n");

    /* Top row: the four free cells, then the four foundations. */
    print(" Free:");
    for (int i = 0; i < NFREE; i++) {
        if (freec[i] < 0) putempty();
        else putcard(freec[i], sel_is_free() && sel_free_idx() == i);
    }
    print(" Fnd:");
    for (int s = 0; s < NFND; s++) {
        if (fnd[s] == 0) putempty();                     /* empty foundation placeholder */
        else putcard((s * 13) + (fnd[s] - 1), 0);        /* top of foundation suit s */
    }
    print("\n");

    /* Free-cell key labels (q w e r) and foundation suit labels, aligned under
     * the row above so the player can see which key parks/builds where. */
    sys_setcolor(8);
    print("       q   w   e   r        S   H   D   C\n");
    sys_setcolor(0);

    /* Cascade headers 1..8, the picked source column marked with '>'. */
    print(" ");
    for (int c = 0; c < NCASC; c++) {
        char h[4]; h[0] = (sel == c) ? '>' : ' '; h[1] = (char)('1' + c); h[2] = ' '; h[3] = 0;
        sys_setcolor(sel == c ? 3 : 0); print(h); sys_setcolor(0);
    }
    print("\n");

    /* Cascades: one card per row, columns side by side. Find the tallest column,
     * but cap the rendered depth so the header/footer never scroll off the
     * 17-row text grid (data is never lost — only very deep stacks are clipped). */
    int maxn = 1;
    for (int c = 0; c < NCASC; c++) if (cn[c] > maxn) maxn = cn[c];
    int rows = maxn, clipped = 0;
    if (rows > 9) { rows = 9; clipped = 1; }             /* leave room for title/top/labels/header/status/help */
    for (int r = 0; r < rows; r++) {
        print(" ");
        for (int c = 0; c < NCASC; c++) {
            if (r < cn[c]) {
                putcard(casc[c][r], sel == c && r >= seli);   /* highlight the picked run */
            } else if (r == 0 && cn[c] == 0) {
                putempty();                              /* an empty column shows one slot */
            } else {
                print("    ");                           /* nothing here: 4 blank cols */
            }
        }
        print("\n");
    }
    if (clipped) { sys_setcolor(8); print(" (deep columns clipped)\n"); sys_setcolor(0); }

    /* Footer: status line + the key help. */
    print(" "); sys_setcolor(7); print(msg); sys_setcolor(0);
    print("\n 1-8 cascade  qwer cell  f found  a auto\n c cancel  n new  Q quit\n");
}

/* ---- move legality ---- */
/* Can `card` stack on cascade column `c` as a destination? Empty column takes
 * any card; otherwise the top card must be one rank higher and opposite colour. */
static int can_to_cascade(int card, int c) {
    if (cn[c] == 0) return 1;                            /* any card onto an empty cascade */
    int top = casc[c][cn[c] - 1];
    return is_red(card) != is_red(top) && rank_of(card) == rank_of(top) - 1;
}
/* Can `card` go up to its foundation? A starts it; otherwise it must be the next
 * rank of the SAME suit already on that foundation. */
static int can_to_foundation(int card) {
    return rank_of(card) == fnd[suit_of(card)];          /* fnd[s] == next rank needed (0 => A) */
}

/* Is casc[c][i..top] a valid descending, alternating-colour run? */
static int is_run(int c, int i) {
    for (int k = i; k + 1 < cn[c]; k++) {
        int a = casc[c][k], b = casc[c][k + 1];
        if (!(rank_of(b) == rank_of(a) - 1 && is_red(a) != is_red(b))) return 0;
    }
    return 1;
}

/* Maximum number of cards that may be moved cascade->cascade in one supermove:
 * (1 + free cells) * 2^(empty cascades). When the destination is itself an
 * empty column it can't double the multiplier, so it is excluded from the count. */
static int max_supermove(int dst) {
    int freecells = 0, empties = 0;
    for (int i = 0; i < NFREE; i++) if (freec[i] < 0) freecells++;
    for (int c = 0; c < NCASC; c++) if (cn[c] == 0 && c != dst) empties++;
    int m = (1 + freecells);
    for (int e = 0; e < empties; e++) m *= 2;            /* * 2^empties */
    return m;
}

static void check_win(void) {
    if (fnd[0] + fnd[1] + fnd[2] + fnd[3] == 52) {
        won = 1; msg = "All 52 home — you win!  (n = new game)";
        sys_beep(880, 120); sys_beep(1175, 120); sys_beep(1568, 160);
    }
}

/* ---- moves (each validates, then applies; returns 1 if the move happened) ---- */

/* Move the picked source onto cascade `dst`. A free-cell source moves its single
 * card; a cascade source moves the picked run casc[sel][seli..top] as a supermove. */
static int move_to_cascade(int dst) {
    if (sel_is_free()) {                                 /* free cell -> cascade (one card) */
        int fi = sel_free_idx();
        int card = freec[fi];
        if (card < 0) { msg = "That free cell is empty."; return 0; }
        if (!can_to_cascade(card, dst)) { msg = "Illegal move."; return 0; }
        casc[dst][cn[dst]++] = card;
        freec[fi] = -1;
        return 1;
    }
    int src = sel;                                       /* cascade -> cascade */
    if (src < 0 || src >= NCASC) { msg = "Pick a source first."; return 0; }
    if (src == dst) { msg = "Pick a different destination."; return 0; }
    if (cn[src] == 0) { msg = "That column is empty."; return 0; }
    int cnt = cn[src] - seli;                            /* how many cards in the picked run */
    int bottom = casc[src][seli];                        /* the card that lands on dst */
    if (!is_run(src, seli)) { msg = "Not a movable run."; return 0; }     /* must be a valid run */
    if (!can_to_cascade(bottom, dst)) { msg = "Illegal move."; return 0; }
    if (cnt > max_supermove(dst)) { msg = "Not enough free cells/columns for that run."; return 0; }
    if (cn[dst] + cnt > 52) { msg = "Illegal move."; return 0; }          /* can't happen, but bound it */
    for (int i = 0; i < cnt; i++)                        /* copy the run across, keeping order */
        casc[dst][cn[dst]++] = casc[src][seli + i];
    cn[src] = seli;                                      /* shrink the source column */
    return 1;
}

/* Park the source's single top card into free cell `fi`. Only a single card may
 * be parked — for a cascade source the picked card must be the top of its column. */
static int move_to_free(int fi) {
    if (freec[fi] >= 0) { msg = "That free cell is occupied."; return 0; }
    if (sel_is_free()) {                                 /* free cell -> free cell (just relocates) */
        int s = sel_free_idx();
        if (freec[s] < 0) { msg = "That free cell is empty."; return 0; }
        freec[fi] = freec[s]; freec[s] = -1;
        return 1;
    }
    int src = sel;                                       /* cascade -> free cell (top card only) */
    if (src < 0 || src >= NCASC) { msg = "Pick a source first."; return 0; }
    if (cn[src] == 0) { msg = "That column is empty."; return 0; }
    if (seli != cn[src] - 1) { msg = "Only one card can go to a free cell."; return 0; }
    freec[fi] = casc[src][cn[src] - 1];
    cn[src]--;
    return 1;
}

/* Send the source's top card up to its foundation. */
static int move_to_foundation(void) {
    if (sel_is_free()) {                                 /* free cell -> foundation */
        int fi = sel_free_idx();
        int card = freec[fi];
        if (card < 0) { msg = "That free cell is empty."; return 0; }
        if (!can_to_foundation(card)) { msg = "Illegal move."; return 0; }
        fnd[suit_of(card)]++;
        freec[fi] = -1;
        return 1;
    }
    int src = sel;                                       /* cascade -> foundation (top card only) */
    if (src < 0 || src >= NCASC) { msg = "Pick a source first."; return 0; }
    if (cn[src] == 0) { msg = "That column is empty."; return 0; }
    if (seli != cn[src] - 1) { msg = "Only the top card goes to a foundation."; return 0; }
    int card = casc[src][cn[src] - 1];
    if (!can_to_foundation(card)) { msg = "Illegal move."; return 0; }
    fnd[suit_of(card)]++;
    cn[src]--;
    return 1;
}

/* One pass of auto-play: send every top card (each free cell + each cascade) that
 * has a legal foundation move home, repeating until nothing more can go up. */
static void auto_play(void) {
    int moved = 1, any = 0;
    while (moved) {
        moved = 0;
        for (int i = 0; i < NFREE; i++) {
            if (freec[i] >= 0 && can_to_foundation(freec[i])) {
                fnd[suit_of(freec[i])]++; freec[i] = -1; moved = 1; any = 1;
            }
        }
        for (int c = 0; c < NCASC; c++) {
            if (cn[c] > 0 && can_to_foundation(casc[c][cn[c] - 1])) {
                fnd[suit_of(casc[c][cn[c] - 1])]++; cn[c]--; moved = 1; any = 1;
            }
        }
    }
    msg = any ? "Auto-played to the foundations." : "Nothing could be auto-played.";
    sel = -1;
}

/* Pick a source. A cascade pick starts the movable run at the deepest card that
 * still forms a valid descending alternating-colour run with everything above it. */
static void pick_cascade(int c) {
    if (cn[c] == 0) { msg = "That column is empty."; sel = -1; return; }
    int i = cn[c] - 1;                                   /* extend the run downward while it stays valid */
    while (i > 0) {
        int a = casc[c][i - 1], b = casc[c][i];
        if (rank_of(b) == rank_of(a) - 1 && is_red(a) != is_red(b)) i--;
        else break;
    }
    sel = c; seli = i;
    msg = "Cascade picked. Destination: 1-8, qwer, or f (c=cancel).";
}
static void pick_free(int fi) {
    if (freec[fi] < 0) { msg = "That free cell is empty."; sel = -1; return; }
    sel = NCASC + fi; seli = 0;
    msg = "Free cell picked. Destination: 1-8, qwer, or f.";
}

/* Map a free-cell key q/w/e/r -> cell index 0..3 (the four cells, left to right). */
static int free_key_idx(int k) {
    if (k == 'q') return 0;
    if (k == 'w') return 1;
    if (k == 'e') return 2;
    if (k == 'r') return 3;
    return -1;
}

int main(void) {
    rng = (unsigned)sys_uptime_ms() | 1u;
    deal();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'Q') break;                             /* uppercase Q quits (lowercase q = free cell 1) */
        if (k == 27) {                                   /* Esc: cancel a selection, else quit */
            if (sel != -1) { sel = -1; msg = "Selection cancelled."; render(); continue; }
            break;
        }
        if (k == 'n' || k == 'N') { deal(); render(); continue; }
        if (won) continue;                               /* game over: only n / Q do anything */

        if (k == 'c' || k == 'C') { sel = -1; msg = "Selection cancelled."; render(); continue; }
        if (k == 'a' || k == 'A') { auto_play(); check_win(); render(); continue; }

        if (k >= '1' && k <= '8') {
            int c = k - '1';
            if (sel == -1) pick_cascade(c);              /* no source yet: pick this cascade */
            else { if (move_to_cascade(c)) { sel = -1; check_win(); } }   /* else it's the destination */
            render(); continue;
        }

        int fi = free_key_idx(k);
        if (fi >= 0) {
            if (sel == -1) pick_free(fi);                /* no source: pick this free cell */
            else { if (move_to_free(fi)) { sel = -1; check_win(); } }     /* else park into this cell */
            render(); continue;
        }

        if (k == 'f' || k == 'F') {
            if (sel == -1) msg = "Pick a source first (1-8 or q w e r).";
            else { if (move_to_foundation()) { sel = -1; check_win(); } }
            render(); continue;
        }
    }
    return 0;
}
