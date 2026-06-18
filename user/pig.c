/*
 * pig.c — Pig, the push-your-luck dice game.
 *
 * On your turn, Roll as many times as you dare: each roll adds to your turn
 * total, but rolling a 1 wipes the turn total and ends your turn. Hold to bank
 * the turn total into your score. First to 100 wins. The CPU holds once it has
 * 20 this turn (or can win outright).
 *
 * r roll, h hold, n new game (after a win), q quit.
 */
#include "ulib.h"

static int you, cpu, total, over, youwin, lastroll;
static const char *msg;

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static int die(void) { return 1 + (int)(rnd() % 6); }

static void putn(int n) {
    char t[8]; int i = 0;
    if (n == 0) { print("0"); return; }
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    char s[8]; int j = 0; while (i) s[j++] = t[--i]; s[j] = 0; print(s);
}

static void reset(void) { you = cpu = total = over = youwin = lastroll = 0; msg = "Your turn - r roll, h hold"; }

static void render(const char *who) {
    sys_clear();
    sys_setcolor(4); print("\n  Pig"); sys_setcolor(8); print("   (first to 100)\n\n");
    sys_setcolor(0); print("    You: "); sys_setcolor(2); putn(you);
    sys_setcolor(0); print("      CPU: "); sys_setcolor(3); putn(cpu); sys_setcolor(0); print("\n\n");
    print("    turn ("); print(who); print("): "); sys_setcolor(14); putn(total); sys_setcolor(0);
    print("    last roll: ");
    if (lastroll) { sys_setcolor(lastroll == 1 ? 2 : 10); putn(lastroll); sys_setcolor(0); }
    else print("-");
    print("\n\n  ");
    if (over) { sys_setcolor(youwin ? 10 : 2); print(youwin ? "YOU WIN!  (n = new game)" : "CPU wins.  (n = new game)"); sys_setcolor(0); }
    else print(msg);
    print("\n  r roll   h hold   q quit\n");
}

static void cpu_turn(void) {
    total = 0;
    msg = "CPU's turn...";
    render("CPU"); sys_sleep(500);
    for (;;) {
        lastroll = die(); render("CPU"); sys_sleep(450);
        if (lastroll == 1) { total = 0; sys_beep(196, 120); break; }
        total += lastroll;
        if (cpu + total >= 100) { cpu += total; total = 0; over = 1; youwin = 0; break; }
        if (total >= 20) { cpu += total; total = 0; sys_beep(523, 80); break; }
    }
    lastroll = 0;
    if (!over) msg = "Your turn - r roll, h hold";
    render("you");
}

int main(void) {
    rng = (unsigned)sys_uptime_ms() | 1u;
    reset();
    render("you");
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 'Q') break;
        if (over) { if (k == 'n' || k == 'N') { reset(); render("you"); } continue; }
        if (k == 'r' || k == 'R') {
            lastroll = die();
            if (lastroll == 1) { total = 0; sys_beep(196, 120); msg = "Rolled a 1 - turn lost!"; render("you"); sys_sleep(600); cpu_turn(); }
            else { total += lastroll; msg = "Roll again, or hold"; render("you"); }
        } else if (k == 'h' || k == 'H') {
            you += total; total = 0;
            if (you >= 100) { over = 1; youwin = 1; sys_beep(1046, 220); render("you"); }
            else { sys_beep(659, 80); cpu_turn(); }
        }
    }
    return 0;
}
