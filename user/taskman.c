/*
 * taskman.c — a graphical Task Manager, a userspace program (M1362).
 *
 * The first gfx app to render REAL text: it pulls the kernel's 8x16 console
 * font through the new sys_font() syscall and uses it to draw a live, auto-
 * refreshing process list (from sys_ps()) into a WM canvas — PID, state and
 * name per row, the whole line tinted by run-state (run=green, ready=cyan,
 * blocked=amber, stopped=grey). Refreshes ~1.4x a second. q/Esc quits.
 *
 * Launch: `run taskman` from the shell, or the Apps menu ("Task Manager").
 */
#include "ulib.h"

#define W 380
#define H 330

static unsigned *FB;
static unsigned char FONT[128 * 16];           /* 128 glyphs x 16 rows, from sys_font (8x16, MSB = left) */

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void ch(char c, int px, int py, unsigned col) {
    unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16];
    for (int r = 0; r < 16; r++)
        for (int b = 0; b < 8; b++)
            if ((g[r] >> (7 - b)) & 1) putpx(px + b, py + r, col);
}
static void text(const char *s, int x, int y, unsigned col) { for (int i = 0; s[i]; i++) { ch(s[i], x, y, col); x += 8; } }

/* ---- instrument-panel UI kit (M1449; see gconv.c M1430) ---- */
#define C_FACE_T  0x232D33u
#define C_FACE_B  0x161D21u
#define C_BEZHI   0x3C4A50u
#define C_BEZLO   0x0E1316u
#define C_SCREEN  0x0A0F0Cu
#define C_SCANLN  0x0D140Fu
#define C_AMBERLO 0x7A521Au
#define C_LABEL   0xC6D0CCu
#define C_DIM     0x6E827Fu
#define C_LED     0x46E0A0u
static void fillr(int x, int y, int w, int h, unsigned c) { for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) putpx(x + i, y + j, c); }
static void vgrad(int x, int y, int w, int h, unsigned t, unsigned b) {
    for (int r = 0; r < h; r++) {
        int R = ((int)(t >> 16 & 0xFF) * (h - 1 - r) + (int)(b >> 16 & 0xFF) * r) / (h - 1);
        int G = ((int)(t >> 8  & 0xFF) * (h - 1 - r) + (int)(b >> 8  & 0xFF) * r) / (h - 1);
        int B = ((int)(t       & 0xFF) * (h - 1 - r) + (int)(b       & 0xFF) * r) / (h - 1);
        fillr(x, y + r, w, 1, ((unsigned)R << 16) | ((unsigned)G << 8) | (unsigned)B);
    }
}
static void bevel_up(int x, int y, int w, int h, unsigned hi, unsigned lo) {
    fillr(x, y, w, 1, hi); fillr(x, y, 1, h, hi); fillr(x, y + h - 1, w, 1, lo); fillr(x + w - 1, y, 1, h, lo);
}
static void bevel_dn(int x, int y, int w, int h, unsigned hi, unsigned lo) {
    fillr(x, y, w, 1, lo); fillr(x, y, 1, h, lo); fillr(x, y + h - 1, w, 1, hi); fillr(x + w - 1, y, 1, h, hi);
}
static void panel(int x, int y, int w, int h) {
    fillr(x, y, w, h, C_SCREEN);
    for (int r = 3; r < h - 1; r += 3) fillr(x + 1, y + r, w - 2, 1, C_SCANLN);
    bevel_dn(x, y, w, h, C_BEZHI, C_BEZLO);
}
static void led(int x, int y) {
    fillr(x, y, 9, 9, C_BEZLO); fillr(x + 1, y + 1, 7, 7, 0x1A6E50u);
    fillr(x + 2, y + 2, 5, 5, C_LED); putpx(x + 3, y + 3, 0xCFFFE8u);
}
static int has(const char *s, const char *sub) {            /* naive substring test */
    for (int i = 0; s[i]; i++) { int j = 0; while (sub[j] && s[i + j] == sub[j]) j++; if (!sub[j]) return 1; }
    return 0;
}
static int putint(char *o, int i, long v) {                 /* append v's decimal digits; return new index */
    char t[12]; int n = 0; if (v < 0) v = 0;
    do { t[n++] = '0' + (int)(v % 10); v /= 10; } while (v);
    while (n) o[i++] = t[--n];
    return i;
}
/* the [ pid ] from a "  [pid] state  name" line, or -1 */
static int extract_pid(const char *line) {
    int i = 0; while (line[i] && line[i] != '[') i++;
    if (!line[i]) return -1; i++;
    int v = 0, any = 0; while (line[i] >= '0' && line[i] <= '9') { v = v * 10 + (line[i++] - '0'); any = 1; }
    return any ? v : -1;
}
/* parse /proc/<pid>/stat for *cpu (utime+stime ticks, 100 Hz) and *rss (resident
   pages); 0 ok / -1 fail. After the comm ")" the numeric fields are ppid pgid sid
   tty tpgid flags minflt cminflt majflt cmajflt utime stime cutime cstime priority
   cutime cstime priority nice numthreads itrealvalue starttime vsize rss ->
   utime=nums[10], stime=nums[11], vsize=nums[19], rss=nums[20]. */
static int proc_cpu_rss(int pid, long *cpu, long *rss) {
    char path[40]; int p = 0;
    const char *pre = "/proc/"; for (int k = 0; pre[k]; k++) path[p++] = pre[k];
    p = putint(path, p, pid);
    const char *suf = "/stat"; for (int k = 0; suf[k]; k++) path[p++] = suf[k];
    path[p] = 0;
    char buf[512]; long n = sys_readfile(path, buf, sizeof buf - 1);
    if (n <= 0) return -1; buf[n] = 0;
    int rp = -1; for (int i = 0; buf[i]; i++) if (buf[i] == ')') rp = i;   /* end of comm */
    if (rp < 0) return -1;
    long nums[22]; int nc = 0;
    for (int i = rp + 1; buf[i] && nc < 22; ) {
        if (buf[i] >= '0' && buf[i] <= '9') { long v = 0; while (buf[i] >= '0' && buf[i] <= '9') v = v * 10 + (buf[i++] - '0'); nums[nc++] = v; }
        else i++;
    }
    if (nc < 21) return -1;
    *cpu = nums[10] + nums[11];
    *rss = nums[20];
    return 0;
}
/* aggregate system CPU% since the last call (busy/total delta of /proc/stat's
   "cpu  user nice system idle ..." line); updates the pbusy/ptot accumulators. */
static int agg_cpu(long *pbusy, long *ptot) {
    char b[600]; long n = sys_readfile("/proc/stat", b, sizeof b - 1);
    if (n <= 0) return 0; b[n] = 0;
    long nums[4] = { 0, 0, 0, 0 }; int nc = 0;
    for (int i = 0; b[i] && nc < 4; ) {
        if (b[i] >= '0' && b[i] <= '9') { long v = 0; while (b[i] >= '0' && b[i] <= '9') v = v * 10 + (b[i++] - '0'); nums[nc++] = v; }
        else i++;
    }
    long busy = nums[0] + nums[1] + nums[2], tot = busy + nums[3];
    int pct = 0;
    if (*ptot > 0 && tot > *ptot) { pct = (int)((busy - *pbusy) * 100 / (tot - *ptot)); if (pct < 0) pct = 0; if (pct > 100) pct = 100; }
    *pbusy = busy; *ptot = tot;
    return pct;
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("taskman: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("taskman: init failed\n"); return 1; }

    char ps[1200];
    int prev_pid[40]; long prev_cpu[40]; int prev_n = 0; long prev_ms = 0;   /* last snapshot, for CPU% deltas */
    int sel = 0, row_pid[40];                                                /* keyboard selection + per-row PID (M1401) */
    long pbusy = 0, ptot = 0;                                                /* aggregate-CPU accumulators */
    for (;;) {
        long n = sys_ps(ps, sizeof ps - 1); if (n < 0) n = 0; ps[n] = 0;
        long now_ms = sys_uptime_ms();
        long ival = (prev_ms > 0 && now_ms > prev_ms) ? now_ms - prev_ms : 700;
        int scpu = agg_cpu(&pbusy, &ptot);                     /* whole-system CPU% (reconciles with sysgraph) */
        int cur_pid[40]; long cur_cpu[40]; int cur_n = 0;

        vgrad(0, 0, W, H, C_FACE_T, C_FACE_B);                      /* slate faceplate (M1449) */
        bevel_up(0, 0, W, H, C_BEZHI, C_BEZLO);
        text("TASK MANAGER", 12, 8, C_LABEL);                       /* silkscreen title */
        fillr(12, 26, 112, 2, C_AMBERLO);                           /* amber title rule */
        led(W - 24, 9);                                             /* power LED */
        text("MEM", W - 76, 8, C_DIM); text("CPU", W - 38, 8, C_DIM);   /* right-aligned column heads */
        panel(6, 32, W - 12, H - 32 - 24);                          /* recessed process-list screen */

        int y = 42, count = 0;
        for (int i = 0; ps[i]; ) {
            int eol = i; while (ps[eol] && ps[eol] != '\n') eol++;
            char line[80]; int s = 0; for (int k = i; k < eol && s < 79; k++) line[s++] = ps[k]; line[s] = 0;
            if (s > 0) {
                if (count == sel) { for (int x = 10; x < W - 10; x++) for (int yy = -2; yy <= 15; yy++) putpx(x, y + yy, 0x182840); fillr(10, y - 2, 2, 18, 0xFFB23E); }  /* lit selected-row bar + amber edge (M1401/M1449) */
                unsigned col = C_LABEL;                             /* default */
                if      (has(line, "run"))   col = 0x5CE070;        /* running: green */
                else if (has(line, "ready")) col = 0x60C8F0;        /* ready:   cyan  */
                else if (has(line, "block")) col = 0xE0B050;        /* blocked: amber */
                else if (has(line, "stop"))  col = 0x9090A0;        /* stopped: grey  */
                text(line, 12, y, col);
                int pid = extract_pid(line);                /* per-task CPU% (delta) + MEM (RSS) from /proc/<pid>/stat (M1371) */
                if (count < 40) row_pid[count] = pid;       /* remember this row's PID for the close key (M1401) */
                if (pid >= 0) {
                    long t = 0, rss = 0; int pct = 0;
                    if (proc_cpu_rss(pid, &t, &rss) == 0) {
                        for (int q = 0; q < prev_n; q++) if (prev_pid[q] == pid) {
                            long dt = (t - prev_cpu[q]) * 1000;     /* ticks(10ms) scaled; pct = dt / interval_ms */
                            if (dt > 0 && ival > 0) { pct = (int)(dt / ival); if (pct > 100) pct = 100; }
                            break;
                        }
                        if (cur_n < 40) { cur_pid[cur_n] = pid; cur_cpu[cur_n] = t; cur_n++; }
                        long kib = rss * 4; char ms[12]; int mi;             /* RSS pages -> KiB, human-readable */
                        if (kib >= 1024) { mi = putint(ms, 0, kib / 1024); ms[mi++] = '.'; mi = putint(ms, mi, (kib % 1024) * 10 / 1024); ms[mi++] = 'M'; }
                        else { mi = putint(ms, 0, kib); ms[mi++] = 'K'; }
                        ms[mi] = 0;
                        text(ms, W - 52 - mi * 8, y, 0x90A0B0);                     /* right-aligned column */
                    }
                    char cs[8]; int ci = putint(cs, 0, pct); cs[ci++] = '%'; cs[ci] = 0;
                    unsigned ccol = pct >= 50 ? 0xE08050 : pct >= 15 ? 0xD0D060 : 0x607080;
                    text(cs, W - 14 - ci * 8, y, ccol);
                }
                y += 18; count++;
            }
            if (ps[eol] == '\n') eol++;
            i = eol;
            if (y > H - 22) break;
        }
        for (int q = 0; q < cur_n; q++) { prev_pid[q] = cur_pid[q]; prev_cpu[q] = cur_cpu[q]; }
        prev_n = cur_n; prev_ms = now_ms;                       /* roll the snapshot forward */

        long up = now_ms / 1000;                                /* system uptime, seconds */
        char foot[72]; int fi = putint(foot, 0, count);
        const char *a = " tasks    up "; for (int k = 0; a[k]; k++) foot[fi++] = a[k];
        fi = putint(foot, fi, up / 60); foot[fi++] = 'm';
        if (up % 60 < 10) foot[fi++] = '0';
        fi = putint(foot, fi, up % 60); foot[fi++] = 's';
        const char *cl = "    CPU "; for (int k = 0; cl[k]; k++) foot[fi++] = cl[k];
        fi = putint(foot, fi, scpu); foot[fi++] = '%';
        const char *kh = "   up/dn k:close"; for (int z = 0; kh[z]; z++) foot[fi++] = kh[z]; foot[fi] = 0;
        text(foot, 12, H - 18, C_DIM);

        sys_gfx_blit(FB);
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        else if ((k == 0x11 || k == 'w') && sel > 0) sel--;                  /* up: move selection */
        else if ((k == 0x12 || k == 's') && sel < count - 1) sel++;          /* down */
        else if ((k == 'k' || k == '\n' || k == '\r') && sel >= 0 && sel < count && row_pid[sel] > 1) sys_kill(row_pid[sel]);  /* close the selected task (skip pid<=1) */
        if (count > 0) { if (sel >= count) sel = count - 1; if (sel < 0) sel = 0; }
        sys_sleep(300);
    }
    return 0;
}
