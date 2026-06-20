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
static int  sel_anchor = -1;      /* selection mark (Ctrl-B); -1 = none. Selection spans [min, max](anchor,cur) */
static char fname[40];
static char findq[40]; static int finding, goting;   /* Ctrl-F find / Ctrl-G go-to-line: shared query buffer + mode flags */

/* ---- undo (Ctrl-Z) -------------------------------------------------------
 * A log of single-character edits, newest last. Each op remembers the char,
 * where it happened, and whether it was an insertion or a deletion, so it can
 * be reversed. Consecutive same-kind edits at adjacent positions share a
 * 'group', so one Ctrl-Z reverts a whole typed (or deleted) run rather than a
 * single character. Continuity is judged purely on the (absolute) edit
 * position, so moving the caret naturally starts a new undo group. */
#define UNDO_MAX 16384
struct uop { int pos, grp; unsigned char ch, kind; };   /* kind: 0=insert 1=backspace 2=delete-fwd */
static struct uop ulog[UNDO_MAX];
static int un, umax, ugrp, uexpect = -1, ulast_kind = -1;
/* un = ops currently applied; umax = ops in the log (>= un). Undo decrements un
 * but leaves the ops in place, so [un, umax) is the redo region; a fresh edit
 * truncates it (umax = un). */

static void undo_record(int pos, char ch, int kind) {
    if (un >= UNDO_MAX) {                       /* log full: drop the oldest half */
        int keep = UNDO_MAX / 2;
        for (int i = 0; i < keep; i++) ulog[i] = ulog[un - keep + i];
        un = keep;
    }
    if (!(un > 0 && kind == ulast_kind && pos == uexpect)) ugrp++;   /* discontinuity -> new group */
    ulog[un].pos = pos; ulog[un].grp = ugrp;
    ulog[un].ch = (unsigned char)ch; ulog[un].kind = (unsigned char)kind;
    un++;
    umax = un;                                  /* a new edit invalidates the redo region */
    ulast_kind = kind;
    uexpect = (kind == 0) ? pos + 1 : (kind == 1) ? pos - 1 : pos;   /* next contiguous pos */
}

/* Force the next edit to begin a fresh undo group (e.g. after a newline, so
 * Ctrl-Z reverts a line at a time rather than the whole typing session). */
static void undo_break(void) { ulast_kind = -1; uexpect = -1; }

static void undo(void) {
    if (un == 0) return;
    int g = ulog[un - 1].grp;
    while (un > 0 && ulog[un - 1].grp == g) {   /* reverse the whole top group, newest first */
        struct uop *o = &ulog[--un];
        if (o->kind == 0) {                     /* was an insertion: delete the char at pos */
            if (o->pos < dlen) {
                for (int i = o->pos; i < dlen - 1; i++) doc[i] = doc[i+1];
                dlen--;
            }
            cur = o->pos;
        } else if (dlen < MAXDOC - 1) {         /* was a deletion: re-insert the char at pos */
            for (int i = dlen; i > o->pos; i--) doc[i] = doc[i-1];
            doc[o->pos] = (char)o->ch; dlen++;
            cur = o->pos + 1;
        }
    }
    if (cur > dlen) cur = dlen;
    ulast_kind = -1; uexpect = -1;              /* the undo itself is a group boundary */
}

static void redo(void) {
    if (un >= umax) return;                     /* nothing undone to re-apply */
    int g = ulog[un].grp;
    while (un < umax && ulog[un].grp == g) {    /* re-apply the whole group, oldest first */
        struct uop *o = &ulog[un++];
        if (o->kind == 0) {                     /* insertion: put the char back at pos */
            if (dlen < MAXDOC - 1) {
                for (int i = dlen; i > o->pos; i--) doc[i] = doc[i-1];
                doc[o->pos] = (char)o->ch; dlen++;
            }
            cur = o->pos + 1;
        } else if (o->pos < dlen) {             /* deletion: remove the char at pos again */
            for (int i = o->pos; i < dlen - 1; i++) doc[i] = doc[i+1];
            dlen--;
            cur = o->pos;
        }
    }
    if (cur > dlen) cur = dlen;
    ulast_kind = -1; uexpect = -1;              /* redo is a group boundary too */
}

/* First offset >= start where findq occurs in doc, or -1. */
static int find_from(int start) {
    int ql = 0; while (findq[ql]) ql++;
    if (ql == 0 || start < 0) return -1;
    for (int i = start; i + ql <= dlen; i++) {
        int j = 0; while (j < ql && doc[i + j] == findq[j]) j++;
        if (j == ql) return i;
    }
    return -1;
}

static void itoa_i(int v, char *o) {
    char t[12]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0; while (i) o[j++] = t[--i];
    o[j] = 0;
}

static void insert(char c) {
    if (readonly || dlen >= MAXDOC - 1) return;
    sel_anchor = -1;
    undo_record(cur, c, 0);
    for (int i = dlen; i > cur; i--) doc[i] = doc[i-1];
    doc[cur++] = c; dlen++;
}
static void backspace(void) {
    if (readonly || cur == 0) return;
    sel_anchor = -1;
    undo_record(cur - 1, doc[cur - 1], 1);
    for (int i = cur - 1; i < dlen - 1; i++) doc[i] = doc[i+1];
    dlen--; cur--;
}
static void del_fwd(void) {                 /* Delete key: remove the char at the cursor */
    if (readonly || cur >= dlen) return;
    sel_anchor = -1;
    undo_record(cur, doc[cur], 2);
    for (int i = cur; i < dlen - 1; i++) doc[i] = doc[i+1];
    dlen--;
}

/* ---- line clipboard: Ctrl-C copy / Ctrl-X cut / Ctrl-V paste --------------
 * Operates on whole lines (no selection UI) via the shared system clipboard,
 * so a line can be carried to the shell/browser too. Cut/paste go through
 * del_fwd()/insert(), so they're undoable and coalesce into one group. */
static void line_bounds(int *ls, int *le) {
    int s = cur; while (s > 0 && doc[s-1] != '\n') s--;
    int e = cur; while (e < dlen && doc[e] != '\n') e++;
    if (e < dlen) e++;                          /* include the trailing newline */
    *ls = s; *le = e;
}
static void copy_line(void) {
    int ls, le; line_bounds(&ls, &le);
    sys_clip_set(doc + ls, le - ls);
}
static void cut_line(void) {
    if (readonly) return;
    int ls, le; line_bounds(&ls, &le);
    sys_clip_set(doc + ls, le - ls);
    cur = ls;
    for (int i = 0; i < le - ls; i++) del_fwd();
    undo_break();
}
static void paste_clip(void) {
    if (readonly) return;
    char buf[2048];
    int n = sys_clip_get(buf, sizeof buf);
    for (int i = 0; i < n; i++) insert(buf[i]);
    undo_break();
}

/* If a selection is active (Ctrl-B mark + cursor moved), fill its byte range
 * [s0,s1) and return 1; else return 0. */
static int sel_bounds(int *s0, int *s1) {
    if (sel_anchor < 0) return 0;
    int a = sel_anchor, b = cur;
    if (a > b) { int t = a; a = b; b = t; }
    if (a < 0) a = 0;
    if (b > dlen) b = dlen;
    *s0 = a; *s1 = b;
    return b > a;
}
/* Ctrl-C: copy the selection if any, else the current line. */
static void do_copy(void) {
    int s0, s1;
    if (sel_bounds(&s0, &s1)) { sys_clip_set(doc + s0, s1 - s0); sel_anchor = -1; }
    else copy_line();
}
/* Ctrl-X: cut the selection if any, else the current line (undoable). */
static void do_cut(void) {
    if (readonly) return;
    int s0, s1;
    if (sel_bounds(&s0, &s1)) {
        sys_clip_set(doc + s0, s1 - s0);
        cur = s0;
        for (int i = 0; i < s1 - s0; i++) del_fwd();   /* del_fwd also clears the mark */
        undo_break();
    } else cut_line();
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
    int total = 1; for (int i = 0; i < dlen; i++) if (doc[i] == '\n') total++;   /* line count */
    char st[96]; int p = 0;
    const char *a = "EDIT "; while (*a) st[p++] = *a++;
    for (int i = 0; fname[i] && p < 30; i++) st[p++] = fname[i];
    a = readonly ? "  ESC=quit [RO: file too big]  " : "  ESC/^S=save ^Q=quit ^Z=undo  "; while (*a) st[p++] = *a++;
    char nb[12]; itoa_i(dlen, nb); for (int i = 0; nb[i]; i++) st[p++] = nb[i];
    a = "b  L"; while (*a) st[p++] = *a++;
    itoa_i(ln, nb); for (int i = 0; nb[i]; i++) st[p++] = nb[i];   /* current line */
    st[p++] = '/';
    itoa_i(total, nb); for (int i = 0; nb[i]; i++) st[p++] = nb[i]; /* of total lines */
    a = " C"; while (*a) st[p++] = *a++;
    itoa_i(cl, nb); for (int i = 0; nb[i]; i++) st[p++] = nb[i];   /* column */
    st[p] = 0;
    sys_setcolor(4); print(st); print("\n"); sys_setcolor(0);   /* status line: cyan */

    /* Scroll a window around the cursor so it's always visible (centred when
     * possible), and print exactly EDVIS grid-rows so the grid doesn't scroll
     * the cursor back off. */
    int crow = row_of(cur);
    int start = crow - EDVIS / 2; if (start < 0) start = 0;
    int off = row_offset(start);
    int s0 = 0, s1 = 0, sel = sel_bounds(&s0, &s1);   /* active selection byte range, if any */
    static char out[2048];                 /* off the stack; only ever holds the EDVIS visible rows */
    int o = 0, row = 0, col = 0, os0 = -1, os1 = -1;
    for (int i = off; i <= dlen && row < EDVIS && o < (int)sizeof(out) - 4; i++) {
        if (sel && i == s0) os0 = o;       /* record where the selection starts/ends in the output... */
        if (sel && i == s1) os1 = o;       /* ...before the cursor mark so the caret isn't highlighted */
        if (i == cur) out[o++] = '|';
        if (i < dlen) {
            char ch = doc[i];
            out[o++] = ch;
            if (ch == '\n') { row++; col = 0; }
            else if (++col == EDCOLS) { row++; col = 0; }
        }
    }
    out[o] = 0;
    if (sel) {                             /* selection may run off the top/bottom of the window */
        if (os0 < 0 && os1 >= 0) os0 = 0;
        if (os0 >= 0 && os1 < 0) os1 = o;
    }
    if (sel && os0 >= 0 && os1 > os0) {    /* print before | selection (yellow) | after */
        char c = out[os0]; out[os0] = 0; print(out); out[os0] = c;
        sys_setcolor(3);
        c = out[os1]; out[os1] = 0; print(out + os0); out[os1] = c;
        sys_setcolor(0);
        print(out + os1);
    } else print(out);
    if (msg) print(msg);
}

/* Show the doc with a "<label><query>_" prompt at the bottom (Ctrl-F / Ctrl-G). */
static void render_prompt(const char *label) {
    char m[64]; int p = 0; m[p++] = '\n';
    for (const char *a = label; *a && p < 40; a++) m[p++] = *a;
    for (int i = 0; findq[i] && p < 60; i++) m[p++] = findq[i];
    m[p++] = '_'; m[p] = 0;
    render(m);
}

int main(void) {
    /* If launched with a filename argument (e.g. from the Files app), open it
     * directly; otherwise prompt for one. */
    if (sys_getarg(fname, sizeof(fname)) <= 0) {
        print("\n  file to edit: ");
        readline(fname, sizeof(fname));    /* prompt keeps the system caret (it's a readline) */
    } else {
        print("\n  editing "); print(fname); print("\n");
    }
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
        if (finding) {                              /* Ctrl-F find mode: edit the query, Enter searches */
            int fl = 0; while (findq[fl]) fl++;
            if (k == '\n' || k == '\r') {
                finding = 0;
                int pos = find_from(cur + 1);       /* next match after the caret... */
                if (pos < 0) pos = find_from(0);    /* ...else wrap to the top */
                if (pos >= 0) { cur = pos; render(0); }
                else render("\n[not found]");
            }
            else if (k == 27) { finding = 0; render(0); }            /* Esc: cancel find */
            else if (k == 8 || k == 127) { if (fl > 0) findq[fl-1] = 0; render_prompt("find: "); }
            else if (k >= 32 && k < 127 && fl < 39) { findq[fl] = (char)k; findq[fl+1] = 0; render_prompt("find: "); }
            continue;
        }
        if (goting) {                               /* Ctrl-G go-to-line: type a number, Enter jumps */
            int fl = 0; while (findq[fl]) fl++;
            if (k == '\n' || k == '\r') {
                goting = 0;
                int target = 0; for (int i = 0; findq[i]; i++) target = target * 10 + (findq[i] - '0');
                if (target > 0) {                   /* move the caret to the start of line `target` */
                    int ln = 1, pos = 0;
                    while (pos < dlen && ln < target) { if (doc[pos] == '\n') ln++; pos++; }
                    cur = pos;
                }
                render(0);
            }
            else if (k == 27) { goting = 0; render(0); }             /* Esc: cancel */
            else if (k == 8 || k == 127) { if (fl > 0) findq[fl-1] = 0; render_prompt("goto line: "); }
            else if (k >= '0' && k <= '9' && fl < 8) { findq[fl] = (char)k; findq[fl+1] = 0; render_prompt("goto line: "); }
            continue;
        }
        if (k == 27) {                              /* ESC: save and quit (read-only if the file was too large) */
            if (readonly) render("\n[not saved: file too large to edit]");
            else if (sys_writefile(fname, doc, (unsigned long)dlen) < 0) render("\n[save failed]");
            else { render("\n[saved - bye]"); }
            sys_sleep(400);
            return 0;
        }
        else if (k == 0x93) {                       /* Ctrl-S: save, keep editing */
            if (readonly) render("\n[not saved: file too large]");
            else if (sys_writefile(fname, doc, (unsigned long)dlen) < 0) render("\n[save failed]");
            else render("\n[saved]");
            sys_sleep(300); render(0);
        }
        else if (k == 0x91) {                       /* Ctrl-Q: quit WITHOUT saving */
            render("\n[quit - changes not saved]"); sys_sleep(350); return 0;
        }
        else if (k == 0x86) { finding = 1; render_prompt("find: "); }    /* Ctrl-F: find (keeps the last query) */
        else if (k == 0x87) { goting = 1; findq[0] = 0; render_prompt("goto line: "); }  /* Ctrl-G: go to line */
        else if (k == 0x9a) undo();                       /* Ctrl-Z: undo last edit group */
        else if (k == 0x99) redo();                       /* Ctrl-Y: redo */
        else if (k == 0x82) sel_anchor = (sel_anchor < 0) ? cur : -1;  /* Ctrl-B: set/clear selection mark */
        else if (k == 0x83) do_copy();                    /* Ctrl-C: copy selection (or line) */
        else if (k == 0x98) do_cut();                     /* Ctrl-X: cut selection (or line)  */
        else if (k == 0x96) paste_clip();                 /* Ctrl-V: paste clipboard   */
        else if (k == '\n' || k == '\r') { insert('\n'); undo_break(); }   /* newline ends an undo group */
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
