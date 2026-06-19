/*
 * editor.c — a small full-screen text editor (a 5th userspace program).
 *
 * Brings together the input pieces: it reads keys one at a time without
 * blocking (sys_pollkey), moves a cursor with the arrow keys, edits a buffer,
 * and saves to the FAT32 disk. The cursor is shown inline as a '|'. The view
 * scrolls to keep the cursor visible (centred) in long files.
 */
#include "ulib.h"

#define MAXDOC 65536              /* editable file size (each editor process has its own copy) */
#define EDCOLS 44                 /* must match the app text grid */
#define EDVIS  16                 /* visible text rows (grid is 17; 1 is the status line) */

static char doc[MAXDOC];
static int  dlen, cur, readonly;  /* readonly: file exceeded the buffer — view only, never save (would truncate it) */
static char fname[40];

static void itoa_i(int v, char *o) {
    char t[12]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0; while (i) o[j++] = t[--i];
    o[j] = 0;
}

static void insert(char c) {
    if (readonly || dlen >= MAXDOC - 1) return;
    for (int i = dlen; i > cur; i--) doc[i] = doc[i-1];
    doc[cur++] = c; dlen++;
}
static void backspace(void) {
    if (readonly || cur == 0) return;
    for (int i = cur - 1; i < dlen - 1; i++) doc[i] = doc[i+1];
    dlen--; cur--;
}
static void del_fwd(void) {                 /* Delete key: remove the char at the cursor */
    if (readonly || cur >= dlen) return;
    for (int i = cur; i < dlen - 1; i++) doc[i] = doc[i+1];
    dlen--;
}

/* up/down move to the same column in the adjacent line (split on '\n'). */
static void move_vert(int down) {
    int ls = cur; while (ls > 0 && doc[ls-1] != '\n') ls--;
    int col = cur - ls;
    if (!down) {
        if (ls == 0) return;
        int ps = ls - 1; while (ps > 0 && doc[ps-1] != '\n') ps--;
        int plen = 0; while (ps + plen < ls - 1 && doc[ps+plen] != '\n') plen++;
        cur = ps + (col < plen ? col : plen);
    } else {
        int le = cur; while (le < dlen && doc[le] != '\n') le++;
        if (le >= dlen) return;
        int ns = le + 1, nlen = 0;
        while (ns + nlen < dlen && doc[ns+nlen] != '\n') nlen++;
        cur = ns + (col < nlen ? col : nlen);
    }
}

/* Wrap-aware grid-row of doc position `pos` (a '\n' or hitting EDCOLS wraps). */
static int row_of(int pos) {
    int row = 0, col = 0;
    for (int i = 0; i < pos && i < dlen; i++) {
        if (doc[i] == '\n') { row++; col = 0; }
        else if (++col == EDCOLS) { row++; col = 0; }
    }
    return row;
}
/* Doc offset where grid-row `target` begins (or dlen if past the end). */
static int row_offset(int target) {
    if (target <= 0) return 0;
    int row = 0, col = 0;
    for (int i = 0; i < dlen; i++) {
        if (doc[i] == '\n') { row++; col = 0; }
        else if (++col == EDCOLS) { row++; col = 0; }
        if (row == target) return i + 1;
    }
    return dlen;
}

static void render(const char *msg) {
    sys_clear();
    /* cursor line:col (1-based) for the status line */
    int ln = 1, lst = 0;
    for (int i = 0; i < cur && i < dlen; i++) if (doc[i] == '\n') { ln++; lst = i + 1; }
    int cl = (cur - lst) + 1;
    char st[96]; int p = 0;
    const char *a = "EDIT "; while (*a) st[p++] = *a++;
    for (int i = 0; fname[i] && p < 30; i++) st[p++] = fname[i];
    a = readonly ? "  ESC=quit [RO: file too big]  " : "  ESC=save&quit  "; while (*a) st[p++] = *a++;
    char nb[12]; itoa_i(dlen, nb); for (int i = 0; nb[i]; i++) st[p++] = nb[i];
    a = "b  "; while (*a) st[p++] = *a++;
    itoa_i(ln, nb); for (int i = 0; nb[i]; i++) st[p++] = nb[i];   /* line */
    st[p++] = ':';
    itoa_i(cl, nb); for (int i = 0; nb[i]; i++) st[p++] = nb[i];   /* col */
    st[p] = 0;
    sys_setcolor(4); print(st); print("\n"); sys_setcolor(0);   /* status line: cyan */

    /* Scroll a window around the cursor so it's always visible (centred when
     * possible), and print exactly EDVIS grid-rows so the grid doesn't scroll
     * the cursor back off. */
    int crow = row_of(cur);
    int start = crow - EDVIS / 2; if (start < 0) start = 0;
    int off = row_offset(start);
    static char out[2048];                 /* off the stack; only ever holds the EDVIS visible rows */
    int o = 0, row = 0, col = 0;
    for (int i = off; i <= dlen && row < EDVIS && o < (int)sizeof(out) - 4; i++) {
        if (i == cur) out[o++] = '|';
        if (i < dlen) {
            char ch = doc[i];
            out[o++] = ch;
            if (ch == '\n') { row++; col = 0; }
            else if (++col == EDCOLS) { row++; col = 0; }
        }
    }
    out[o] = 0;
    print(out);
    if (msg) print(msg);
}

int main(void) {
    print("\n  file to edit: ");
    readline(fname, sizeof(fname));    /* prompt keeps the system caret (it's a readline) */
    sys_caret(0);                      /* editor body draws its own '|' cursor; hide the block caret */
    if (fname[0] == 0) { fname[0] = 'N'; fname[1] = 'O'; fname[2] = 'T'; fname[3] = 'E';
                         fname[4] = '.'; fname[5] = 'T'; fname[6] = 'X'; fname[7] = 'T'; fname[8] = 0; }
    long n = sys_readfile(fname, doc, MAXDOC - 1);
    dlen = (n > 0) ? (int)n : 0;
    readonly = (n >= MAXDOC - 1);    /* read filled the buffer: the file is larger -> view only (saving would truncate it) */
    cur = dlen;

    render(0);
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 27) {                              /* ESC: save and quit (read-only if the file was too large) */
            if (readonly) render("\n[not saved: file too large to edit]");
            else if (sys_writefile(fname, doc, (unsigned long)dlen) < 0) render("\n[save failed]");
            else { render("\n[saved - bye]"); }
            sys_sleep(400);
            return 0;
        }
        else if (k == '\n' || k == '\r') insert('\n');
        else if (k == 8 || k == 127)     backspace();
        else if (k == 0x04)              del_fwd();        /* Delete: forward-delete  */
        else if (k == 0x13) { if (cur > 0) cur--; }       /* left  */
        else if (k == 0x14) { if (cur < dlen) cur++; }    /* right */
        else if (k == 0x11) move_vert(0);                 /* up    */
        else if (k == 0x12) move_vert(1);                 /* down  */
        else if (k == 0x15) { for (int i = 0; i < EDVIS - 1; i++) move_vert(0); }  /* PgUp / wheel up   */
        else if (k == 0x16) { for (int i = 0; i < EDVIS - 1; i++) move_vert(1); }  /* PgDn / wheel down */
        else if (k == 0x01) { while (cur > 0 && doc[cur-1] != '\n') cur--; }    /* Home: line start */
        else if (k == 0x05) { while (cur < dlen && doc[cur] != '\n') cur++; }   /* End:  line end   */
        else if (k == '\t') {                             /* Tab: spaces to the next 4-col stop */
            int ls = cur; while (ls > 0 && doc[ls-1] != '\n') ls--;
            for (int sp = 4 - ((cur - ls) % 4); sp > 0; sp--) insert(' ');
        }
        else if (k >= 32 && k < 127) insert((char)k);
        render(0);
    }
}
