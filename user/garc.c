/*
 * garc.c — an archive browser, a userspace program (M1707).
 *
 * The shell can extract a .zip / .tar / .tar.gz (unzip/tar), but nothing could
 * LIST an archive's contents without extracting it. garc does: it reads the
 * archive, lists every entry with its size (directories marked), and can
 * extract-all with one key. gzip'd tarballs (.tgz/.tar.gz) are transparently
 * gunzipped first (via sys_gunzip) and then listed as a tar. The listing engine
 * (ZIP central-directory + TAR header parsing) lives in the pure user/arccore.h,
 * host-unit-tested by tests/arc like calc/sheet/plot/gjson/diff's cores.
 *
 * Launch: `garc FILE` (or `garc` for a built-in demo, TEST.ZIP). Keys: up/down
 * scroll, x extract-all, Esc/q quit.
 */
#include "ulib.h"
#include "arccore.h"        /* arc_list() + arc_ent[] — the pure archive lister (host-tested by tests/arc) */

#define IOMAX    (4 * 1024 * 1024)
#define VIEWROWS 18

static unsigned char buf[IOMAX];
static char fname[64];
static char status[80];
static int  top, fmt;                     /* fmt: ARC_ZIP / ARC_TAR (post-gunzip) / ARC_GZIP */

static int  slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scopy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }
static void putn_pad(long v, int w) {                       /* right-align v in w cols */
    char b[16]; int n = 0; long a = v;
    if (a == 0) b[n++] = '0';
    while (a) { b[n++] = (char)('0' + a % 10); a /= 10; }
    for (int i = n; i < w; i++) print(" ");
    char c[2] = {0,0}; while (n) { c[0] = b[--n]; print(c); }
}

static void load(void) {
    top = 0; arc_n = 0; status[0] = 0;
    long n = sys_readfile(fname, buf, IOMAX);
    if (n <= 0) { scopy(status, "cannot read file", sizeof status); return; }
    fmt = arc_detect(buf, n);
    if (fmt == ARC_GZIP) {                                  /* .tgz/.tar.gz: gunzip, then list as tar */
        if (sys_gunzip(fname, "ARCTMP.TAR") < 0) { scopy(status, "gunzip failed", sizeof status); return; }
        n = sys_readfile("ARCTMP.TAR", buf, IOMAX);
        if (n <= 0) { scopy(status, "gunzip produced nothing", sizeof status); return; }
    }
    if (arc_list(buf, n) < 0) { scopy(status, "not a zip/tar archive", sizeof status); return; }
    long total = 0; for (int i = 0; i < arc_n; i++) total += arc_ent[i].size;
    /* status = "<N> entries, <total> bytes" */
    int p = 0; long v = arc_n; char t[16]; int tn = 0;
    if (v == 0) t[tn++] = '0';
    while (v) { t[tn++] = (char)('0' + v % 10); v /= 10; }
    while (tn) status[p++] = t[--tn];
    scopy(status + p, arc_n == 1 ? " entry, " : " entries, ", (int)sizeof status - p); p = slen(status);
    v = total; tn = 0; if (v == 0) t[tn++] = '0'; while (v) { t[tn++] = (char)('0' + v % 10); v /= 10; }
    while (tn) status[p++] = t[--tn];
    scopy(status + p, " bytes uncompressed", (int)sizeof status - p);
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print(" archive "); sys_setcolor(1); print(fname);
    sys_setcolor(8); print("   "); print(status); sys_setcolor(0); print("\n\n");
    for (int r = 0; r < VIEWROWS; r++) {
        int e = top + r;
        if (e >= arc_n) { print("\n"); continue; }
        if (arc_ent[e].isdir) { sys_setcolor(6); print("    <dir> "); }
        else { sys_setcolor(3); putn_pad(arc_ent[e].size, 10); }
        print("  ");
        sys_setcolor(arc_ent[e].isdir ? 6 : 1); print(arc_ent[e].name);
        sys_setcolor(0); print("\n");
    }
    sys_setcolor(8); print(" up/dn scroll  x extract-all  Esc quit");
    sys_setcolor(0);
}

int main(void) {
    char arg[64];
    if (sys_getarg(arg, sizeof arg) > 0 && arg[0] && !streq(arg, "demo")) scopy(fname, arg, sizeof fname);
    else scopy(fname, "TEST.ZIP", sizeof fname);
    load();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(15); continue; }
        if (k == 27 || k == 'q') break;
        else if (k == 0x11) { if (top > 0) top--; render(); }
        else if (k == 0x12) { if (top < arc_n - 1) top++; render(); }
        else if (k == 0x13) { top -= VIEWROWS; if (top < 0) top = 0; render(); }
        else if (k == 0x14) { top += VIEWROWS; if (top > arc_n - 1) top = arc_n - 1; if (top < 0) top = 0; render(); }
        else if (k == 'x') {                                /* extract-all via the existing syscalls */
            long rc;
            if (fmt == ARC_ZIP) rc = sys_unzip(fname);
            else if (fmt == ARC_GZIP) rc = sys_untar("ARCTMP.TAR");
            else rc = sys_untar(fname);
            scopy(status, rc >= 0 ? "extracted to the root directory" : "extract failed", sizeof status);
            render();
        }
    }
    sys_clear();
    return 0;
}
