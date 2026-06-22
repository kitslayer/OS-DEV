/*
 * editor.c — a small full-screen text editor (a 5th userspace program).
 *
 * Brings together the input pieces: it reads keys one at a time without
 * blocking (sys_pollkey), moves a cursor with the arrow keys, edits a buffer,
 * and saves to the FAT32 disk. The cursor is shown inline as a '|'. The view
 * scrolls to keep the cursor visible (centred) in long files.
 */
#include "ulib.h"

#define MAXDOC 262144             /* editable file size 256 KB (each editor process has its own copy); large downloaded/saved web pages exceed 64 KB */
#define EDCOLS 44                 /* must match the app text grid */
#define GUTTER 5                  /* left line-number gutter: 4 digits + 1 pad (good to 9999; wider numbers eat the pad) */
#define EDTEXT (EDCOLS - GUTTER)  /* usable text columns to the right of the gutter (the wrap width) */
#define EDVIS  16                 /* visible text rows (grid is 17; 1 is the status line) */

static char doc[MAXDOC];
static int  dlen, cur, readonly;  /* readonly: file exceeded the buffer — view only, never save (would truncate it) */
static int  sel_anchor = -1;      /* selection mark (Ctrl-B); -1 = none. Selection spans [min, max](anchor,cur) */
static char fname[40];
static char findq[40]; static int finding, goting;   /* Ctrl-F find / Ctrl-G go-to-line: shared query buffer + mode flags */
static char replq[40]; static int replacing;          /* Ctrl-R replace: replacement text + mode (1=typing search, 2=typing replacement) */
static int helping;                                   /* Ctrl-H: key-list overlay shown */
static int saving_as;                                 /* Ctrl-W: typing a new filename to save under */
static int opening;                                   /* Ctrl-O: typing a filename to open in place */
static int dirty;                                     /* buffer modified since the last save (shown as * in the status) */

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

/* Re-label the most recent `n` undo ops as a single group, so one Ctrl-Z (or
 * one Backspace's worth of undo) reverts them together. Used when a single
 * keystroke produces a mixed run of edits the (kind,pos)-continuity rule would
 * otherwise split — e.g. auto-dedent's leading-space deletes + the `}` insert. */
static void undo_merge_last(int n) {
    if (n <= 1 || n > un) return;
    int g = ulog[un - 1].grp;
    for (int i = un - n; i < un - 1; i++) ulog[i].grp = g;
    undo_break();                               /* the merged run is itself a group boundary */
}

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
    dirty = 1;
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
    dirty = 1;
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
    sel_anchor = -1; dirty = 1;
    undo_record(cur, c, 0);
    for (int i = dlen; i > cur; i--) doc[i] = doc[i-1];
    doc[cur++] = c; dlen++;
}
static void backspace(void) {
    if (readonly || cur == 0) return;
    sel_anchor = -1; dirty = 1;
    undo_record(cur - 1, doc[cur - 1], 1);
    for (int i = cur - 1; i < dlen - 1; i++) doc[i] = doc[i+1];
    dlen--; cur--;
}
static void del_fwd(void) {                 /* Delete key: remove the char at the cursor */
    if (readonly || cur >= dlen) return;
    sel_anchor = -1; dirty = 1;
    undo_record(cur, doc[cur], 2);
    for (int i = cur; i < dlen - 1; i++) doc[i] = doc[i+1];
    dlen--;
}

/* Enter with auto-indent: insert a newline, then reproduce the current line's
 * leading whitespace on the new line (so code keeps its indentation), plus one
 * extra 4-space level when the line ended with an open bracket {([. All the
 * inserts coalesce into one undo group. */
static void newline_indent(void) {
    char ind[64]; int ni = 0;
    int ls = cur; while (ls > 0 && doc[ls-1] != '\n') ls--;          /* start of the current line */
    while (ls + ni < cur && ni < 63 && (doc[ls+ni] == ' ' || doc[ls+ni] == '\t')) { ind[ni] = doc[ls+ni]; ni++; }
    int extra = 0;                                                   /* indent one more level after a trailing {([ */
    for (int j = cur - 1; j >= ls; j--) { char c = doc[j]; if (c == ' ' || c == '\t') continue; if (c == '{' || c == '(' || c == '[') extra = 4; break; }
    insert('\n');
    for (int i = 0; i < ni; i++) insert(ind[i]);
    for (int i = 0; i < extra; i++) insert(' ');
}

static int sel_bounds(int *s0, int *s1);   /* fwd: defined below, used by block_indent */

/* Raw insert/delete at an arbitrary position (undoable). Unlike insert()/del_fwd()
 * they leave cur and sel_anchor for the caller to fix up — used by block_indent. */
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

/* Auto-dedent: complement to newline_indent(). When the user interactively types
 * a closing `}` and everything on the current line before the caret is whitespace,
 * pull the line back one indent level (a leading tab, else up to 4 spaces removed
 * immediately left of the caret, never past the line start) before inserting the
 * `}` — so a `}` that opened a block ends up under its `if`/`for`/etc. rather than
 * at the inner body indent. The deletes + the insert coalesce into one undo group,
 * so a single Backspace/Ctrl-Z reverts the whole keystroke. Only `}` is handled
 * (`)`/`]` close inline far more often than they sit alone on a line, where a
 * dedent would just be noise). Returns 1 if it inserted the `}` (caller is done). */
static int dedent_brace(void) {
    if (readonly || dlen >= MAXDOC - 1) return 0;
    int ls = cur; while (ls > 0 && doc[ls-1] != '\n') ls--;     /* start of the current line */
    if (ls == cur) return 0;                                    /* caret at line start: nothing to dedent */
    for (int i = ls; i < cur; i++) if (doc[i] != ' ' && doc[i] != '\t') return 0;  /* prefix must be whitespace-only */
    int rm = 0;                                                 /* one level: a leading tab, else up to 4 spaces */
    if (doc[cur-1] == '\t') rm = 1;
    else while (rm < 4 && cur - 1 - rm >= ls && doc[cur-1-rm] == ' ') rm++;
    int n0 = un;                                                /* undo ops before this keystroke */
    for (int k = 0; k < rm; k++) { del_at(cur - 1); cur--; }    /* drop the whitespace just left of the caret */
    insert('}');                                                /* then insert the brace as usual */
    undo_merge_last(un - n0);                                   /* deletes + insert = one undo step */
    return 1;
}
/* Tab / Shift-Tab block indent (dedent=0) or dedent (dedent=1): operate on every
 * line touched by the selection, or just the current line when there's no
 * selection. Indent adds 4 spaces at each line start; dedent removes a leading
 * tab or up to 4 leading spaces. cur + the selection are kept over the same text. */
static void block_indent(int dedent) {
    if (readonly) return;
    int s0, s1;
    if (!sel_bounds(&s0, &s1)) { s0 = s1 = cur; }
    int lo = s0; while (lo > 0 && doc[lo-1] != '\n') lo--;       /* start of the first line */
    int oa = sel_anchor, oc = cur, na = oa, nc = oc;
    /* Walk each touched line's start from the LAST in range back to `lo`. Going
     * backward means a mutation at a higher offset never invalidates a lower start
     * still to come — so no fixed per-line array is needed (was capped at 1024
     * lines, silently skipping the rest of a larger selection). */
    int q = s1 - 1; if (q < lo) q = lo;                          /* last line start that is < s1 (or just `lo` when there's no selection) */
    for (;;) {
        int ls = q; while (ls > 0 && doc[ls-1] != '\n') ls--;   /* start of the line containing q (an original offset: all edits so far were above it) */
        if (!dedent) {
            for (int k = 0; k < 4; k++) ins_at(ls, ' ');        /* indent: 4 spaces at the line start */
            if (oa > ls) na += 4;                               /* anchor exactly at a line start stays put, so the selection covers the new indent */
            if (oc >= ls) nc += 4;
        } else {
            int rm = 0;                                         /* dedent: remove a leading tab, else up to 4 leading spaces */
            if (ls < dlen && doc[ls] == '\t') rm = 1;
            else while (rm < 4 && ls + rm < dlen && doc[ls+rm] == ' ') rm++;
            for (int k = 0; k < rm; k++) del_at(ls);
            if (oa >= ls + rm) na -= rm; else if (oa > ls) na -= (oa - ls);
            if (oc >= ls + rm) nc -= rm; else if (oc > ls) nc -= (oc - ls);
        }
        if (ls <= lo) break;                                    /* just processed the first line */
        q = ls - 1;                                             /* step into the previous line */
    }
    if (sel_anchor >= 0) sel_anchor = na < 0 ? 0 : na;
    cur = nc < 0 ? 0 : (nc > dlen ? dlen : nc);
    dirty = 1;
    undo_break();
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

/* Wrap-aware grid-row of doc position `pos` (a '\n' or hitting EDTEXT wraps).
 * EDTEXT (not EDCOLS) is the wrap width because the gutter eats the left columns;
 * this must agree with the render loop's wrap, or the scroll window mis-tracks. */
static int row_of(int pos) {
    int row = 0, col = 0;
    for (int i = 0; i < pos && i < dlen; i++) {
        if (doc[i] == '\n') { row++; col = 0; }
        else if (++col == EDTEXT) { row++; col = 0; }
    }
    return row;
}
/* Doc offset where grid-row `target` begins (or dlen if past the end). */
static int row_offset(int target) {
    if (target <= 0) return 0;
    int row = 0, col = 0;
    for (int i = 0; i < dlen; i++) {
        if (doc[i] == '\n') { row++; col = 0; }
        else if (++col == EDTEXT) { row++; col = 0; }
        if (row == target) return i + 1;
    }
    return dlen;
}

/* ---- syntax highlighting -------------------------------------------------
 * Picks a language from the filename extension and colours the visible window
 * of `doc` one byte at a time. Palette indices (see kernel app_palette):
 *   0 default (green) · 6 keyword (blue) · 7 string (orange) · 8 comment (grey)
 *   10 C preprocessor (teal) · 11 number (purple).  hl_lang 0 = plain (no
 *   colour), 1 = C, 2 = shell, 3 = HTML, 4 = JS, 5 = CSS. */
static int hl_lang;                  /* set by detect_lang() in load_file() */
static unsigned char vcol[2048];     /* colour per byte of the visible window */

static int hl_isids(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int hl_isid (char c){ return hl_isids(c)||(c>='0'&&c<='9'); }
static int hl_isdig(char c){ return c>='0'&&c<='9'; }
static int hl_eqi(const char*a,const char*b){
    while(*a&&*b){ char x=*a,y=*b; if(x>='a'&&x<='z')x-=32; if(y>='a'&&y<='z')y-=32; if(x!=y)return 0; a++;b++; }
    return *a==0&&*b==0;
}
/* True if doc[i] is the first non-blank char on its line (for C `#` directives). */
static int hl_linestart(int i){ int j=i; while(j>0&&(doc[j-1]==' '||doc[j-1]=='\t'))j--; return j==0||doc[j-1]=='\n'; }
/* True if doc[i] is at a word boundary (for a shell `#` comment). */
static int hl_prews(int i){ return i==0||doc[i-1]==' '||doc[i-1]=='\t'||doc[i-1]=='\n'; }

static const char *hl_kw(void){
    if(hl_lang==1) return " auto break case char const continue default do double else enum extern float for goto if inline int long register return short signed sizeof static struct switch typedef union unsigned void volatile while ";
    if(hl_lang==4) return " async await break case catch class const continue debugger default delete do else export extends false finally for function if import in instanceof let new null of return static super switch this throw true try typeof undefined var void while yield ";
    if(hl_lang==2) return " if then else elif fi for while until do done case esac in function return break continue local select time ";
    return " ";
}
static int hl_iskw(const char*w,int wl){
    const char*L=hl_kw(); int i=0;
    while(L[i]){ while(L[i]==' ')i++; if(!L[i])break; int s=i; while(L[i]&&L[i]!=' ')i++;
        if(i-s==wl){ int m=1; for(int j=0;j<wl;j++) if(L[s+j]!=w[j]){m=0;break;} if(m)return 1; } }
    return 0;
}

/* Tokenise doc[start,end). When write, store each byte's colour into vcol[idx-start].
 * Carries multi-line state (block/HTML comment, HTML tag, string) via the mode/delim
 * so a [0,off) pass seeds the state for the visible-window pass. Modes: 0 normal,
 * 1 line comment, 2 block comment, 3 string, 4 HTML tag, 5 HTML comment,
 * 6 HTML attribute-value string. */
static void hl_run(int start,int end,int write,int *pmode,char *pdelim){
    int mode=*pmode, lang=hl_lang; char delim=*pdelim; int i=start;
    #define PUT(idx,cc) do{ if(write){ int _x=(idx)-start; if(_x>=0&&_x<2048) vcol[_x]=(unsigned char)(cc); } }while(0)
    while(i<end){
        char c=doc[i];
        if(mode==1){ PUT(i,8); if(c=='\n') mode=0; i++; continue; }
        if(mode==2){ PUT(i,8); if(c=='*'&&i+1<end&&doc[i+1]=='/'){ PUT(i+1,8); i+=2; mode=0; continue; } i++; continue; }
        if(mode==5){ PUT(i,8); if(c=='-'&&i+2<end&&doc[i+1]=='-'&&doc[i+2]=='>'){ PUT(i+1,8); PUT(i+2,8); i+=3; mode=0; continue; } i++; continue; }
        if(mode==3){ PUT(i,7);
            if(c=='\\'&&i+1<end){ PUT(i+1,7); i+=2; continue; }
            if(c==delim){ mode=0; i++; continue; }
            if(c=='\n'&&delim!='`'){ mode=0; i++; continue; }   /* non-template string ends at EOL */
            i++; continue; }
        if(mode==6){ PUT(i,7); if(c==delim) mode=4; i++; continue; }   /* HTML attribute value string */
        if(mode==4){
            if(c=='"'||c=='\''){ delim=c; mode=6; PUT(i,7); i++; continue; }
            PUT(i,6); if(c=='>') mode=0; i++; continue; }
        /* mode 0: normal */
        if(lang==3){
            if(c=='<'){ if(i+3<end&&doc[i+1]=='!'&&doc[i+2]=='-'&&doc[i+3]=='-'){ PUT(i,8); mode=5; i++; continue; } PUT(i,6); mode=4; i++; continue; }
            PUT(i,0); i++; continue; }
        if((lang==1||lang==4)&&c=='/'&&i+1<end&&doc[i+1]=='/'){ PUT(i,8); PUT(i+1,8); i+=2; mode=1; continue; }
        if((lang==1||lang==4||lang==5)&&c=='/'&&i+1<end&&doc[i+1]=='*'){ PUT(i,8); PUT(i+1,8); i+=2; mode=2; continue; }   /* block comment (C/JS/CSS) */
        if(lang==5&&c=='#'){ PUT(i,11); i++; while(i<end&&hl_isid(doc[i])){ PUT(i,11); i++; } continue; }   /* CSS #hex colour */
        if(lang==2&&c=='#'&&hl_prews(i)){ mode=1; continue; }
        if(lang==1&&c=='#'&&hl_linestart(i)){ while(i<end&&doc[i]!='\n'){ PUT(i,10); i++; } continue; }
        if(c=='"'||c=='\''||((lang==1||lang==4)&&c=='`')){ delim=c; mode=3; PUT(i,7); i++; continue; }
        if(hl_isdig(c)&&!(i>0&&hl_isid(doc[i-1]))){ while(i<end&&(hl_isid(doc[i])||doc[i]=='.')){ PUT(i,11); i++; } continue; }
        if(hl_isids(c)){ int s=i; while(i<end&&hl_isid(doc[i])) i++; int cc=hl_iskw(doc+s,i-s)?6:0; for(int k=s;k<i;k++) PUT(k,cc); continue; }
        PUT(i,0); i++;
    }
    *pmode=mode; *pdelim=delim;
    #undef PUT
}

/* Choose the highlighter language from fname's extension (FAT 8.3 -> uppercase). */
static void detect_lang(void){
    int n=0; while(fname[n]) n++;
    int d=-1; for(int i=0;i<n;i++) if(fname[i]=='.') d=i;
    hl_lang=0;
    if(d<0) return;
    const char *e=fname+d+1;
    if(hl_eqi(e,"c")||hl_eqi(e,"h")) hl_lang=1;
    else if(hl_eqi(e,"js")) hl_lang=4;
    else if(hl_eqi(e,"sh")) hl_lang=2;
    else if(hl_eqi(e,"htm")||hl_eqi(e,"html")) hl_lang=3;
    else if(hl_eqi(e,"css")) hl_lang=5;
}

static int is_bracket(char c){ return c=='('||c==')'||c=='['||c==']'||c=='{'||c=='}'; }
/* Position of the bracket matching the one at `pos`, or -1. Naive depth scan
 * (doesn't skip brackets in strings/comments -- good enough for an editor aid). */
static int match_bracket(int pos){
    char c = doc[pos];
    const char *opn = "([{", *cls = ")]}";
    int dir = 0; char want = 0;
    for (int i = 0; i < 3; i++){ if (c==opn[i]){ dir=1; want=cls[i]; break; } if (c==cls[i]){ dir=-1; want=opn[i]; break; } }
    if (!dir) return -1;
    int depth = 0;
    for (int i = pos; i >= 0 && i < dlen; i += dir){
        if (doc[i]==c) depth++;
        else if (doc[i]==want && --depth==0) return i;
    }
    return -1;
}

/* Left line-number gutter: append GUTTER cells to out[]/hlc[]. `lineno` > 0 is
 * shown right-aligned (with a 1-col pad), `lineno` <= 0 leaves the field blank
 * (a wrap-continuation row). The cells carry the GUT sentinel colour so the
 * run-printer always renders them grey and a selection never tints them. The
 * gutter is drawn by the editor itself, inline in its print() stream — there is
 * no absolute positioning here, so the cursor '|' and selection markers, which
 * are also emitted into this same stream, stay aligned automatically. */
#define GUT_COL ((signed char)-2)   /* hlc[] sentinel: gutter cell -> always palette 8 (grey), never selected */
static void emit_gutter(char *out, signed char *hlc, int *po, int lineno) {
    char num[12]; int nl = 0;
    if (lineno > 0) itoa_i(lineno, num); else num[0] = 0;
    nl = 0; while (num[nl]) nl++;
    int o = *po;
    int pad = GUTTER - 1 - nl;          /* GUTTER-1 digit columns (1 col is the trailing pad) */
    for (int i = 0; i < GUTTER - 1; i++) {            /* right-align the digits in the leading field */
        int di = i - pad;                             /* index into num once we reach the number */
        char ch = (lineno > 0 && di >= 0 && di < nl) ? num[di] : ' ';
        out[o] = ch; hlc[o] = GUT_COL; o++;
    }
    out[o] = ' '; hlc[o] = GUT_COL; o++;              /* 1-column pad between gutter and text */
    *po = o;
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
    if (dirty) st[p++] = '*';                  /* unsaved-changes indicator */
    for (int i = 0; fname[i] && p < 31; i++) st[p++] = fname[i];
    a = readonly ? "  ESC=quit [RO: file too big]  " : "  ESC/^S=save ^Q=quit ^H=help  "; while (*a) st[p++] = *a++;
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
    /* Syntax highlighting: colour the visible window of `doc` into vcol[], seeded
     * by a scan of [0,off) so a block comment / HTML tag / string opened earlier
     * colours correctly. hl_lang==0 -> all default (plain text renders as before). */
    int vn = dlen - off; if (vn > 2046) vn = 2046; if (vn < 0) vn = 0;
    for (int z = 0; z < vn; z++) vcol[z] = 0;
    if (hl_lang) {
        int mode = 0; char delim = 0;
        hl_run(0, off, 0, &mode, &delim);              /* seed: tokeniser state at `off` */
        hl_run(off, off + vn, 1, &mode, &delim);       /* colour the visible window */
    }
    /* Matching-bracket highlight: if the caret sits on (or just after) a bracket,
     * tint it and its partner pink (5). Works in any file, plain text included. */
    {
        int bp = -1;
        if (cur < dlen && is_bracket(doc[cur])) bp = cur;
        else if (cur > 0 && is_bracket(doc[cur-1])) bp = cur - 1;
        if (bp >= 0) {
            int mp = match_bracket(bp);
            if (bp - off >= 0 && bp - off < vn) vcol[bp - off] = 5;
            if (mp >= 0 && mp - off >= 0 && mp - off < vn) vcol[mp - off] = 5;
        }
    }
    static char out[2048];                 /* off the stack; only ever holds the EDVIS visible rows */
    static signed char hlc[2048];          /* parallel colour for each out[] byte */
    int o = 0, row = 0, col = 0, os0 = -1, os1 = -1;
    /* Line number of the first visible row, and whether that row starts a fresh
     * document line (number shown) or is a wrap-continuation (blank gutter). */
    int vline = 1; for (int i = 0; i < off && i < dlen; i++) if (doc[i] == '\n') vline++;
    int line_start = (off == 0 || (off <= dlen && doc[off-1] == '\n'));
    emit_gutter(out, hlc, &o, line_start ? vline : 0);   /* gutter for the first visible row */
    for (int i = off; i <= dlen && row < EDVIS && o < (int)sizeof(out) - GUTTER - 4; i++) {
        if (sel && i == s0) os0 = o;       /* record where the selection starts/ends in the output... */
        if (sel && i == s1) os1 = o;       /* ...before the cursor mark so the caret isn't highlighted */
        if (i == cur) { out[o] = '|'; hlc[o] = 0; o++; }   /* caret sits just after the gutter at col 0 */
        if (i < dlen) {
            char ch = doc[i];
            int cc = (i - off >= 0 && i - off < vn) ? vcol[i - off] : 0;
            out[o] = ch; hlc[o] = (signed char)cc; o++;
            int wrapped = 0;
            if (ch == '\n') { row++; col = 0; vline++; line_start = 1; }
            else if (++col == EDTEXT) { row++; col = 0; line_start = 0; wrapped = 1; }
            if ((ch == '\n' || wrapped) && row < EDVIS)   /* start the next row with its gutter */
                emit_gutter(out, hlc, &o, line_start ? vline : 0);
        }
    }
    out[o] = 0;
    if (sel) {                             /* selection may run off the top/bottom of the window */
        if (os0 < 0 && os1 >= 0) os0 = 0;
        if (os0 >= 0 && os1 < 0) os1 = o;
    }
    /* Print out[] in maximal same-colour runs; an active selection overrides to
     * yellow (3) within [os0,os1). Gutter cells (GUT_COL) are forced grey (8) and
     * never tinted by a selection, so the numbers stay legible. With hl_lang==0
     * every text byte is colour 0, so this reduces to the previous default-text /
     * yellow-selection rendering with a grey gutter prefixed to each row. */
    int k = 0;
    while (k < o) {
        int c = hlc[k] == GUT_COL ? 8 : hlc[k];
        if (hlc[k] != GUT_COL && sel && k >= os0 && k < os1) c = 3;
        int e = k + 1;
        while (e < o) {
            int ce = hlc[e] == GUT_COL ? 8 : hlc[e];
            if (hlc[e] != GUT_COL && sel && e >= os0 && e < os1) ce = 3;
            if (ce != c) break;
            e++;
        }
        sys_setcolor(c);
        char sv = out[e]; out[e] = 0; print(out + k); out[e] = sv;
        k = e;
    }
    sys_setcolor(0);
    if (msg) print(msg);
}

/* Replace every occurrence of findq with replq (Ctrl-R). Each edit goes through
 * del_fwd/insert so it's undoable; returns the number replaced. */
static int replace_all(void) {
    int flen = 0; while (findq[flen]) flen++;
    int rlen = 0; while (replq[rlen]) rlen++;
    if (flen == 0 || readonly) return 0;
    int count = 0, pos = 0;
    while (pos <= dlen - flen) {
        int m = 1; for (int i = 0; i < flen; i++) if (doc[pos+i] != findq[i]) { m = 0; break; }
        if (!m) { pos++; continue; }
        if (dlen - flen + rlen > MAXDOC - 1) break;   /* this replacement wouldn't fit -> stop cleanly, never a partial (match deleted but replacement truncated at the cap) */
        cur = pos;
        for (int i = 0; i < flen; i++) del_fwd();
        for (int i = 0; i < rlen; i++) insert(replq[i]);
        undo_break();
        pos += rlen;                 /* skip the inserted text so replq containing findq can't loop */
        count++;
    }
    return count;
}

/* Full-screen key reference (Ctrl-H toggles it; any key dismisses). */
static void render_help(void) {
    sys_clear();
    sys_setcolor(4); print("EDITOR KEYS  (any key returns)\n"); sys_setcolor(0);
    print("arrows Home End PgUp PgDn   move\n");
    print("^S save  ^W save as  ^O open  ^Q quit  ESC save+quit\n");
    print("^Z undo    ^Y redo\n");
    print("^F find    ^R replace  ^G go to line\n");
    print("^B set mark, then move to select   ^A select all\n");
    print("^C copy    ^X cut      ^V paste\n");
    print("Tab indent (selection)  Shift-Tab dedent   Del fwd-delete\n");
}

/* Show the doc with a "<label><query>_" prompt at the bottom (Ctrl-F/G/R). */
static void render_prompt(const char *label, const char *query) {
    char m[80]; int p = 0; m[p++] = '\n';
    for (const char *a = label; *a && p < 50; a++) m[p++] = *a;
    for (int i = 0; query[i] && p < 76; i++) m[p++] = query[i];
    m[p++] = '_'; m[p] = 0;
    render(m);
}

/* Load `fname` into the buffer and reset per-file state (undo history, mark). */
static void load_file(void) {
    long n = sys_readfile(fname, doc, MAXDOC - 1);
    dlen = (n > 0) ? (int)n : 0;
    readonly = (n >= MAXDOC - 1);    /* read filled the buffer: the file is larger -> view only */
    cur = dlen;
    un = umax = 0; ulast_kind = -1; uexpect = -1;   /* undo history is per-file */
    sel_anchor = -1; dirty = 0;
    detect_lang();                   /* choose syntax highlighting from the extension */
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
    load_file();

    render(0);
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (helping) { helping = 0; render(0); continue; }   /* any key dismisses the help overlay */
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
            else if (k == 8 || k == 127) { if (fl > 0) findq[fl-1] = 0; render_prompt("find: ", findq); }
            else if (k >= 32 && k < 127 && fl < 39) { findq[fl] = (char)k; findq[fl+1] = 0; render_prompt("find: ", findq); }
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
            else if (k == 8 || k == 127) { if (fl > 0) findq[fl-1] = 0; render_prompt("goto line: ", findq); }
            else if (k >= '0' && k <= '9' && fl < 8) { findq[fl] = (char)k; findq[fl+1] = 0; render_prompt("goto line: ", findq); }
            continue;
        }
        if (replacing) {                            /* Ctrl-R: phase 1 type the search, phase 2 the replacement */
            char *q = (replacing == 1) ? findq : replq;
            const char *lbl = (replacing == 1) ? "replace: " : "with: ";
            int fl = 0; while (q[fl]) fl++;
            if (k == '\n' || k == '\r') {
                if (replacing == 1) { replacing = 2; replq[0] = 0; render_prompt("with: ", replq); }
                else {
                    replacing = 0;
                    int c = replace_all();
                    char m[40]; int p = 0; m[p++] = '\n'; m[p++] = '[';
                    char nb[12]; itoa_i(c, nb); for (int i = 0; nb[i]; i++) m[p++] = nb[i];
                    const char *s = " replaced]"; for (int i = 0; s[i]; i++) m[p++] = s[i]; m[p] = 0;
                    render(m);
                }
            }
            else if (k == 27) { replacing = 0; render(0); }          /* Esc: cancel */
            else if (k == 8 || k == 127) { if (fl > 0) q[fl-1] = 0; render_prompt(lbl, q); }
            else if (k >= 32 && k < 127 && fl < 39) { q[fl] = (char)k; q[fl+1] = 0; render_prompt(lbl, q); }
            continue;
        }
        if (saving_as) {                            /* Ctrl-W: type a new filename, Enter saves to it */
            int fl = 0; while (findq[fl]) fl++;
            if (k == '\n' || k == '\r') {
                saving_as = 0;
                if (findq[0]) {
                    int i = 0; for (; findq[i] && i < (int)sizeof(fname)-1; i++) fname[i] = findq[i]; fname[i] = 0;
                    detect_lang();                   /* "save as foo.css" -> re-pick syntax highlighting for the new extension */
                    if (sys_writefile(fname, doc, (unsigned long)dlen) < 0) render("\n[save failed]");
                    else { render("\n[saved]"); dirty = 0; }
                    sys_sleep(400); render(0);
                } else render(0);
            }
            else if (k == 27) { saving_as = 0; render(0); }
            else if (k == 8 || k == 127) { if (fl > 0) findq[fl-1] = 0; render_prompt("save as: ", findq); }
            else if (k >= 32 && k < 127 && fl < 39) { findq[fl] = (char)k; findq[fl+1] = 0; render_prompt("save as: ", findq); }
            continue;
        }
        if (opening) {                              /* Ctrl-O: type a filename, Enter saves current then loads it */
            int fl = 0; while (findq[fl]) fl++;
            if (k == '\n' || k == '\r') {
                opening = 0;
                if (findq[0]) {
                    if (!readonly && sys_writefile(fname, doc, (unsigned long)dlen) < 0) {   /* save current first; if it FAILS (disk full / unwritable), abort — don't clobber the unsaved buffer by loading over it */
                        render("\n[save failed - open cancelled]"); sys_sleep(400);
                    } else {
                        int i = 0; for (; findq[i] && i < (int)sizeof(fname)-1; i++) fname[i] = findq[i]; fname[i] = 0;
                        load_file();
                    }
                }
                render(0);
            }
            else if (k == 27) { opening = 0; render(0); }
            else if (k == 8 || k == 127) { if (fl > 0) findq[fl-1] = 0; render_prompt("open: ", findq); }
            else if (k >= 32 && k < 127 && fl < 39) { findq[fl] = (char)k; findq[fl+1] = 0; render_prompt("open: ", findq); }
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
            else { render("\n[saved]"); dirty = 0; }
            sys_sleep(300); render(0);
        }
        else if (k == 0x91) {                       /* Ctrl-Q: quit WITHOUT saving */
            render("\n[quit - changes not saved]"); sys_sleep(350); return 0;
        }
        else if (k == 0x86) { finding = 1; render_prompt("find: ", findq); }    /* Ctrl-F: find (keeps the last query) */
        else if (k == 0x87) { goting = 1; findq[0] = 0; render_prompt("goto line: ", findq); }  /* Ctrl-G: go to line */
        else if (k == 0x88) { helping = 1; render_help(); continue; }   /* Ctrl-H: key-list overlay (skip the trailing render) */
        else if (k == 0x92) { replacing = 1; findq[0] = 0; render_prompt("replace: ", findq); } /* Ctrl-R: find & replace */
        else if (k == 0x97 && !readonly) { saving_as = 1; findq[0] = 0; render_prompt("save as: ", findq); } /* Ctrl-W: save as */
        else if (k == 0x8f) { opening = 1; findq[0] = 0; render_prompt("open: ", findq); }   /* Ctrl-O: open another file */
        else if (k == 0x9a) undo();                       /* Ctrl-Z: undo last edit group */
        else if (k == 0x99) redo();                       /* Ctrl-Y: redo */
        else if (k == 0x82) sel_anchor = (sel_anchor < 0) ? cur : -1;  /* Ctrl-B: set/clear selection mark */
        else if (k == 0x81) { sel_anchor = 0; cur = dlen; }            /* Ctrl-A: select all */
        else if (k == 0x83) do_copy();                    /* Ctrl-C: copy selection (or line) */
        else if (k == 0x98) do_cut();                     /* Ctrl-X: cut selection (or line)  */
        else if (k == 0x96) paste_clip();                 /* Ctrl-V: paste clipboard   */
        else if (k == '\n' || k == '\r') { newline_indent(); undo_break(); }   /* newline (auto-indented) ends an undo group */
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
        else if (k == '\t') {                             /* Tab */
            int t0, t1, multi = 0;                        /* multi-line selection -> block-indent it */
            if (sel_bounds(&t0, &t1)) for (int i = t0; i < t1; i++) if (doc[i] == '\n') { multi = 1; break; }
            if (multi) block_indent(0);
            else {                                        /* else: spaces to the next 4-col stop */
                int ls = cur; while (ls > 0 && doc[ls-1] != '\n') ls--;
                for (int sp = 4 - ((cur - ls) % 4); sp > 0; sp--) insert(' ');
            }
        }
        else if (k == 0x9b) block_indent(1);              /* Shift-Tab: dedent line / selection */
        else if (k == '}') { if (!dedent_brace()) insert('}'); }  /* `}` on a blank-prefix line auto-dedents one level first */
        else if (k >= 32 && k < 127) insert((char)k);
        render(0);
    }
}
