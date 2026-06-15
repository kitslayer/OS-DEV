/*
 * bj.c — Blackjack (21) against the dealer.
 *
 * A card game — a genre the app suite didn't have. Standard rules: hit until
 * you stand or bust (over 21); the dealer then reveals its hole card and draws
 * to 17. Aces count 11 or 1 (whichever avoids a bust); a two-card 21 is a
 * blackjack and pays 3:2. You start with 100 chips and bet 10 a hand. A 52-card
 * deck is Fisher-Yates shuffled from a clock-seeded xorshift PRNG.
 */
#include "ulib.h"

#define BET 10

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int deck[52], dp;          /* shuffled deck + draw pointer */
static int ph[16], pn;            /* player hand */
static int dh[16], dn;            /* dealer hand */
static int chips = 100;
static int state;                 /* 0 = ready to deal, 1 = player's turn, 2 = round over */
static int hide;                  /* dealer's hole card concealed during the player's turn */
static const char *msg;

static const char *RANKS[13] = { "A","2","3","4","5","6","7","8","9","10","J","Q","K" };
static const char SUITS[4]   = { 's','h','d','c' };
static int rankof(int c) { return c % 13; }
static int suitof(int c) { return c / 13; }
static int cardval(int c) { int r = rankof(c); if (r == 0) return 11; if (r >= 9) return 10; return r + 1; }

/* Hand total, demoting aces from 11 to 1 as needed to dodge a bust. */
static int total(const int *h, int n) {
    int sum = 0, aces = 0;
    for (int i = 0; i < n; i++) { sum += cardval(h[i]); if (rankof(h[i]) == 0) aces++; }
    while (sum > 21 && aces) { sum -= 10; aces--; }
    return sum;
}

static void shuffle(void) {
    for (int i = 0; i < 52; i++) deck[i] = i;
    for (int i = 51; i > 0; i--) { int j = (int)(rnd() % (unsigned)(i + 1)); int t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
    dp = 0;
}
static int draw(void) { if (dp >= 52) shuffle(); return deck[dp++]; }

static void printnum(int v) {
    char t[12]; int i = 0, n = v;
    if (n == 0) t[i++] = '0';
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    char o[12]; int j = 0; while (i) o[j++] = t[--i]; o[j] = 0;
    print(o);
}
static void putcard(int c) {
    int s = suitof(c);
    if (s == 1 || s == 2) sys_setcolor(2); else sys_setcolor(8);   /* hearts/diamonds red, else grey */
    print(RANKS[rankof(c)]);
    char sc[2] = { SUITS[s], 0 }; print(sc);
    sys_setcolor(0); print(" ");
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("\n  Blackjack"); sys_setcolor(0);
    print("    chips "); sys_setcolor(3); printnum(chips); sys_setcolor(0); print("\n\n");
    print("  Dealer: ");
    for (int i = 0; i < dn; i++) { if (i == 1 && hide) print("?? "); else putcard(dh[i]); }
    if (dn && !hide) { print(" ("); printnum(total(dh, dn)); print(")"); }
    print("\n  You:    ");
    for (int i = 0; i < pn; i++) putcard(ph[i]);
    if (pn) { print(" ("); printnum(total(ph, pn)); print(")"); }
    print("\n\n  "); print(msg);
    print("\n  d deal   h hit   s stand   r reset   q quit\n");
}

/* The player has stood (or has a natural): reveal, the dealer draws to 17, settle. */
static void finish(void) {
    hide = 0;
    int pt = total(ph, pn);
    if (pt > 21) { chips -= BET; msg = "BUST! you lose.   d = deal"; state = 2; sys_beep(196, 220); return; }
    while (total(dh, dn) < 17 && dn < 16) dh[dn++] = draw();
    int dt = total(dh, dn);
    int bj = (pt == 21 && pn == 2);
    if (dt > 21 || pt > dt) {
        int w = bj ? (BET * 3) / 2 : BET;
        chips += w;
        msg = bj ? "BLACKJACK! you win 3:2.   d = deal" : "You win!   d = deal";
        sys_beep(880, 160);
    } else if (pt < dt) {
        chips -= BET; msg = "Dealer wins.   d = deal"; sys_beep(196, 220);
    } else {
        msg = "Push (tie).   d = deal";
    }
    state = 2;
}

static void deal(void) {
    if (chips < BET) { msg = "Out of chips! r = reset"; state = 2; return; }
    shuffle();
    pn = dn = 0;
    ph[pn++] = draw(); dh[dn++] = draw(); ph[pn++] = draw(); dh[dn++] = draw();
    hide = 1; state = 1; msg = "h = hit, s = stand";
    if (total(ph, pn) == 21) finish();      /* a natural blackjack stands at once */
}

int main(void) {
    char tb[40]; long tn = sys_time(tb, sizeof(tb));     /* seed from the clock */
    rng = 0x2545F491u;
    for (long i = 0; i < tn; i++) rng = rng * 31u + (unsigned char)tb[i];
    if (!rng) rng = 12345u;

    chips = 100; state = 0; pn = dn = 0;
    msg = "Press d to deal (bet 10)";
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 'Q') break;
        if (k == 'r' || k == 'R') { chips = 100; state = 0; pn = dn = 0; msg = "Reset. d = deal."; render(); continue; }
        if ((k == 'd' || k == 'D') && state != 1) { deal(); render(); continue; }
        if (state == 1) {
            if (k == 'h' || k == 'H') {
                ph[pn++] = draw();
                if (total(ph, pn) > 21) finish(); else msg = "h = hit, s = stand";
                render();
            } else if (k == 's' || k == 'S') {
                finish(); render();
            }
        }
    }
    return 0;
}
