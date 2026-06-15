/*
 * typing.c — a typing-speed test (WPM + accuracy), with a persistent best.
 *
 * A different category from the games: a skill/practice tool. A target sentence
 * is shown; you retype it. Each key is matched against the expected character
 * and coloured green (correct) or red (wrong) in place, with a cursor marking
 * your position — there's no backspace, so accuracy counts every keystroke.
 * Timing (from the wall clock, to the second) starts on your first keystroke;
 * on completion it reports words-per-minute (chars/5 over the time) + accuracy.
 * Your best WPM on a clean run (>=90% accuracy) persists in TYPING.HI.
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
static int result_wpm, result_acc;   /* computed once at completion */
static int best;             /* best clean-run WPM, persisted to TYPING.HI */

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

static int now_secs(void) {                    /* parse "YYYY-MM-DD HH:MM:SS" -> seconds of day */
    char b[40];
    sys_time(b, sizeof(b));
    int hh = (b[11]-'0')*10 + (b[12]-'0');
    int mm = (b[14]-'0')*10 + (b[15]-'0');
    int ss = (b[17]-'0')*10 + (b[18]-'0');
    return hh*3600 + mm*60 + ss;
}

static int itoa_b(int v, char *out) {          /* non-negative int -> string; returns length */
    char t[12]; int i = 0, n = v < 0 ? 0 : v;
    if (n == 0) t[i++] = '0';
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    int j = 0; while (i) out[j++] = t[--i];
    out[j] = 0; return j;
}
static void printnum(int v) { char b[12]; itoa_b(v, b); print(b); }

static void load_best(void) {
    char b[16]; long n = sys_readfile("TYPING.HI", b, sizeof(b) - 1);
    best = 0;
    for (long i = 0; i < n; i++) {
        if (b[i] < '0' || b[i] > '9') break;
        best = best * 10 + (b[i] - '0');
        if (best > 100000) { best = 100000; break; }   /* clamp: a corrupt file can't overflow int */
    }
}
static void save_best(void) {
    char b[12]; int n = itoa_b(best, b);
    sys_writefile("TYPING.HI", b, (unsigned long)n);
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
    print("    round "); printnum(round + 1);
    print("    best "); sys_setcolor(3); printnum(best); print(" wpm"); sys_setcolor(0); print("\n\n");

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
        sys_setcolor(2); print("done!  "); sys_setcolor(0);
        print("WPM "); sys_setcolor(3); printnum(result_wpm); sys_setcolor(0);
        print("   accuracy "); sys_setcolor(3); printnum(result_acc); print("%"); sys_setcolor(0);
        print("\n  SPACE = next sentence");
    }
    print("\n");
}

static void finish(void) {
    int el = now_secs() - t_start;
    if (el < 0) el += 86400;                       /* crossed midnight */
    if (el < 1) el = 1;
    result_wpm = (correct * 12) / el;              /* (correct/5) / (el/60) */
    result_acc = len ? (correct * 100) / len : 0;
    state = 2;
    if (result_acc >= 90 && result_wpm > best) {   /* a clean run beat the record */
        best = result_wpm;
        save_best();
    }
}

int main(void) {
    round = 0;
    load_best();
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
                if (pos >= len) finish();
                render();
            }
        } else { /* state 2: done */
            if (k == ' ') { round++; load_round(); render(); }
        }
    }
    return 0;
}
