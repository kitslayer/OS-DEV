/*
 * gdiff.c — a visual line-diff viewer, a userspace program (M1705).
 *
 * The shell has a terse `diff`; this is the friendly version — a scrollable,
 * colour-coded unified diff of two files (removed lines red, added lines teal,
 * context grey), with a +/- summary. The diff itself (an LCS line diff) lives in
 * the pure user/diffcore.h, host-unit-tested by tests/diff exactly like calc/
 * sheet/plot/gjson's cores. This file is just the terminal UI and file load.
 *
 * Launch: `gdiff A B` (diff two files) or `gdiff` (a built-in before/after demo).
 * Keys: up/down scroll a line, left/right page, s save a real unified-diff patch
 * (---/+++ header + @@ hunks with context) to DIFF.PATCH, Esc/q quit.
 */
#include "ulib.h"
#include "diffcore.h"        /* diff_run() + dc_out[] — the pure LCS diff (host-tested by tests/diff) */

#define IOMAX    32768
#define VIEWROWS 20

static char abuf[IOMAX], bbuf[IOMAX];
static char fa[64], fb[64];
static int  top;
static char patchbuf[2 * IOMAX];          /* unified-diff patch text for :save */
static const char *msg;                    /* transient status note (after save) */

static const char *DEMO_A =
    "OS-DEV apps\n- kernel\n- TLS 1.3\n- JS engine\n- spreadsheet\n- paint (ASCII art)\n- calc\nthe end\n";
static const char *DEMO_B =
    "OS-DEV apps\n- kernel\n- TLS 1.3\n- JS engine\n- spreadsheet\n- graphing calculator\n- paint (gfx, with tools)\n- JSON viewer\n- regex tester\n- calc\nthe end\n";

static void scopy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }
static void putn(int v) { char b[12]; int n = 0; if (v == 0) b[n++] = '0'; while (v) { b[n++] = (char)('0' + v % 10); v /= 10; } char c[2] = {0,0}; while (n) { c[0] = b[--n]; print(c); } }

static void load(void) {
    if (fa[0]) { long n = sys_readfile(fa, abuf, IOMAX - 1); if (n < 0) n = 0; abuf[n] = 0; }
    else scopy(abuf, DEMO_A, IOMAX);
    if (fb[0]) { long n = sys_readfile(fb, bbuf, IOMAX - 1); if (n < 0) n = 0; bbuf[n] = 0; }
    else scopy(bbuf, DEMO_B, IOMAX);
    diff_run(abuf, bbuf);
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print(" diff "); sys_setcolor(1);
    print(fa[0] ? fa : "(demo before)"); sys_setcolor(8); print("  ->  "); sys_setcolor(1); print(fb[0] ? fb : "(demo after)");
    sys_setcolor(10); print("   +"); putn(dc_add); sys_setcolor(2); print(" -"); putn(dc_del);
    sys_setcolor(0); print("\n");

    char c[2] = {0,0};
    for (int r = 0; r < VIEWROWS; r++) {
        int e = top + r;
        if (e >= dc_n) { print("\n"); continue; }
        char op = dc_out[e].op;
        sys_setcolor(op == '+' ? 10 : op == '-' ? 2 : 8);
        c[0] = op; print(c); print(" ");
        sys_setcolor(op == '+' ? 10 : op == '-' ? 2 : 7);
        int L = dc_out[e].len; if (L > 76) L = 76;          /* clip to the 80-col window */
        for (int i = 0; i < L; i++) { c[0] = dc_out[e].text[i]; print(c); }
        sys_setcolor(0); print("\n");
    }
    sys_setcolor(8);
    if (msg) { sys_setcolor(10); print(" "); print(msg); sys_setcolor(8); print("  "); }
    print(" up/dn scroll  s:save patch  Esc quit   ("); putn(dc_n); print(" diff lines)");
    sys_setcolor(0);
}

int main(void) {
    char arg[160];
    if (sys_getarg(arg, sizeof arg) > 0 && arg[0] && !streq(arg, "demo")) {
        int i = 0; while (arg[i] && arg[i] != ' ') i++;    /* "A B" -> two filenames */
        if (arg[i] == ' ') { arg[i] = 0; scopy(fa, arg, sizeof fa); int j = i + 1; while (arg[j] == ' ') j++; scopy(fb, arg + j, sizeof fb); }
        else scopy(fa, arg, sizeof fa);
    }
    load();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(15); continue; }
        if (k == 27 || k == 'q') break;
        else if (k == 0x11) { if (top > 0) top--; msg = 0; }
        else if (k == 0x12) { if (top < dc_n - 1) top++; msg = 0; }
        else if (k == 0x13) { top -= VIEWROWS; if (top < 0) top = 0; msg = 0; }
        else if (k == 0x14) { top += VIEWROWS; if (top > dc_n - 1) top = dc_n - 1; if (top < 0) top = 0; msg = 0; }
        else if (k == 's') {                             /* save a unified-diff patch to DIFF.PATCH */
            int n = diff_to_patch(fa[0] ? fa : "a", fb[0] ? fb : "b", patchbuf, sizeof patchbuf);
            msg = n == 0 ? "no differences to save"
                : (sys_writefile("DIFF.PATCH", patchbuf, n) >= 0 ? "saved DIFF.PATCH" : "save failed");
        }
        else continue;
        render();
    }
    sys_clear();
    return 0;
}
