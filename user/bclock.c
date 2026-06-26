/*
 * bclock.c — a graphical BINARY clock, a userspace program (M1388).
 *
 * Shows the time as six columns of BCD "LED" dots — H10 H1 : M10 M1 : S10 S1 —
 * each column's four dots (bit 8/4/2/1, top to bottom) lit when that bit of the
 * decimal digit is set, colour-grouped (hours green, minutes cyan, seconds
 * amber). The decimal HH:MM:SS is shown below for reference. Time from sys_time;
 * redraws only when the second changes. Pure render, no FPU, no state.
 *
 * Launch: `run bclock` from the shell, or the Apps menu ("Binary Clock").
 */
#include "ulib.h"

#define W 248
#define H 212

static unsigned *FB;
static unsigned char FONT[128 * 16];

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void disc(int cx, int cy, int r, unsigned c) { for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) if (dx * dx + dy * dy <= r * r) putpx(cx + dx, cy + dy, c); }
static void ch(char c, int px, int py, unsigned col) { unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16]; for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) putpx(px + b, py + r, col); }
static void text(const char *s, int x, int y, unsigned col) { for (int i = 0; s[i]; i++) { ch(s[i], x, y, col); x += 8; } }

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("bclock: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("bclock: init failed\n"); return 1; }

    /* per-group lit/dim colours: hours, minutes, seconds */
    static const unsigned lit[3] = { 0x46E05A, 0x46C8E0, 0xE8C040 };
    static const unsigned dim[3] = { 0x123A1C, 0x123040, 0x342C12 };
    static const char *hdr[3] = { "Hr", "Min", "Sec" };

    int psec = -1;
    for (;;) {
        char tb[40]; sys_time(tb, sizeof tb);                    /* "YYYY-MM-DD HH:MM:SS" */
        int hh = (tb[11]-'0')*10 + (tb[12]-'0');
        int mm = (tb[14]-'0')*10 + (tb[15]-'0');
        int ss = (tb[17]-'0')*10 + (tb[18]-'0');

        if (ss != psec) {
            psec = ss;
            for (int i = 0; i < W * H; i++) FB[i] = 0x0A0C12;
            int dig[6] = { hh/10, hh%10, mm/10, mm%10, ss/10, ss%10 };

            for (int row = 0; row < 4; row++) {                  /* bit-value labels down the left (8/4/2/1) */
                char bl[2] = { (char)('0' + (8 >> row)), 0 };
                text(bl, 6, 44 + row * 32, 0x60687A);
            }
            for (int col = 0; col < 6; col++) {
                int grp = col / 2;
                int cx = 36 + col * 32 + grp * 10;
                if (col % 2 == 0) text(hdr[grp], cx + 8, 18, 0x90A0C0);   /* group header over each pair */
                for (int row = 0; row < 4; row++) {
                    int on = dig[col] & (8 >> row);
                    disc(cx + 8, 48 + row * 32, 11, on ? lit[grp] : dim[grp]);
                }
            }
            char t[12]; int i = 0;                               /* decimal HH:MM:SS for reference */
            t[i++]='0'+hh/10; t[i++]='0'+hh%10; t[i++]=':'; t[i++]='0'+mm/10; t[i++]='0'+mm%10; t[i++]=':'; t[i++]='0'+ss/10; t[i++]='0'+ss%10; t[i]=0;
            text(t, (W - 8 * 8) / 2, H - 22, 0xC0C8D6);
        }

        sys_gfx_blit(FB);
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        sys_sleep(150);
    }
    return 0;
}
