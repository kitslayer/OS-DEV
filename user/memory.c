/*
 * memory.c — Memory / Concentration.
 *
 * A 4x4 grid of 8 face-down pairs (letters A-H). Flip two cards a turn: a match
 * stays face-up, otherwise both flip back. Clear the board in as few turns as
 * you can.
 *
 * Arrows move the cursor, Space flips the card under it.
 */
#include "ulib.h"

#define R 4
#define C 4
#define NCARD (R * C)

static int card[NCARD];             /* 0..7 value (each appears twice) */
static int matched[NCARD];          /* 1 = found */
static int cr, cc, first, reveal2;  /* first/second selected index, or -1 */
static int pairs, tries, won;

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void putn(int n) {
    char t[8]; int i = 0;
    if (n == 0) { print("0"); return; }
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    char s[8]; int j = 0; while (i) s[j++] = t[--i]; s[j] = 0; print(s);
}

static void deal(void) {
    for (int i = 0; i < NCARD; i++) { card[i] = i / 2; matched[i] = 0; }
    for (int i = NCARD - 1; i > 0; i--) {            /* Fisher-Yates shuffle */
        int j = (int)(rnd() % (unsigned)(i + 1));
        int t = card[i]; card[i] = card[j]; card[j] = t;
    }
    cr = cc = 0; first = reveal2 = -1; pairs = tries = won = 0;
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("\n  Memory"); sys_setcolor(0);
    print("       pairs "); sys_setcolor(2); putn(pairs); sys_setcolor(0);
    print("/8   tries "); putn(tries); print("\n\n");
    for (int r = 0; r < R; r++) {
        print("      ");
        for (int c = 0; c < C; c++) {
            int i = r * C + c, cur = (r == cr && c == cc);
            int face = matched[i] || i == first || i == reveal2;
            char ch = face ? (char)('A' + card[i]) : '?';
            sys_setcolor(matched[i] ? 10 : face ? 14 : 8);
            char cell[5];
            cell[0] = cur ? '[' : ' '; cell[1] = ch; cell[2] = ch;
            cell[3] = cur ? ']' : ' '; cell[4] = 0;
            print(cell);
        }
        sys_setcolor(0); print("\n\n");
    }
    print("  ");
    if (won) { sys_setcolor(10); print("Cleared in "); putn(tries); print(" tries!  (n = new)"); sys_setcolor(0); }
    else print("arrows move  space flip");
    print("\n  n new   q quit\n");
}

static void flip(void) {
    int i = cr * C + cc;
    if (matched[i] || i == first) return;
    if (first < 0) { first = i; render(); return; }
    reveal2 = i; render();                           /* show both */
    tries++;
    if (card[first] == card[i]) {
        matched[first] = matched[i] = 1; pairs++;
        sys_beep(880, 80);
        if (pairs == 8) { won = 1; sys_beep(1320, 200); }
    } else {
        sys_sleep(750);                              /* a beat to memorise, then flip back */
    }
    first = reveal2 = -1;
    render();
}

int main(void) {
    rng = (unsigned)sys_uptime_ms() | 1u;
    deal();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 'Q') break;
        if (k == 'n' || k == 'N') { deal(); render(); continue; }
        if (won) continue;
        if      (k == 0x11) { if (cr > 0) cr--; render(); }
        else if (k == 0x12) { if (cr < R - 1) cr++; render(); }
        else if (k == 0x13) { if (cc > 0) cc--; render(); }
        else if (k == 0x14) { if (cc < C - 1) cc++; render(); }
        else if (k == ' ' || k == '\n' || k == '\r') flip();
    }
    return 0;
}
