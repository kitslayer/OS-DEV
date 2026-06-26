/*
 * sysgraph.c — a graphical system monitor, a userspace program (M1361).
 *
 * The OS reported CPU/RAM only as text (`top`, `free`, the clock dashboard).
 * This is the visual companion: a WM pixel canvas (sys_gfx_init/sys_gfx_blit)
 * showing two live scrolling area graphs — CPU utilisation (green) over RAM
 * usage (cyan) — each sampled twice a second and labelled with its current
 * percentage. CPU% is the busy/total delta of the aggregate `cpu` line in
 * /proc/stat; RAM% is MemUsed/MemTotal from /proc/meminfo. A compact 3x5
 * bitmap font draws the "CPU nn%" / "RAM nn%" labels (no FPU, no kernel font).
 *
 * Launch: `run sysgraph` or the Apps menu ("System Monitor"); q/Esc quits.
 */
#include "ulib.h"

#define W   320
#define H   240
#define NS  308                /* history samples kept (<= graph width) */

static unsigned *FB;
static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }

/* 3x5 bitmap font: 0-9, then : % space C P U R A M (low 3 bits per row) */
static const unsigned char FONT[][5] = {
    {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
    {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7},  /* 0-9 */
    {0,2,0,2,0},   /* : */  {5,1,2,4,5},   /* % */  {0,0,0,0,0},   /* space */
    {7,4,4,4,7},   /* C */  {7,5,7,4,4},   /* P */  {5,5,5,5,7},   /* U */
    {7,5,7,6,5},   /* R */  {7,5,7,5,5},   /* A */  {5,7,7,5,5},   /* M */
};
static int gid(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    switch (c) {
        case ':': return 10; case '%': return 11; case ' ': return 12;
        case 'C': return 13; case 'P': return 14; case 'U': return 15;
        case 'R': return 16; case 'A': return 17; case 'M': return 18;
    }
    return -1;
}
static void glyph(int idx, int px, int py, int s, unsigned col) {
    if (idx < 0) return;
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 3; c++)
            if ((FONT[idx][r] >> (2 - c)) & 1)
                for (int yy = 0; yy < s; yy++)
                    for (int xx = 0; xx < s; xx++) putpx(px + c * s + xx, py + r * s + yy, col);
}
static void text(const char *t, int x, int y, int s, unsigned col) {
    for (int i = 0; t[i]; i++) { glyph(gid(t[i]), x, y, s, col); x += (t[i] == ' ') ? 2 * s : 4 * s; }
}

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
/* build "LBL nn%" (LBL is a 3-char tag) into out */
static void label(char *out, const char *tag, int pct) {
    int i = 0; out[i++] = tag[0]; out[i++] = tag[1]; out[i++] = tag[2]; out[i++] = ' ';
    if (pct >= 100) { out[i++] = '1'; out[i++] = '0'; out[i++] = '0'; }
    else { if (pct >= 10) out[i++] = '0' + pct / 10; out[i++] = '0' + pct % 10; }
    out[i++] = '%'; out[i] = 0;
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
        for (int yy = 0; yy < h; yy++) putpx(x, base - yy, yy >= h - 1 ? col : dim);   /* bright crest, dim fill */
    }
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("sysgraph: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB) { print("sysgraph: out of memory\n"); return 1; }

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

        /* RAM%: MemTotal (0th num) and MemUsed (2nd num) from /proc/meminfo */
        slurp("/proc/meminfo", buf, sizeof buf);
        long mt = nthnum(buf, 0), mu = nthnum(buf, 2);
        int rp = (mt > 0) ? (int)(mu * 100 / mt) : 0; if (rp < 0) rp = 0; if (rp > 100) rp = 100;

        cpu[head] = cp; ram[head] = rp; head = (head + 1) % NS; if (count < NS) count++;

        for (int i = 0; i < W * H; i++) FB[i] = 0x0C0C14;       /* dark background */
        panel(cpu, head, count, 22, 94, 0x46E05A, 0x16401E);    /* CPU: green crest, dim-green fill */
        panel(ram, head, count, 140, 94, 0x46C0E0, 0x16384A);   /* RAM: cyan crest, dim-cyan fill  */
        for (int x = 0; x < W; x++) putpx(x, 120, 0x202030);    /* divider */
        char lab[12];
        label(lab, "CPU", cp); text(lab, 6, 5, 3, 0x80FF90);
        label(lab, "RAM", rp); text(lab, 6, 124, 3, 0x80E0FF);
        sys_gfx_blit(FB);

        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        sys_sleep(500);
    }
    return 0;
}
