/*
 * hexedit.c — an interactive hex editor, a userspace program (M1342).
 *
 * The OS had `hexdump` (view-only, in the shell) and a text editor, but no way
 * to EDIT raw bytes. This loads a file, shows it as an offset / hex / ASCII grid
 * (8 bytes per row), lets you move with the arrow keys and overtype bytes with
 * hex digits (high nibble then low), and save with 's'. Ring-3, like the games.
 *
 * Colours match the shell's hexdump style: offset grey, ASCII pane blue, the
 * cursor byte cyan, edited-but-unsaved bytes amber, the status line grey.
 *
 * Launch: `hexedit <file>` from the shell, or the Apps menu (opens README.TXT).
 * Input is limited to printables + the four arrows (kernel cooks no PgUp/Ctrl),
 * so navigation is arrows-only and the window auto-scrolls to follow the cursor.
 */
#include "ulib.h"

#define CAP  32768         /* largest file we load to edit (bigger -> read-only) */
#define BPR  8             /* bytes per row */
#define ROWS 14            /* visible rows (title + ROWS + status <= 17) */

static unsigned char buf[CAP];     /* the working bytes (BSS, not the stack) */
static unsigned char orig[CAP];    /* the loaded bytes, to flag unsaved edits */
static long flen;
static int  ro;            /* file hit CAP: it may be larger, so view-only (saving would truncate) */
static char fname[64];

static void hx(int v) { char s[2] = { "0123456789abcdef"[v & 15], 0 }; print(s); }
static void hoff(long o) { hx((int)((o >> 12) & 15)); hx((int)((o >> 8) & 15)); hx((int)((o >> 4) & 15)); hx((int)(o & 15)); }

static void render(long cur, long top, int nibble, int dirty) {
    sys_clear();
    sys_setcolor(4); print(" hexedit "); sys_setcolor(8); print(fname);
    if (dirty) print(" *"); if (ro) { sys_setcolor(2); print(" [RO]"); } sys_setcolor(0); print("\n");
    for (int r = 0; r < ROWS; r++) {
        long off = top + (long)r * BPR;
        if (off >= flen) break;
        sys_setcolor(8); hoff(off); print("  ");                 /* offset grey */
        for (int i = 0; i < BPR; i++) {                          /* hex bytes */
            long o = off + i;
            if (o < flen) {
                if (o == cur) sys_setcolor(4);                   /* cursor: cyan */
                else if (buf[o] != orig[o]) sys_setcolor(7);     /* unsaved edit: amber */
                else sys_setcolor(0);
                hx(buf[o] >> 4); hx(buf[o] & 15); sys_setcolor(0); print(" ");
            } else print("   ");
        }
        print(" ");
        for (int i = 0; i < BPR; i++) {                          /* ASCII pane */
            long o = off + i;
            if (o >= flen) break;
            unsigned char c = buf[o]; char d = (c >= 32 && c < 127) ? (char)c : '.';
            sys_setcolor(o == cur ? 4 : 6);                      /* cursor char cyan, rest blue */
            char s[2] = { d, 0 }; print(s);
        }
        sys_setcolor(0); print("\n");
    }
    sys_setcolor(8);
    print("\n arrows move  0-9a-f edit  s save  q quit   @"); hoff(cur);
    if (nibble) print(" lo");
    sys_setcolor(0);
}

int main(void) {
    if (sys_getarg(fname, sizeof fname) <= 0) {                            /* default file */
        const char *d = "README.TXT"; int i = 0; while (d[i]) { fname[i] = d[i]; i++; } fname[i] = 0;
    }
    flen = sys_readfile(fname, (char *)buf, CAP);
    if (flen < 0) { print("hexedit: cannot read "); print(fname); print("\n"); return 1; }
    if (flen == 0) { print("hexedit: "); print(fname); print(" is empty\n"); return 1; }
    for (long i = 0; i < flen; i++) orig[i] = buf[i];
    ro = (flen == CAP);    /* read filled the buffer -> the file may be truncated; refuse to save it back */

    long cur = 0, top = 0; int nibble = 0, dirty = 0;
    render(cur, top, nibble, dirty);
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 27) break;
        else if (k == 's' && !ro) {                              /* save (disabled when read-only) */
            if (sys_writefile(fname, (char *)buf, flen) >= 0) {
                for (long i = 0; i < flen; i++) orig[i] = buf[i]; dirty = 0;
            }
        }
        else if (k == 0x13) { if (cur > 0) cur--; nibble = 0; }             /* left */
        else if (k == 0x14) { if (cur < flen - 1) cur++; nibble = 0; }      /* right */
        else if (k == 0x11) { if (cur >= BPR) cur -= BPR; nibble = 0; }     /* up */
        else if (k == 0x12) { if (cur + BPR < flen) cur += BPR; nibble = 0; } /* down */
        else {
            int d = -1;
            if (k >= '0' && k <= '9') d = k - '0';
            else if (k >= 'a' && k <= 'f') d = k - 'a' + 10;
            else if (k >= 'A' && k <= 'F') d = k - 'A' + 10;
            if (d < 0 || cur >= flen) continue;                  /* ignore other keys */
            if (nibble == 0) { buf[cur] = (unsigned char)((d << 4) | (buf[cur] & 0x0F)); nibble = 1; }
            else { buf[cur] = (unsigned char)((buf[cur] & 0xF0) | d); nibble = 0; if (cur < flen - 1) cur++; }
            dirty = 1;
        }
        if (cur < top) top = (cur / BPR) * BPR;                  /* scroll to follow the cursor */
        if (cur >= top + (long)ROWS * BPR) top = (cur / BPR - ROWS + 1) * BPR;
        if (top < 0) top = 0;
        render(cur, top, nibble, dirty);
    }
    return 0;
}
