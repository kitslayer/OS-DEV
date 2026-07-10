/* Host regression test for the editor's UNDO GROUPING (user/editor.c).
 *
 * The bug class (M1756): a single user keystroke that produces several char
 * edits must undo as ONE Ctrl-Z. The (kind,pos)-continuity rule in undo_record
 * only coalesces a *run* of same-kind edits at advancing positions, so a
 * keystroke that mixes kinds (delete then insert) or inserts repeatedly at a
 * FIXED position splits into many groups unless the caller calls
 * undo_merge_last(). Two callers forgot to: block_indent (multi-line Tab) and
 * replace_all (Ctrl-R Replace-All). This test drives the exact undo machinery
 * and asserts one undo() fully reverts each operation.
 *
 * The undo cluster below is COPIED VERBATIM from user/editor.c (undo state +
 * undo_record/undo_break/undo_merge_last/undo/redo + the edit primitives +
 * sel_bounds + block_indent + replace_all). editor.c can't be #included (it has
 * main() + syscalls), so keep this copy in sync if that code changes -- same
 * verbatim-extraction contract as the browser/shell/calc test families. Build
 * with ASan+UBSan; exit 0 = pass. */
#include <stdio.h>
#include <string.h>

/* ---- verbatim from user/editor.c ---------------------------------------- */
#define MAXDOC 262144
static char doc[MAXDOC];
static int  dlen, cur, readonly;
static int  sel_anchor = -1;
static char findq[40];
static char replq[40];
static int  dirty;

#define UNDO_MAX 16384
struct uop { int pos, grp; unsigned char ch, kind; };
static struct uop ulog[UNDO_MAX];
static int un, umax, ugrp, uexpect = -1, ulast_kind = -1;

static void undo_record(int pos, char ch, int kind) {
    if (un >= UNDO_MAX) {
        int keep = UNDO_MAX / 2;
        for (int i = 0; i < keep; i++) ulog[i] = ulog[un - keep + i];
        un = keep;
    }
    if (!(un > 0 && kind == ulast_kind && pos == uexpect)) ugrp++;
    ulog[un].pos = pos; ulog[un].grp = ugrp;
    ulog[un].ch = (unsigned char)ch; ulog[un].kind = (unsigned char)kind;
    un++;
    umax = un;
    ulast_kind = kind;
    uexpect = (kind == 0) ? pos + 1 : (kind == 1) ? pos - 1 : pos;
}
static void undo_break(void) { ulast_kind = -1; uexpect = -1; }
static void undo_merge_last(int n) {
    if (n <= 1 || n > un) return;
    int g = ulog[un - 1].grp;
    for (int i = un - n; i < un - 1; i++) ulog[i].grp = g;
    undo_break();
}
static void undo(void) {
    if (un == 0) return;
    int g = ulog[un - 1].grp;
    while (un > 0 && ulog[un - 1].grp == g) {
        struct uop *o = &ulog[--un];
        if (o->kind == 0) {
            if (o->pos < dlen) {
                for (int i = o->pos; i < dlen - 1; i++) doc[i] = doc[i+1];
                dlen--;
            }
            cur = o->pos;
        } else if (dlen < MAXDOC - 1) {
            for (int i = dlen; i > o->pos; i--) doc[i] = doc[i-1];
            doc[o->pos] = (char)o->ch; dlen++;
            cur = o->pos + 1;
        }
    }
    if (cur > dlen) cur = dlen;
    ulast_kind = -1; uexpect = -1;
    dirty = 1;
}
static void insert(char c) {
    if (readonly || dlen >= MAXDOC - 1) return;
    sel_anchor = -1; dirty = 1;
    undo_record(cur, c, 0);
    for (int i = dlen; i > cur; i--) doc[i] = doc[i-1];
    doc[cur++] = c; dlen++;
}
static void del_fwd(void) {
    if (readonly || cur >= dlen) return;
    sel_anchor = -1; dirty = 1;
    undo_record(cur, doc[cur], 2);
    for (int i = cur; i < dlen - 1; i++) doc[i] = doc[i+1];
    dlen--;
}
static void ins_at(int pos, char ch) {
    if (dlen >= MAXDOC - 1 || pos < 0 || pos > dlen) return;
    undo_record(pos, ch, 0);
    for (int i = dlen; i > pos; i--) doc[i] = doc[i-1];
    doc[pos] = ch; dlen++;
}
static void del_at(int pos) {
    if (pos < 0 || pos >= dlen) return;
    undo_record(pos, doc[pos], 2);
    for (int i = pos; i < dlen - 1; i++) doc[i] = doc[i+1];
    dlen--;
}
static int sel_bounds(int *s0, int *s1) {
    if (sel_anchor < 0) return 0;
    int a = sel_anchor, b = cur;
    if (a > b) { int t = a; a = b; b = t; }
    if (a < 0) a = 0;
    if (b > dlen) b = dlen;
    *s0 = a; *s1 = b;
    return b > a;
}
static void block_indent(int dedent) {
    if (readonly) return;
    int s0, s1;
    if (!sel_bounds(&s0, &s1)) { s0 = s1 = cur; }
    int lo = s0; while (lo > 0 && doc[lo-1] != '\n') lo--;
    int oa = sel_anchor, oc = cur, na = oa, nc = oc;
    int q = s1 - 1; if (q < lo) q = lo;
    int n0 = un;                                                /* M1756 */
    for (;;) {
        int ls = q; while (ls > 0 && doc[ls-1] != '\n') ls--;
        if (!dedent) {
            for (int k = 0; k < 4; k++) ins_at(ls, ' ');
            if (oa > ls) na += 4;
            if (oc >= ls) nc += 4;
        } else {
            int rm = 0;
            if (ls < dlen && doc[ls] == '\t') rm = 1;
            else while (rm < 4 && ls + rm < dlen && doc[ls+rm] == ' ') rm++;
            for (int k = 0; k < rm; k++) del_at(ls);
            if (oa >= ls + rm) na -= rm; else if (oa > ls) na -= (oa - ls);
            if (oc >= ls + rm) nc -= rm; else if (oc > ls) nc -= (oc - ls);
        }
        if (ls <= lo) break;
        q = ls - 1;
    }
    if (sel_anchor >= 0) sel_anchor = na < 0 ? 0 : na;
    cur = nc < 0 ? 0 : (nc > dlen ? dlen : nc);
    dirty = 1;
    undo_merge_last(un - n0);   /* M1756 */
    undo_break();
}
static int replace_all(void) {
    int flen = 0; while (findq[flen]) flen++;
    int rlen = 0; while (replq[rlen]) rlen++;
    if (flen == 0 || readonly) return 0;
    int count = 0, pos = 0, n0 = un;   /* M1756 */
    while (pos <= dlen - flen) {
        int m = 1; for (int i = 0; i < flen; i++) if (doc[pos+i] != findq[i]) { m = 0; break; }
        if (!m) { pos++; continue; }
        if (dlen - flen + rlen > MAXDOC - 1) break;
        cur = pos;
        for (int i = 0; i < flen; i++) del_fwd();
        for (int i = 0; i < rlen; i++) insert(replq[i]);
        pos += rlen;
        count++;
    }
    if (count) { undo_merge_last(un - n0); undo_break(); }   /* M1756 */
    return count;
}
/* ---- end verbatim ------------------------------------------------------- */

static int fails = 0;
#define CK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } } while (0)

static void reset(const char *s) {
    dlen = (int)strlen(s); memcpy(doc, s, dlen); cur = 0; sel_anchor = -1;
    un = umax = ugrp = 0; uexpect = -1; ulast_kind = -1; readonly = 0; dirty = 0;
}
static int doc_is(const char *s) {
    int n = (int)strlen(s);
    return dlen == n && memcmp(doc, s, n) == 0;
}
/* how many undo groups sit in the current log (distinct grp ids in [0,un)) */
static int group_count(void) {
    int g = 0, last = -999;
    for (int i = 0; i < un; i++) if (ulog[i].grp != last) { g++; last = ulog[i].grp; }
    return g;
}

int main(void) {
    /* ---- Finding 1: multi-line Tab indent undoes in ONE Ctrl-Z ---- */
    reset("aaa\nbbb\nccc");
    sel_anchor = 0; cur = 7;                 /* select lines 1-2 (offsets 0..7 spans "aaa\nbbb\n") */
    block_indent(0);
    CK(doc_is("    aaa\n    bbb\nccc"), "indent: both selected lines gain 4 spaces");
    CK(group_count() == 1, "indent: the whole multi-line indent is ONE undo group");
    undo();
    CK(doc_is("aaa\nbbb\nccc"), "indent: a single undo() fully reverts the block indent");

    /* dedent (Shift-Tab) of a multi-line selection also undoes in one */
    reset("    aaa\n    bbb\nccc");
    sel_anchor = 0; cur = 11;                /* covers the two indented lines */
    block_indent(1);
    CK(doc_is("aaa\nbbb\nccc"), "dedent: 4 leading spaces removed from both lines");
    CK(group_count() == 1, "dedent: the whole multi-line dedent is ONE undo group");
    undo();
    CK(doc_is("    aaa\n    bbb\nccc"), "dedent: a single undo() fully reverts");

    /* single-line indent (no selection) also merges to one group */
    reset("hello");
    cur = 2;
    block_indent(0);
    CK(doc_is("    hello"), "single-line indent adds 4 spaces");
    undo();
    CK(doc_is("hello"), "single-line indent undoes in one");

    /* ---- Finding 2: Replace-All undoes in ONE Ctrl-Z, never a blank ---- */
    reset("axbxcx");
    strcpy(findq, "x"); strcpy(replq, "YY");
    int n = replace_all();
    CK(n == 3, "replace-all replaced all 3 matches");
    CK(doc_is("aYYbYYcYY"), "replace-all produced the replaced text");
    CK(group_count() == 1, "replace-all is ONE undo group");
    undo();
    CK(doc_is("axbxcx"), "a single undo() fully reverts replace-all (no blank, no residue)");

    /* replacement CONTAINING the search term must not loop or double-undo */
    reset("aaa");
    strcpy(findq, "a"); strcpy(replq, "ba");
    n = replace_all();
    CK(n == 3 && doc_is("bababa"), "replace 'a'->'ba' hits each original 'a' once");
    undo();
    CK(doc_is("aaa"), "and undoes in one step");

    /* delete-all (empty replacement) undoes in one */
    reset("a,b,c,");
    strcpy(findq, ","); replq[0] = 0;
    n = replace_all();
    CK(n == 3 && doc_is("abc"), "replace ','->'' deletes all commas");
    undo();
    CK(doc_is("a,b,c,"), "delete-all undoes in one step");

    if (fails) { fprintf(stderr, "FAIL: %d editor-undo check(s) failed\n", fails); return 1; }
    printf("PASS: editor undo grouping (multi-line indent/dedent + replace-all atomic, ASan/UBSan clean)\n");
    return 0;
}
