/*
 * vpoker.c — Video Poker (Jacks or Better).
 *
 * You're dealt five cards; toggle holds with keys 1-5, then press Space to draw
 * replacements for the rest. The final hand pays out on the classic 9/6
 * Jacks-or-Better table (a pair of jacks or better wins). Each deal costs one
 * credit; you start with 100 and your best balance is saved to VPOKER.HI.
 *
 * 1-5 hold/unhold, Space deal then draw, q quit.
 */
#include "ulib.h"

static int deck[52], pos;       /* shuffled deck + draw position */
static int hand[5];             /* current five cards (deck indices' card values) */
static int held[5];             /* 1 = keep this card on the draw */
static int credits, best, drawn, msg_pay;
static const char *msg;

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void putn(int n) {
    if (n < 0) { print("-"); n = -n; }
    char t[12]; int i = 0; if (n == 0) { print("0"); return; }
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    char s[12]; int j = 0; while (i) s[j++] = t[--i]; s[j] = 0; print(s);
}
static void load_hi(void){ char b[16]; long n=sys_readfile("VPOKER.HI",b,15); best=0; for(long i=0;i<n;i++){ if(b[i]<'0'||b[i]>'9')break; best=best*10+(b[i]-'0'); } if(best<100)best=100; }
static void save_hi(void){ char t[12],b[12]; int i=0,n=0,v=best; if(v==0)t[i++]='0'; while(v){t[i++]=(char)('0'+v%10);v/=10;} while(i)b[n++]=t[--i]; sys_writefile("VPOKER.HI",b,(unsigned long)n); }

/* card value 0..51: rank = 2 + v%13 (so 14 = ace), suit = v/13 (0=S 1=H 2=D 3=C). */
static int rank_of(int v) { return 2 + v % 13; }
static int suit_of(int v) { return v / 13; }

static void shuffle(void) {
    for (int i = 0; i < 52; i++) deck[i] = i;
    for (int i = 51; i > 0; i--) { int j = (int)(rnd() % (unsigned)(i + 1)); int t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
    pos = 0;
}
static void deal(void) {
    shuffle();
    for (int i = 0; i < 5; i++) { hand[i] = deck[pos++]; held[i] = 0; }
    drawn = 0; msg = "Hold cards (1-5), then Space"; msg_pay = 0;
}

/* Evaluate the five-card hand -> payout per 1 credit, and name it. */
static int score_hand(const char **name) {
    int cnt[15] = {0}, suit[4] = {0};
    for (int i = 0; i < 5; i++) { cnt[rank_of(hand[i])]++; suit[suit_of(hand[i])]++; }
    int flush = 0; for (int s = 0; s < 4; s++) if (suit[s] == 5) flush = 1;
    int pairs = 0, trips = 0, quads = 0, jbpair = 0, hi = 2, lo = 14;
    for (int r = 2; r <= 14; r++) {
        if (cnt[r] == 2) { pairs++; if (r >= 11 || r == 14) jbpair = 1; }
        if (cnt[r] == 3) trips++;
        if (cnt[r] == 4) quads++;
        if (cnt[r]) { if (r > hi) hi = r; if (r < lo) lo = r; }
    }
    int distinct = 0; for (int r = 2; r <= 14; r++) if (cnt[r]) distinct++;
    int straight = 0;
    if (distinct == 5) {
        if (hi - lo == 4) straight = 1;
        if (cnt[14] && cnt[2] && cnt[3] && cnt[4] && cnt[5]) straight = 1;   /* A-2-3-4-5 wheel */
    }
    int royal = flush && straight && cnt[14] && cnt[13] && cnt[10];          /* T-J-Q-K-A suited */

    if (royal)                 { *name = "ROYAL FLUSH!!";  return 250; }
    if (flush && straight)     { *name = "Straight flush"; return 50;  }
    if (quads)                 { *name = "Four of a kind"; return 25;  }
    if (trips && pairs)        { *name = "Full house";     return 9;   }
    if (flush)                 { *name = "Flush";          return 6;   }
    if (straight)              { *name = "Straight";       return 4;   }
    if (trips)                 { *name = "Three of a kind";return 3;   }
    if (pairs == 2)            { *name = "Two pair";       return 2;   }
    if (jbpair)                { *name = "Jacks or better";return 1;   }
    *name = "No win"; return 0;
}

static void draw_cards(void) {
    for (int i = 0; i < 5; i++) if (!held[i]) hand[i] = deck[pos++];
    const char *name; int pay = score_hand(&name);
    credits += pay;                              /* the 1-credit ante was taken at deal time */
    if (credits > best) { best = credits; save_hi(); }
    drawn = 1; msg = name; msg_pay = pay;
}

static void put_card(int v) {
    static const char *R = "  23456789TJQKA";   /* index by rank 2..14 */
    int r = rank_of(v), s = suit_of(v);
    sys_setcolor((s == 1 || s == 2) ? 12 : 15);  /* hearts/diamonds red, spades/clubs white */
    char c[5]; c[0] = '['; c[1] = R[r]; c[2] = "SHDC"[s]; c[3] = ']'; c[4] = 0;
    print(c); sys_setcolor(0);
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("  Video Poker"); sys_setcolor(0);
    print("    credits "); sys_setcolor(2); putn(credits); sys_setcolor(0);
    print("  best "); sys_setcolor(14); putn(best); sys_setcolor(0); print("\n\n");
    print("    1    2    3    4    5\n");
    print("  ");
    for (int i = 0; i < 5; i++) { put_card(hand[i]); print(" "); }
    print("\n  ");
    for (int i = 0; i < 5; i++) { sys_setcolor(10); print(held[i] ? "HELD " : " --  "); }
    sys_setcolor(0); print("\n\n  ");
    if (msg_pay > 0) sys_setcolor(2); else sys_setcolor(0);
    print(msg);
    if (drawn && msg_pay > 0) { print("  +"); putn(msg_pay); }
    sys_setcolor(0); print("\n\n");
    if (credits <= 0 && drawn) print("  Out of credits. Space = free 100\n");
    print("  1-5 hold   space ");
    print(drawn ? "deal" : "draw");
    print("   q quit\n");
}

int main(void) {
    rng = (unsigned)sys_uptime_ms() | 1u;
    load_hi();
    credits = 100;
    deal(); credits--;                 /* ante for the opening hand */
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 'Q') return 0;
        if (k >= '1' && k <= '5') {
            if (!drawn) { held[k - '1'] ^= 1; render(); }
        } else if (k == ' ') {
            if (!drawn) { draw_cards(); sys_beep(msg_pay > 0 ? 880 : 220, 90); }
            else {
                if (credits <= 0) credits = 100;     /* bust: comp another 100 to keep playing */
                deal(); credits--;                   /* ante for the next hand */
            }
            render();
        }
    }
}
