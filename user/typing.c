/*
 * typing.c — a typing-speed test (WPM + accuracy).
 *
 * A different category from the games: a skill/practice tool. A target sentence
 * is shown; you retype it. Each key is matched against the expected character
 * and coloured green (correct) or red (wrong) in place, with a cursor marking
 * your position — there's no backspace, so accuracy counts every keystroke.
 * Timing (from the wall clock, to the second) starts on your first keystroke;
 * on completion it reports words-per-minute (chars/5 over the time) + accuracy.
 */
#include "ulib.h"

static const char *SENTENCES[] = {
    "the quick brown fox jumps lazily",
    "pack my box with five dozen jugs",
    "how quickly daft zebras can jump",
    "five boxing wizards jump quickly",
    "sphinx of black quartz judge vow",
};
#define NSENT (int)(sizeof(SENTENCES)/sizeof(SENTENCES[0]))
#define MAXLEN 80

static const char *target;
static int len;
static char ok[MAXLEN];      /* 1 = the char typed at this position was correct */
static int pos;              /* characters typed so far */
static int correct;          /* count typed correctly */
static int round;            /* which sentence (rotates) */
static int state;            /* 1 typing, 2 done */
static int t_start;          /* seconds-of-day at the first keystroke (-1 = not yet) */
static int t_end;            /* seconds-of-day at completion */

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

static int now_secs(void) {                    /* parse "YYYY-MM-DD HH:MM:SS" -> seconds of day */
    char b[40];
    sys_time(b, sizeof(b));
    int hh = (b[11]-'0')*10 + (b[12]-'0');
    int mm = (b[14]-'0')*10 + (b[15]-'0');
    int ss = (b[17]-'0')*10 + (b[18]-'0');
    return hh*3600 + mm*60 + ss;
}

static void printnum(int v) {
    char t[12]; int i = 0, n = v < 0 ? 0 : v;
    if (n == 0) t[i++] = '0';
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    char o[12]; int j = 0; while (i) o[j++] = t[--i]; o[j] = 0;
    print(o);
}

static void load_round(void) {
    target = SENTENCES[round % NSENT];
    len = (int)slen(target);
    if (len > MAXLEN) len = MAXLEN;
    pos = correct = 0;
    t_start = -1;
    state = 1;
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("\n  Typing Test"); sys_setcolor(0);
    print("    round "); printnum(round + 1); print("\n\n");

    print("  target:\n  "); sys_setcolor(8); print(target); sys_setcolor(0); print("\n\n");

    print("  you:\n  ");
    for (int i = 0; i < len; i++) {
        if (i < pos)        { sys_setcolor(ok[i] ? 1 : 2); char c[2] = { target[i], 0 }; print(c); }  /* green / red */
        else if (i == pos && state == 1) { sys_setcolor(3); print("_"); }                              /* cursor */
        else                { sys_setcolor(8); print("."); }                                           /* untyped */
    }
    sys_setcolor(0); print("\n\n  ");

    if (state == 1) print("type it!   (no backspace, Esc quits)");
    else {
        int el = t_end - t_start;
        if (el < 0) el += 86400;
        if (el < 1) el = 1;
        int wpm = (correct * 12) / el;             /* (correct/5) / (el/60) */
        int acc = len ? (correct * 100) / len : 0;
        sys_setcolor(2); print("done!  "); sys_setcolor(0);
        print("WPM "); sys_setcolor(3); printnum(wpm); sys_setcolor(0);
        print("   accuracy "); sys_setcolor(3); printnum(acc); print("%"); sys_setcolor(0);
        print("\n  SPACE = next sentence");
    }
    print("\n");
}

int main(void) {
    round = 0;
    load_round();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 27) break;                            /* Esc quits */

        if (state == 1) {
            if (k >= 0x20 && k <= 0x7e && pos < len) {
                if (t_start < 0) t_start = now_secs();  /* start timing on the first keystroke */
                ok[pos] = (k == target[pos]);
                if (ok[pos]) correct++;
                pos++;
                if (pos >= len) { state = 2; t_end = now_secs(); }   /* finished */
                render();
            }
        } else { /* state 2: done */
            if (k == ' ') { round++; load_round(); render(); }
        }
    }
    return 0;
}
