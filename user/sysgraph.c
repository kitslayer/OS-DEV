/*
 * sysgraph.c — a graphical system monitor, a userspace program (M1361;
 * kernel-font labels + MB readout M1363).
 *
 * A WM pixel canvas with two live scrolling area graphs — CPU% (green) over
 * RAM% (cyan), sampled twice a second from /proc/stat (the aggregate cpu
 * busy/total delta) and /proc/meminfo. Labels are drawn with the kernel's
 * 8x16 console font (via sys_font), and the RAM label shows the actual MB
 * used / total. Pure integer math.
 *
 * Launch: `run sysgraph` or the Apps menu ("System Monitor"); q/Esc quits.
 */
#include "ulib.h"

#define W   320
#define H   240
#define NS  308                /* history samples kept (<= graph width) */

static unsigned *FB;
static unsigned char FONT[128 * 16];    /* kernel 8x16 console font, fetched once via sys_font (M1363) */

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void gch(char c, int px, int py, unsigned col) {        /* one 8x16 glyph, MSB = leftmost pixel */
    unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16];
    for (int r = 0; r < 16; r++)
        for (int b = 0; b < 8; b++)
            if ((g[r] >> (7 - b)) & 1) putpx(px + b, py + r, col);
}
static void gtext(const char *t, int x, int y, unsigned col) { for (int i = 0; t[i]; i++) { gch(t[i], x, y, col); x += 8; } }

/* read a whole /proc file, NUL-terminated; returns length */
static int slurp(const char *p, char *buf, int max) {
    long n = sys_readfile(p, buf, max - 1); if (n < 0) n = 0; buf[n] = 0; return (int)n;
}
/* the n-th (0-based) unsigned integer appearing in s */
static long nthnum(const char *s, int n) {
    int k = 0;
    for (int i = 0; s[i]; ) {
        if (s[i] >= '0' && s[i] <= '9') {
            long v = 0; while (s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
            if (k == n) return v; k++;
        } else i++;
    }
    return 0;
}
static int putu(char *o, int i, long v) {              /* append v's decimal digits; return new index */
    char t[12]; int n = 0; if (v < 0) v = 0;
    do { t[n++] = '0' + (int)(v % 10); v /= 10; } while (v);
    while (n) o[i++] = t[--n];
    return i;
}
/* "TAG nn%" into out (TAG = 3 chars); returns the length */
static int label(char *out, const char *tag, int pct) {
    int i = 0; out[i++] = tag[0]; out[i++] = tag[1]; out[i++] = tag[2]; out[i++] = ' ';
    i = putu(out, i, pct); out[i++] = '%'; return i;
}

/* one scrolling area-graph panel: the newest `count` samples right-aligned */
static void panel(const int *arr, int head, int count, int y0, int gh, unsigned col, unsigned dim) {
    int base = y0 + gh - 1;
    for (int q = 1; q <= 3; q++) {                       /* 25/50/75% grid lines */
        int gy = base - q * gh / 4;
        for (int x = 2; x < W - 2; x += 2) putpx(x, gy, 0x252533);
    }
    for (int c = 0; c < count; c++) {
        int x = W - 3 - c; if (x < 2) break;
        int v = arr[(head - 1 - c + NS) % NS]; if (v < 0) v = 0; if (v > 100) v = 100;
        int h = v * gh / 100;
        unsigned crest = v >= 80 ? 0xF05858 : v >= 50 ? 0xE8C040 : col;   /* load colour: red >=80%, amber >=50%, else base */
        for (int yy = 0; yy < h; yy++) putpx(x, base - yy, yy >= h - 1 ? crest : dim);   /* coloured crest, dim fill */
    }
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("sysgraph: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("sysgraph: init failed\n"); return 1; }

    static int cpu[NS], ram[NS];
    int head = 0, count = 0, first = 1;
    long pbusy = 0, ptot = 0;
    char buf[2048];

    for (;;) {
        /* CPU%: aggregate `cpu  user nice system idle ...` busy/total delta */
        slurp("/proc/stat", buf, sizeof buf);
        long u = nthnum(buf, 0), ni = nthnum(buf, 1), sy = nthnum(buf, 2), id = nthnum(buf, 3);
        long busy = u + ni + sy, tot = busy + id;
        int cp = 0;
        if (!first && tot > ptot) { cp = (int)((busy - pbusy) * 100 / (tot - ptot)); if (cp < 0) cp = 0; if (cp > 100) cp = 100; }
        pbusy = busy; ptot = tot; first = 0;

        /* RAM%: MemTotal (0th num) and MemUsed (2nd num) from /proc/meminfo, in kB */
        slurp("/proc/meminfo", buf, sizeof buf);
        long mt = nthnum(buf, 0), mu = nthnum(buf, 2);
        int rp = (mt > 0) ? (int)(mu * 100 / mt) : 0; if (rp < 0) rp = 0; if (rp > 100) rp = 100;

        cpu[head] = cp; ram[head] = rp; head = (head + 1) % NS; if (count < NS) count++;

        for (int i = 0; i < W * H; i++) FB[i] = 0x0C0C14;       /* dark background */
        panel(cpu, head, count, 26, 90, 0x46E05A, 0x16401E);    /* CPU: green crest, dim-green fill */
        panel(ram, head, count, 142, 90, 0x46C0E0, 0x16384A);   /* RAM: cyan crest, dim-cyan fill  */
        for (int x = 0; x < W; x++) putpx(x, 122, 0x202030);    /* divider */

        char lab[40]; int n;
        unsigned ccol = cp >= 80 ? 0xF08080 : cp >= 50 ? 0xF0D060 : 0x80FF90;   /* headline number tinted by load */
        n = label(lab, "CPU", cp); lab[n] = 0; gtext(lab, 8, 6, ccol);
        unsigned rcol = rp >= 80 ? 0xF08080 : rp >= 50 ? 0xF0D060 : 0x80E0FF;
        n = label(lab, "RAM", rp);                              /* + actual MB used/total */
        lab[n++] = ' '; n = putu(lab, n, mu / 1024); lab[n++] = '/'; n = putu(lab, n, mt / 1024); lab[n++] = 'M'; lab[n] = 0;
        gtext(lab, 8, 124, rcol);

        sys_gfx_blit(FB);
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        sys_sleep(500);
    }
    return 0;
}
