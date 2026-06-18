/*
 * mancala.c — Mancala (Kalah) against the computer.
 *
 * The classic sow-and-capture game. Pick one of your six pits (1-6); its seeds
 * are sown one per pit counterclockwise (skipping the opponent's store). Land
 * the last seed in your own store and you go again; land it in an empty pit on
 * your side and you capture that seed plus everything in the pit opposite, into
 * your store. When one side empties, the other sweeps its remaining seeds; most
 * seeds in your store wins.
 *
 * Board indices: 0-5 your pits, 6 your store, 7-12 the CPU's pits, 13 its store.
 * Keys 1-6 sow, r restarts, q quits.
 */
#include "ulib.h"

static int p[14];
static int over;
static const char *msg;

static void reset(void) {
    for (int i = 0; i < 14; i++) p[i] = 4;
    p[6] = p[13] = 0;
    over = 0; msg = "Your move: pick a pit (1-6).";
}

/* Sow the seeds from pit `start`; isYou picks which store to skip and the
 * capture side. Returns the index of the last seed (for the extra-turn /
 * capture rules). Operates on board `b`. */
static int sow(int *b, int start, int isYou) {
    int seeds = b[start], i = start;
    b[start] = 0;
    while (seeds > 0) {
        i = (i + 1) % 14;
        if (isYou && i == 13) continue;     /* you never seed the CPU's store */
        if (!isYou && i == 6) continue;     /* CPU never seeds your store     */
        b[i]++; seeds--;
    }
    if (isYou && i >= 0 && i <= 5 && b[i] == 1) {        /* capture into your store */
        int opp = 12 - i; if (b[opp] > 0) { b[6] += b[opp] + 1; b[opp] = 0; b[i] = 0; }
    }
    if (!isYou && i >= 7 && i <= 12 && b[i] == 1) {      /* capture into CPU store */
        int opp = 12 - i; if (b[opp] > 0) { b[13] += b[opp] + 1; b[opp] = 0; b[i] = 0; }
    }
    return i;
}

static int your_empty(void) { for (int i = 0; i < 6; i++) if (p[i]) return 0; return 1; }
static int cpu_empty(void)  { for (int i = 7; i <= 12; i++) if (p[i]) return 0; return 1; }

static int check_end(void) {
    if (!your_empty() && !cpu_empty()) return 0;
    for (int i = 0; i < 6; i++)  { p[6]  += p[i]; p[i]  = 0; }   /* sweep the leftovers */
    for (int i = 7; i <= 12; i++){ p[13] += p[i]; p[i] = 0; }
    over = 1;
    msg = (p[6] > p[13]) ? "Game over - you win!"
        : (p[6] < p[13]) ? "Game over - CPU wins." : "Game over - a tie.";
    return 1;
}

/* Greedy CPU: score each legal pit (extra turn >> capture >> seeds banked) and
 * take the best, repeating while a move earns another turn. */
static int cpu_pick(void) {
    int best = -1, bestsc = -1000000;
    for (int i = 7; i <= 12; i++) {
        if (!p[i]) continue;
        int t[14]; for (int j = 0; j < 14; j++) t[j] = p[j];
        int before = t[13];
        int land = sow(t, i, 0);
        int sc = (t[13] - before) * 2;          /* seeds banked (incl. captures) */
        if (land == 13) sc += 100;              /* lands in store -> extra turn */
        if (sc > bestsc) { bestsc = sc; best = i; }
    }
    return best;
}
static void cpu_turn(void) {
    for (;;) {
        if (check_end()) return;
        int m = cpu_pick();
        if (m < 0) return;
        int land = sow(p, m, 0);
        if (check_end()) return;
        if (land != 13) { msg = "Your move: pick a pit (1-6)."; return; }   /* no extra turn */
        msg = "CPU goes again...";
    }
}

static void putpit(int v) {                     /* a 2-wide right-aligned cell */
    print(" ");
    if (v < 10) print(" ");
    char s[4]; int n = 0, t = v; if (!t) s[n++] = '0'; while (t) { s[n++] = (char)('0'+t%10); t/=10; }
    char o[4]; int j = 0; while (n) o[j++] = s[--n]; o[j] = 0; print(o);
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("  Mancala"); sys_setcolor(0);
    print("   you vs CPU\n\n");
    /* CPU pits shown right-to-left (12..7), above your pits (0..5) */
    sys_setcolor(12); print("  CPU "); sys_setcolor(0);
    for (int i = 12; i >= 7; i--) putpit(p[i]);
    print("\n");
    print("  store CPU "); sys_setcolor(12); putpit(p[13]); sys_setcolor(0);
    print("   you "); sys_setcolor(2); putpit(p[6]); sys_setcolor(0); print("\n");
    sys_setcolor(2); print("  YOU "); sys_setcolor(0);
    for (int i = 0; i <= 5; i++) putpit(p[i]);
    print("\n  pit  ");
    for (int i = 1; i <= 6; i++) putpit(i);
    print("\n\n  ");
    sys_setcolor(over ? 2 : 0); print(msg); sys_setcolor(0);
    print("\n  1-6 sow   r reset   q quit\n");
}

int main(void) {
    reset();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 'Q') return 0;
        if (k == 'r' || k == 'R') { reset(); render(); continue; }
        if (over) continue;
        if (k >= '1' && k <= '6') {
            int pit = k - '1';
            if (p[pit] == 0) { sys_beep(160, 80); continue; }   /* empty pit */
            int land = sow(p, pit, 1);
            sys_beep(660, 40);
            if (check_end()) { render(); continue; }
            if (land == 6) { msg = "Landed in your store - go again!"; }   /* extra turn */
            else { sys_sleep(150); cpu_turn(); }
            render();
        }
    }
}
