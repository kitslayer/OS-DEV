/*
 * solitaire.c — Klondike Solitaire (the classic "draw one" Patience).
 *
 * The full standard game: a 52-card deck dealt into seven tableau columns
 * (column i gets i+1 cards, only the top face-up), four foundations to build up
 * by suit A->K, and a stock you turn one card at a time into a waste pile
 * (recycling the waste back when the stock runs out). Win by getting all 52
 * cards onto the foundations.
 *
 * The OS font has no suit glyphs, so suits are letters — S/H/D/C — with Hearts
 * and Diamonds drawn red and Spades/Clubs grey, exactly like Blackjack (bj.c)
 * and Video Poker (vpoker.c) render their cards.
 *
 * Moves are made source-then-destination, with pile keys:
 *   1-7   pick a tableau column (source), then a destination
 *   w     pick the waste pile as the source
 *   space/d   draw the next card from stock to waste (recycles when empty)
 *   then, with a source picked:
 *     1-7   drop onto that tableau column (descending, alternating colour;
 *           a King onto an empty column). A tableau source moves the whole
 *           face-up run from the picked card down.
 *     f     send the source's top card up to its foundation (same suit, A->K)
 *   c / Esc-of-selection   cancel the current selection
 *   a     auto-play every card that can go to a foundation (one pass)
 *   n     deal a new game
 *   q / Esc   quit
 *
 * Every move is validated before it is applied — an illegal move is rejected
 * with a brief message and changes nothing. A tableau column whose top card is
 * face-down auto-flips face-up once it is exposed. All piles are bounded at 52
 * cards (the whole deck), so no array can be overrun.
 */
#include "ulib.h"

/* card 0..51: rank = c%13 (0=A,1=2,..,9=10,10=J,11=Q,12=K), suit = c/13 (0=S 1=H 2=D 3=C). */
static int rank_of(int c) { return c % 13; }
static int suit_of(int c) { return c / 13; }
static int is_red(int c)  { int s = suit_of(c); return s == 1 || s == 2; }   /* Hearts/Diamonds */

static const char *RANK[13] = { "A","2","3","4","5","6","7","8","9","10","J","Q","K" };
static const char  SUIT[4]  = { 'S','H','D','C' };

/* ---- piles (all bounded by the 52-card deck) ---- */
static int tab[7][52];      /* tableau columns: card values, bottom..top */
static int tup[7][52];      /* 1 = that tableau position is face-up */
static int tn[7];           /* cards in each tableau column */
static int fnd[4];          /* foundation height per suit (0=empty; n => A..n on the pile) */
static int stock[52], sn;   /* the stock (face-down draw pile) */
static int waste[52], wn;   /* the waste (turned-up cards; only the top is playable) */

static int sel;             /* current source: -1 none, 0-6 a tableau column, 7 = waste */
static int seli;            /* for a tableau source, the index of the picked card in its column */
static const char *msg;     /* one-line status / rejection message */
static int won;

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* ---- deal ---- */
static void deal(void) {
    int deck[52];
    for (int i = 0; i < 52; i++) deck[i] = i;
    for (int i = 51; i > 0; i--) {                       /* Fisher-Yates shuffle */
        int j = (int)(rnd() % (unsigned)(i + 1));
        int t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
    for (int i = 0; i < 4; i++) fnd[i] = 0;
    for (int c = 0; c < 7; c++) tn[c] = 0;
    wn = 0;
    int p = 0;
    for (int c = 0; c < 7; c++)                          /* column c gets c+1 cards */
        for (int r = 0; r <= c; r++) {
            tab[c][tn[c]] = deck[p];
            tup[c][tn[c]] = (r == c);                     /* only the last (top) card face-up */
            tn[c]++; p++;
        }
    sn = 0;                                              /* the remaining 24 cards -> stock */
    while (p < 52) stock[sn++] = deck[p++];
    sel = -1; seli = 0; won = 0;
    msg = "Pick a column (1-7), w=waste, space=draw.";
}

/* ---- rendering ---- */
/* Draw one card (rank+suit) in its suit colour; pads to 4 columns ("10H" = 3,
 * a single-rank card "QS " = 3, both then a trailing space => a 4-col cell). */
static void putcard(int c, int highlight) {
    sys_setcolor(highlight ? 3 : (is_red(c) ? 2 : 1));   /* selected = yellow, else red/white */
    print(RANK[rank_of(c)]);
    char sc[2] = { SUIT[suit_of(c)], 0 }; print(sc);
    sys_setcolor(0);
    /* pad so every card occupies the same width (rank "10" is 2 chars, others 1) */
    print(rank_of(c) == 9 ? " " : "  ");
}
static void putdown(void) { sys_setcolor(8); print("## ");  sys_setcolor(0); }  /* a face-down card */
static void putempty(void){ sys_setcolor(8); print("--  "); sys_setcolor(0); }  /* an empty slot   */

static void render(void) {
    sys_clear();
    sys_setcolor(4); print(" Klondike Solitaire"); sys_setcolor(0);
    if (won) { sys_setcolor(10); print("   YOU WIN!"); sys_setcolor(0); }
    print("\n");

    /* Top row: the four foundations, then stock + waste. */
    print(" F:");
    for (int s = 0; s < 4; s++) {
        if (fnd[s] == 0) putempty();                     /* empty foundation: a placeholder */
        else putcard((s * 13) + (fnd[s] - 1), 0);        /* top of foundation suit s */
    }
    print(" St:");
    if (sn > 0) putdown(); else putempty();              /* stock face-down (or empty) */
    print("Wt:");
    if (wn > 0) putcard(waste[wn - 1], sel == 7);        /* top of waste (the playable card) */
    else putempty();
    print("\n");

    /* Column headers (1..7), the picked source column marked with '>'. */
    print("  ");
    for (int c = 0; c < 7; c++) {
        char h[4]; h[0] = (sel == c) ? '>' : ' '; h[1] = (char)('1' + c); h[2] = ' '; h[3] = 0;
        sys_setcolor(sel == c ? 3 : 0); print(h); sys_setcolor(0);
    }
    print("\n");

    /* Tableau: one card per row, columns side by side. Find the tallest column,
     * but cap the rendered depth so the header/footer never scroll off the
     * visible text grid (data is never lost — only very deep stacks are
     * clipped), the same way freecell.c's own near-identical cascade loop
     * already does (M1640: this loop had no cap at all). */
    int maxn = 1;
    for (int c = 0; c < 7; c++) if (tn[c] > maxn) maxn = tn[c];
    int rows = maxn, clipped = 0;
    if (rows > 9) { rows = 9; clipped = 1; }
    for (int r = 0; r < rows; r++) {
        print("  ");
        for (int c = 0; c < 7; c++) {
            if (r < tn[c]) {
                if (!tup[c][r]) putdown();
                else putcard(tab[c][r], sel == c && r >= seli);   /* highlight the picked run */
            } else if (r == 0 && tn[c] == 0) {
                putempty();                              /* an empty column shows one slot */
            } else {
                print("    ");                           /* nothing here: 4 blank cols */
            }
        }
        print("\n");
    }
    if (clipped) { sys_setcolor(8); print(" (deep columns clipped)\n"); sys_setcolor(0); }

    /* Footer: status line + the key help. */
    print("\n "); sys_setcolor(7); print(msg); sys_setcolor(0);
    print("\n 1-7 col  w waste  space draw  f foundation\n c cancel  a auto  n new  q quit\n");
}

/* ---- move legality ---- */
/* Can `card` stack on tableau column `c` as a destination? Empty col wants a
 * King; otherwise the top card must be one rank higher and the opposite colour. */
static int can_to_tableau(int card, int c) {
    if (tn[c] == 0) return rank_of(card) == 12;          /* only a King onto an empty column */
    int top = tab[c][tn[c] - 1];
    return is_red(card) != is_red(top) && rank_of(card) == rank_of(top) - 1;
}
/* Can `card` go up to its foundation? A starts it; otherwise it must be the next
 * rank of the SAME suit already on that foundation. */
static int can_to_foundation(int card) {
    return rank_of(card) == fnd[suit_of(card)];          /* fnd[s] == next rank needed (0 => A) */
}

/* After removing the top card from tableau column `c`, flip the newly-exposed
 * card face-up if it was face-down. */
static void flip_expose(int c) {
    if (tn[c] > 0 && !tup[c][tn[c] - 1]) tup[c][tn[c] - 1] = 1;
}

static void check_win(void) {
    if (fnd[0] + fnd[1] + fnd[2] + fnd[3] == 52) {
        won = 1; msg = "All 52 home — you win!  (n = new game)";
        sys_beep(880, 120); sys_beep(1175, 120); sys_beep(1568, 160);
    }
}

/* ---- moves (each validates, then applies; returns 1 if the move happened) ---- */

/* Draw the next stock card to the waste; recycle the waste back when stock is empty. */
static void do_draw(void) {
    if (sn > 0) {
        waste[wn++] = stock[--sn];                       /* turn one card up */
        msg = "Drew a card.";
    } else if (wn > 0) {
        while (wn > 0) stock[sn++] = waste[--wn];         /* recycle: waste -> stock (order reversed) */
        msg = "Stock recycled from waste.";
    } else {
        msg = "Stock and waste are both empty.";
    }
    sel = -1;
}

/* Move the source's top card (or, for a tableau source, the picked face-up run)
 * onto tableau column `dst`. */
static int move_to_tableau(int dst) {
    if (sel == 7) {                                      /* waste -> tableau (one card) */
        if (wn == 0) { msg = "The waste is empty."; return 0; }
        if (!can_to_tableau(waste[wn - 1], dst)) { msg = "Illegal move."; return 0; }
        tab[dst][tn[dst]] = waste[wn - 1]; tup[dst][tn[dst]] = 1; tn[dst]++;
        wn--;
        return 1;
    }
    /* tableau -> tableau: move the run tab[sel][seli..tn-1] (it is a valid run by
     * construction — only face-up descending alternating cards are pickable). */
    int src = sel;
    if (src == dst) { msg = "Pick a different destination."; return 0; }
    int card = tab[src][seli];                           /* the bottom of the moved run */
    if (!can_to_tableau(card, dst)) { msg = "Illegal move."; return 0; }
    int cnt = tn[src] - seli;
    if (tn[dst] + cnt > 52) { msg = "Illegal move."; return 0; }   /* can't happen, but bound it */
    for (int i = 0; i < cnt; i++) {                      /* copy the run across, keeping order */
        tab[dst][tn[dst]] = tab[src][seli + i];
        tup[dst][tn[dst]] = 1;
        tn[dst]++;
    }
    tn[src] = seli;                                      /* shrink the source column */
    flip_expose(src);
    return 1;
}

/* Send the source's top card up to its foundation. */
static int move_to_foundation(void) {
    if (sel == 7) {                                      /* waste -> foundation */
        if (wn == 0) { msg = "The waste is empty."; return 0; }
        if (!can_to_foundation(waste[wn - 1])) { msg = "Illegal move."; return 0; }
        fnd[suit_of(waste[wn - 1])]++;
        wn--;
        return 1;
    }
    int src = sel;                                       /* tableau -> foundation (top card only) */
    if (tn[src] == 0) { msg = "That column is empty."; return 0; }
    if (seli != tn[src] - 1) { msg = "Only the top card goes to a foundation."; return 0; }
    int card = tab[src][tn[src] - 1];
    if (!can_to_foundation(card)) { msg = "Illegal move."; return 0; }
    fnd[suit_of(card)]++;
    tn[src]--;
    flip_expose(src);
    return 1;
}

/* One pass of auto-play: send every top card (waste + each tableau) that has a
 * legal foundation move home, repeating until nothing more can go up. */
static void auto_play(void) {
    int moved = 1, any = 0;
    while (moved) {
        moved = 0;
        if (wn > 0 && can_to_foundation(waste[wn - 1])) {
            fnd[suit_of(waste[wn - 1])]++; wn--; moved = 1; any = 1;
        }
        for (int c = 0; c < 7; c++) {
            if (tn[c] > 0 && tup[c][tn[c] - 1] && can_to_foundation(tab[c][tn[c] - 1])) {
                fnd[suit_of(tab[c][tn[c] - 1])]++; tn[c]--; flip_expose(c); moved = 1; any = 1;
            }
        }
    }
    msg = any ? "Auto-played to the foundations." : "Nothing could be auto-played.";
    sel = -1;
}

/* Pick a source pile. A tableau pick must land on a face-up card (the start of a
 * movable run); the waste pick needs a card on the waste. */
static void pick(int which) {
    if (which == 7) {                                    /* waste */
        if (wn == 0) { msg = "The waste is empty."; sel = -1; return; }
        sel = 7; seli = 0; msg = "Waste picked. Destination: 1-7 or f.";
        return;
    }
    int c = which;                                       /* tableau column 0..6 */
    if (tn[c] == 0) { msg = "That column is empty."; sel = -1; return; }
    int i = tn[c] - 1;                                   /* start the run at the topmost face-up card */
    while (i > 0 && tup[c][i - 1]) i--;
    sel = c; seli = i;
    msg = "Column picked. Destination: 1-7 or f (c=cancel).";
}

int main(void) {
    rng = (unsigned)sys_uptime_ms() | 1u;
    deal();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 'Q') break;
        if (k == 27) {                                   /* Esc: cancel a selection, else quit */
            if (sel != -1) { sel = -1; msg = "Selection cancelled."; render(); continue; }
            break;
        }
        if (k == 'n' || k == 'N') { deal(); render(); continue; }
        if (won) continue;                               /* game over: only n / q do anything */

        if (k == 'c' || k == 'C') { sel = -1; msg = "Selection cancelled."; render(); continue; }
        if (k == 'a' || k == 'A') { auto_play(); check_win(); render(); continue; }
        if (k == ' ' || k == 'd' || k == 'D') { do_draw(); render(); continue; }

        if (k == 'w' || k == 'W') { pick(7); render(); continue; }

        if (k >= '1' && k <= '7') {
            int c = k - '1';
            if (sel == -1) pick(c);                      /* no source yet: pick this column */
            else { if (move_to_tableau(c)) { sel = -1; check_win(); } }   /* else: it's the destination */
            render(); continue;
        }
        if (k == 'f' || k == 'F') {
            if (sel == -1) msg = "Pick a source first (1-7 or w).";
            else { if (move_to_foundation()) { sel = -1; check_win(); } }
            render(); continue;
        }
    }
    return 0;
}
