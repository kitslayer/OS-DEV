/*
 * timer.c — a Stopwatch + Countdown Timer, a userspace program.
 *
 * Two modes, Tab to switch.  STOPWATCH counts up (Space start/stop, r reset);
 * TIMER counts down from a duration you dial in with the arrows (Space
 * start/pause, r reset) and, when it hits zero, flashes "TIME'S UP!" and beeps
 * an alarm on the PC speaker.  Both modes read time from sys_uptime_ms() — the
 * monotonic clock — so nothing ever blocks: each tick recomputes the elapsed
 * value and redraws the whole little screen, like calendar/jukebox do.  The big
 * MM:SS digits come from a 5-row block-glyph renderer.  Pure integer, ring-3.
 */
#include "ulib.h"

/* the modes */
#define M_STOP  0          /* stopwatch (counts up) */
#define M_TIMER 1          /* countdown */

/* a 5-row x 3-col block glyph for each of '0'..'9' and ':' / '.', drawn with
 * '#' so the time reads big from across the room.  Index 10 = ':', 11 = '.'. */
static const char *GLYPH[12][5] = {
    { "###", "# #", "# #", "# #", "###" },   /* 0 */
    { "  #", "  #", "  #", "  #", "  #" },   /* 1 */
    { "###", "  #", "###", "#  ", "###" },   /* 2 */
    { "###", "  #", "###", "  #", "###" },   /* 3 */
    { "# #", "# #", "###", "  #", "  #" },   /* 4 */
    { "###", "#  ", "###", "  #", "###" },   /* 5 */
    { "###", "#  ", "###", "# #", "###" },   /* 6 */
    { "###", "  #", "  #", "  #", "  #" },   /* 7 */
    { "###", "# #", "###", "# #", "###" },   /* 8 */
    { "###", "# #", "###", "  #", "###" },   /* 9 */
    { "   ", " # ", "   ", " # ", "   " },   /* : */
    { "   ", "   ", "   ", "   ", " # " },   /* . */
};

/* map a display char to its GLYPH row; ' ' stays blank */
static int glyph_idx(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == ':') return 10;
    if (c == '.') return 11;
    return -1;
}

/* render the NUL-terminated time string (e.g. "01:23.4") as big block digits */
static void big_time(const char *s) {
    for (int row = 0; row < 5; row++) {
        char line[80]; int p = 0;
        line[p++] = ' '; line[p++] = ' ';
        for (int i = 0; s[i]; i++) {
            int gi = glyph_idx(s[i]);
            if (gi < 0) { line[p++] = ' '; line[p++] = ' '; line[p++] = ' '; }
            else for (int c = 0; c < 3; c++) line[p++] = GLYPH[gi][row][c];
            line[p++] = ' ';                       /* gap between glyphs */
        }
        line[p] = 0; print(line); print("\n");
    }
}

static void put2(char *b, int *p, int v) {        /* zero-padded 2-digit field */
    b[(*p)++] = (char)('0' + (v / 10) % 10);
    b[(*p)++] = (char)('0' + v % 10);
}

/* format whole-ms into "MM:SS.t" (tenths) when want_tenths, else "MM:SS" */
static void fmt_time(char *b, unsigned long ms, int want_tenths) {
    unsigned long total_s = ms / 1000;
    int mm = (int)(total_s / 60);
    int ss = (int)(total_s % 60);
    if (mm > 99) mm = 99;                          /* MM field is 2 wide */
    int p = 0;
    put2(b, &p, mm); b[p++] = ':'; put2(b, &p, ss);
    if (want_tenths) { b[p++] = '.'; b[p++] = (char)('0' + (ms / 100) % 10); }
    b[p] = 0;
}

/* ---- shared state ---- */
static int mode = M_STOP;

/* stopwatch: elapsed = sw_acc + (sw_run ? now - sw_t0 : 0) */
static unsigned long sw_acc, sw_t0;
static int sw_run;

/* timer: set duration, remaining = td_rem while paused, counts down while run */
static unsigned long td_set = 60000;               /* default 1:00 */
static unsigned long td_rem = 60000;               /* remaining when paused */
static unsigned long td_t0;                         /* uptime at last (re)start */
static int td_run;
static int td_alarm;                                /* 1 = TIME'S UP banner showing */

#define TD_MAX  (99UL * 60 + 59) * 1000             /* 99:59 clamp */

/* current stopwatch elapsed in ms */
static unsigned long sw_elapsed(void) {
    unsigned long e = sw_acc;
    if (sw_run) e += sys_uptime_ms() - sw_t0;
    return e;
}

/* current timer remaining in ms (0 if expired) */
static unsigned long td_remaining(void) {
    if (!td_run) return td_rem;
    unsigned long gone = sys_uptime_ms() - td_t0;
    if (gone >= td_rem) return 0;
    return td_rem - gone;
}

static void draw(void) {
    sys_clear();
    sys_setcolor(4);                                /* header */
    print("  == Timer ==   ");
    if (mode == M_STOP) print("[ STOPWATCH ]   Timer\n\n");
    else                print("Stopwatch   [ TIMER ]\n\n");
    sys_setcolor(0);

    char ts[16];
    if (mode == M_STOP) {
        fmt_time(ts, sw_elapsed(), 1);              /* MM:SS.t */
        sys_setcolor(sw_run ? 3 : 0);               /* green while running */
        big_time(ts);
        sys_setcolor(0);
        print("\n  Space start/stop    r reset (when stopped)\n");
        print("  Tab switch mode     q quit\n");
    } else {
        unsigned long rem = td_remaining();
        if (td_alarm) {
            fmt_time(ts, 0, 0);                      /* 00:00 */
            sys_setcolor(0); big_time(ts);
            sys_setcolor(12);                        /* bright red banner */
            print("\n      *** TIME'S UP! ***\n");
            sys_setcolor(0);
            print("\n  Press any key to dismiss\n");
        } else {
            fmt_time(ts, rem, 0);                    /* MM:SS */
            sys_setcolor(td_run ? 3 : 0);
            big_time(ts);
            sys_setcolor(0);
            if (td_run) {
                print("\n  Space pause         r reset\n");
            } else {
                print("\n  Up/Dn +/- 1 min   Lt/Rt +/- 10 sec\n");
                print("  Space start         r reset\n");
            }
            print("  Tab switch mode     q quit\n");
        }
    }
}

/* ring the alarm: a few short beeps with small gaps (blocks ~0.9 s total) */
static void alarm(void) {
    for (int i = 0; i < 4; i++) {
        sys_beep(880, 150);
        sys_sleep(80);
    }
}

int main(void) {
    draw();
    for (;;) {
        int k = sys_pollkey();

        /* fire the alarm the instant a running timer reaches zero */
        if (mode == M_TIMER && td_run && td_remaining() == 0) {
            td_run = 0; td_rem = 0; td_alarm = 1;
            draw();                                  /* show the banner first */
            alarm();
        }

        if (k < 0) {                                 /* no key: tick a running clock, then idle */
            if ((mode == M_STOP && sw_run) || (mode == M_TIMER && td_run)) draw();
            sys_sleep(70);                            /* ~14 Hz */
            continue;
        }

        if (k == 'q' || k == 27) return 0;

        if (mode == M_TIMER && td_alarm) {           /* any key clears the banner */
            td_alarm = 0; td_rem = td_set;
            draw();
            continue;
        }

        if (k == '\t') {                             /* Tab: switch mode */
            mode = (mode == M_STOP) ? M_TIMER : M_STOP;
            draw();
            continue;
        }

        if (mode == M_STOP) {
            if (k == ' ') {                          /* start/stop */
                if (sw_run) { sw_acc = sw_elapsed(); sw_run = 0; }
                else        { sw_t0 = sys_uptime_ms(); sw_run = 1; }
            } else if (k == 'r' && !sw_run) {        /* reset (when stopped) */
                sw_acc = 0;
            } else { sys_sleep(70); continue; }
            draw();
        } else {                                     /* M_TIMER */
            if (k == ' ') {                          /* start / pause */
                if (td_run) { td_rem = td_remaining(); td_run = 0; }
                else if (td_rem > 0) { td_t0 = sys_uptime_ms(); td_run = 1; }
            } else if (k == 'r') {                    /* reset to the set duration */
                td_run = 0; td_rem = td_set;
            } else if (!td_run) {                     /* adjust while stopped */
                long v = (long)td_set;
                if      (k == 0x11)              v += 60000;   /* up    +1 min */
                else if (k == 0x12)              v -= 60000;   /* down  -1 min */
                else if (k == 0x14 || k == '+')  v += 10000;   /* right +10 s  */
                else if (k == 0x13 || k == '-')  v -= 10000;   /* left  -10 s  */
                else { sys_sleep(70); continue; }
                if (v < 0) v = 0;
                if (v > (long)TD_MAX) v = (long)TD_MAX;
                td_set = (unsigned long)v;
                td_rem = td_set;                       /* track the set value */
            } else { sys_sleep(70); continue; }
            draw();
        }
    }
}
