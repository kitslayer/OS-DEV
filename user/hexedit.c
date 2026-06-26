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
#define ROWS 12            /* visible rows (title + header + ROWS + blank + status <= 17) */

static unsigned char buf[CAP];     /* the working bytes (BSS, not the stack) */
static unsigned char orig[CAP];    /* the loaded bytes, to flag unsaved edits */
static long flen;
static int  ro;            /* file hit CAP: it may be larger, so view-only (saving would truncate) */
static char fname[64];
static int  gmode; static long gval;   /* 'g' goto-offset input mode + the offset being typed (M1348) */
static int  smode; static unsigned char spat[16]; static int splen, snib, sfail;   /* '/' find-bytes mode (M1349) */

static void hx(int v) { char s[2] = { "0123456789abcdef"[v & 15], 0 }; print(s); }
static void hoff(long o) { hx((int)((o >> 12) & 15)); hx((int)((o >> 8) & 15)); hx((int)((o >> 4) & 15)); hx((int)(o & 15)); }
static long find_from(long start) {   /* next occurrence of spat[0..splen) from `start`, wrapping; -1 if none (M1354) */
    if (splen <= 0) return -1;
    for (long i = start; i + splen <= flen; i++) { int m = 1; for (int j = 0; j < splen; j++) if (buf[i+j] != spat[j]) { m = 0; break; } if (m) return i; }
    for (long i = 0; i < start && i + splen <= flen; i++) { int m = 1; for (int j = 0; j < splen; j++) if (buf[i+j] != spat[j]) { m = 0; break; } if (m) return i; }
    return -1;
}

static void render(long cur, long top, int nibble, int dirty) {
    sys_clear();
    sys_setcolor(4); print(" hexedit "); sys_setcolor(8); print(fname);
    if (dirty) print(" *"); if (ro) { sys_setcolor(2); print(" [RO]"); }
    sys_setcolor(8); print("  @"); hoff(cur); if (nibble) print(" lo");
    if (sfail) { sys_setcolor(2); print(" !notfound"); } sys_setcolor(0); print("\n");   /* offset (+ last-find result) in the title (M1343/M1349) */
    sys_setcolor(8); print("      00 01 02 03 04 05 06 07\n"); sys_setcolor(0);   /* column-index header, aligned over the hex bytes (M1345) */
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
    if (gmode) { print("\n goto: "); hoff(gval); print("  (enter jump, esc cancel)"); }
    else if (smode) { print("\n find: "); for (int i = 0; i < splen; i++) { hx(spat[i] >> 4); hx(spat[i] & 15); print(" "); } print(" (enter, esc)"); }
    else print("\n arrows/np/g  /N find  0-9a-f edit s save");   /* <44 cols so the cursor stays (M1343/M1354) */
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
        if (gmode) {                                             /* goto-offset input (M1348) */
            if (k == 27) gmode = 0;                              /* cancel */
            else if (k == '\n' || k == '\r') { cur = (gval < flen) ? gval : flen - 1; nibble = 0; gmode = 0; }
            else if (k == 8 || k == 127) gval /= 16;             /* backspace a nibble */
            else { int d = (k>='0'&&k<='9') ? k-'0' : (k>='a'&&k<='f') ? k-'a'+10 : (k>='A'&&k<='F') ? k-'A'+10 : -1;
                   if (d >= 0) gval = gval * 16 + d; }
            if (cur < top) top = (cur / BPR) * BPR;
            if (cur >= top + (long)ROWS * BPR) top = (cur / BPR - ROWS + 1) * BPR;
            if (top < 0) top = 0;
            render(cur, top, nibble, dirty);
            continue;
        }
        if (smode) {                                             /* find-pattern input (M1349) */
            if (k == 27) smode = 0;
            else if (k == '\n' || k == '\r') {
                smode = 0;
                if (splen > 0) { long f = find_from(cur + 1); if (f >= 0) { cur = f; nibble = 0; sfail = 0; } else sfail = 1; }
            }
            else if (k == 8 || k == 127) { if (snib) snib = 0; else if (splen > 0) splen--; }
            else { int d = (k>='0'&&k<='9') ? k-'0' : (k>='a'&&k<='f') ? k-'a'+10 : (k>='A'&&k<='F') ? k-'A'+10 : -1;
                   if (d >= 0) { if (snib == 0) { if (splen < 16) { spat[splen] = (unsigned char)(d << 4); snib = 1; } }
                                 else { spat[splen] = (unsigned char)((spat[splen] & 0xF0) | d); splen++; snib = 0; } } }
            if (cur < top) top = (cur / BPR) * BPR;
            if (cur >= top + (long)ROWS * BPR) top = (cur / BPR - ROWS + 1) * BPR;
            if (top < 0) top = 0;
            render(cur, top, nibble, dirty);
            continue;
        }
        sfail = 0;                                               /* any normal key clears the find indicator */
        if (k == 'q' || k == 27) break;
        else if (k == '/') { smode = 1; splen = 0; snib = 0; sfail = 0; }   /* enter find mode */
        else if (k == 'N') { long f = find_from(cur + 1); if (f >= 0) { cur = f; nibble = 0; sfail = 0; } else sfail = 1; }   /* find-next, reuse last pattern (M1354) */
        else if (k == 'g') { gmode = 1; gval = 0; }              /* enter goto mode */
        else if (k == 's' && !ro) {                              /* save (disabled when read-only) */
            if (sys_writefile(fname, (char *)buf, flen) >= 0) {
                for (long i = 0; i < flen; i++) orig[i] = buf[i]; dirty = 0;
            }
        }
        else if (k == 0x13) { if (cur > 0) cur--; nibble = 0; }             /* left */
        else if (k == 0x14) { if (cur < flen - 1) cur++; nibble = 0; }      /* right */
        else if (k == 0x11) { if (cur >= BPR) cur -= BPR; nibble = 0; }     /* up */
        else if (k == 0x12) { if (cur + BPR < flen) cur += BPR; nibble = 0; } /* down */
        else if (k == 'n') { cur += (long)ROWS * BPR; if (cur >= flen) cur = flen - 1; nibble = 0; }  /* page down */
        else if (k == 'p') { cur -= (long)ROWS * BPR; if (cur < 0) cur = 0; nibble = 0; }             /* page up */
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
