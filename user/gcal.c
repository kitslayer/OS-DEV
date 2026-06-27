/*
 * gcal.c — a graphical month CALENDAR, a userspace program (M1385).
 *
 * A WM pixel canvas showing the current month as a grid: a "Month YYYY" title,
 * weekday headers (Su..Sa, weekends tinted), and the day numbers laid out from
 * the 1st's weekday (Zeller's congruence), with TODAY highlighted. Real text via
 * the kernel 8x16 console font (sys_font). Pure integer math, no FPU.
 *
 * Launch: `run gcal` from the shell, or the Apps menu ("Calendar (gfx)").
 * p/n (or ←/→) page to the previous/next month, t jumps back to today; q/Esc quits.
 */
#include "ulib.h"

#define W 240
#define H 250

static unsigned *FB;
static unsigned char FONT[128 * 16];

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void fill(int x0, int y0, int w, int h, unsigned c) {
    for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) putpx(x, y, c);
}
static void ch(char c, int px, int py, unsigned col) {
    unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16];
    for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) putpx(px + b, py + r, col);
}
static void text(const char *s, int x, int y, unsigned col) { for (int i = 0; s[i]; i++) { ch(s[i], x, y, col); x += 8; } }

/* weekday of a date via Zeller's congruence: 0=Sat, 1=Sun, ..., 6=Fri */
static int zeller(int y, int m, int d) {
    if (m < 3) { m += 12; y -= 1; }                 /* Jan/Feb = months 13/14 of the prior year */
    int K = y % 100, J = y / 100;
    return (d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
}
static int days_in_month(int y, int m) {
    static const int t[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return 29;
    return t[(m >= 1 && m <= 12) ? m - 1 : 0];
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gcal: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gcal: init failed\n"); return 1; }

    static const char *mon[] = { "January", "February", "March", "April", "May", "June",
                                 "July", "August", "September", "October", "November", "December" };
    static const char *wd[] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };

    int pday = -1, moff = 0, dirty = 1;   /* moff = months from the current month (M1461) */
    for (;;) {
        char tb[40]; sys_time(tb, sizeof tb);                    /* "YYYY-MM-DD HH:MM:SS" */
        int y = (tb[0]-'0')*1000 + (tb[1]-'0')*100 + (tb[2]-'0')*10 + (tb[3]-'0');
        int m = (tb[5]-'0')*10 + (tb[6]-'0');
        int d = (tb[8]-'0')*10 + (tb[9]-'0');

        if (d != pday) { pday = d; dirty = 1; }                  /* day rollover -> re-render */
        if (dirty) {
            dirty = 0;
            int dm = m + moff, dy = y;                           /* displayed month (navigable, M1461) */
            while (dm > 12) { dm -= 12; dy++; }
            while (dm < 1)  { dm += 12; dy--; }
            for (int i = 0; i < W * H; i++) FB[i] = 0x10121A;    /* dark background */

            char title[24]; int ti = 0; const char *mn = mon[(dm >= 1 && dm <= 12) ? dm - 1 : 0];
            for (int k = 0; mn[k]; k++) title[ti++] = mn[k]; title[ti++] = ' ';
            title[ti++] = '0' + dy/1000; title[ti++] = '0' + (dy/100)%10; title[ti++] = '0' + (dy/10)%10; title[ti++] = '0' + dy%10; title[ti] = 0;
            text(title, (W - ti * 8) / 2, 8, 0x8FD0FF);
            fill(8, 28, W - 16, 1, 0x2A2E3A);                    /* rule under the title */

            int cw = 32, x0 = 8, hy = 36;
            for (int c = 0; c < 7; c++)                          /* weekday headers (weekends reddish) */
                text(wd[c], x0 + c * cw + (cw - 16) / 2, hy, (c == 0 || c == 6) ? 0xD08080 : 0x90A0C0);

            int first = (zeller(dy, dm, 1) + 6) % 7;             /* grid column of the 1st (Sunday=0) */
            int nd = days_in_month(dy, dm), gy = hy + 22;
            for (int day = 1; day <= nd; day++) {
                int idx = first + day - 1, col = idx % 7, row = idx / 7;
                int cx = x0 + col * cw, cy = gy + row * 26;
                char ds[3]; int di = 0; if (day >= 10) ds[di++] = '0' + day / 10; ds[di++] = '0' + day % 10; ds[di] = 0;
                int tx = cx + (cw - di * 8) / 2;
                if (day == d && dy == y && dm == m) { fill(cx + 1, cy - 3, cw - 2, 22, 0x2C5AA0); text(ds, tx, cy, 0xFFFFFF); }   /* today (only in the current month) */
                else text(ds, tx, cy, (col == 0 || col == 6) ? 0xC09090 : 0xD0D0D8);                       /* weekend dimmer */
            }
            text(moff ? "p/n month  t today  q quit" : "p/n month  q quit", 8, H - 18, 0x6A7A88);   /* nav footer */
        }

        sys_gfx_blit(FB);
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        else if (k == 0x13 || k == 'p' || k == 'P') { moff--; dirty = 1; }   /* prev month (M1461) */
        else if (k == 0x14 || k == 'n' || k == 'N') { moff++; dirty = 1; }   /* next month */
        else if (k == 't' || k == 'T' || k == '.') { moff = 0; dirty = 1; }  /* back to today */
        sys_sleep(120);
    }
    return 0;
}
