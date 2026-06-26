/*
 * calendar.c — a month calendar, a userspace program.
 *
 * Reads today's date from the RTC (sys_time gives "YYYY-MM-DD HH:MM:SS"), draws
 * the month as a Su..Sa grid with today highlighted in [brackets], and lets you
 * page through months/years with the arrow keys (t = jump back to today). The
 * weekday of the 1st comes from Zeller's congruence. Runs ring-3, like the games.
 */
#include "ulib.h"

static const char *MONTHS[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int days_in_month(int y, int m) {           /* m: 1..12 */
    static const int d[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2 && is_leap(y)) return 29;
    return d[m - 1];
}

/* weekday of (y, m, day) via Zeller's congruence -> 0=Sun .. 6=Sat */
static int weekday(int y, int m, int day) {
    if (m < 3) { m += 12; y -= 1; }
    int K = y % 100, J = y / 100;
    int h = (day + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;  /* 0=Sat..6=Fri */
    return (h + 6) % 7;                                                  /* -> 0=Sun..6=Sat */
}

static void puts_(char *b, int *p, const char *s) { while (*s) b[(*p)++] = *s++; }
static void putn_(char *b, int *p, int v) {
    char t[8]; int i = 0; if (!v) t[i++] = '0'; while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) b[(*p)++] = t[--i];
}
/* a 4-char day cell: "  1 "/" 15 " normal, "[ 1]"/"[15]" today, "    " blank */
static void cell(char *b, int *p, int day, int today) {
    if (day < 1) { for (int i = 0; i < 4; i++) b[(*p)++] = ' '; return; }
    b[(*p)++] = today ? '[' : ' ';
    b[(*p)++] = day >= 10 ? (char)('0' + day / 10) : ' ';
    b[(*p)++] = (char)('0' + day % 10);
    b[(*p)++] = today ? ']' : ' ';
}

static void render(int y, int m, int today_y, int today_m, int today_d) {
    sys_clear();
    char line[64]; int p = 0;
    /* title in cyan (M1336) */
    puts_(line, &p, "    "); puts_(line, &p, MONTHS[m - 1]); line[p++] = ' '; putn_(line, &p, y);
    line[p] = 0; sys_setcolor(4); print(line); sys_setcolor(0); print("\n\n");
    /* weekday header: weekend columns (Su/Sa) amber, weekdays light-blue */
    sys_setcolor(7); print("  Su");
    sys_setcolor(6); print("  Mo  Tu  We  Th  Fr");
    sys_setcolor(7); print("  Sa");
    sys_setcolor(0); print("\n");

    int first = weekday(y, m, 1);          /* column of the 1st */
    int dim = days_in_month(y, m);
    int day = 1;
    for (int row = 0; row < 6 && day <= dim; row++) {
        print(" ");
        for (int col = 0; col < 7; col++) {
            int show = (row == 0 && col < first) ? 0 : (day <= dim ? day : 0);
            int today = (show && y == today_y && m == today_m && show == today_d);
            char c[8]; int q = 0; cell(c, &q, show, today); c[q] = 0;   /* one 4-char cell */
            if (today)                     sys_setcolor(4);   /* today: cyan (with its [brackets]) */
            else if (col == 0 || col == 6) sys_setcolor(7);   /* weekend: amber */
            else                           sys_setcolor(0);   /* weekday: default */
            print(c);
            if (show) day++;
        }
        sys_setcolor(0); print("\n");
    }
    sys_setcolor(8); print("\n arrows: month / year   t today   q quit\n"); sys_setcolor(0);
}

int main(void) {
    char when[24];
    sys_time(when, sizeof(when));
    int ty = (when[0]-'0')*1000 + (when[1]-'0')*100 + (when[2]-'0')*10 + (when[3]-'0');
    int tm = (when[5]-'0')*10 + (when[6]-'0');
    int td = (when[8]-'0')*10 + (when[9]-'0');
    if (tm < 1 || tm > 12) tm = 1;          /* defend against an odd clock */

    int y = ty, m = tm;
    render(y, m, ty, tm, td);
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 27) return 0;
        else if (k == 't') { y = ty; m = tm; }
        else if (k == 0x13) { if (--m < 1)  { m = 12; y--; } }   /* left  = prev month */
        else if (k == 0x14) { if (++m > 12) { m = 1;  y++; } }   /* right = next month */
        else if (k == 0x11) { y++; }                             /* up    = next year  */
        else if (k == 0x12) { y--; }                             /* down  = prev year  */
        else continue;
        if (y < 1) y = 1;
        render(y, m, ty, tm, td);
    }
}
