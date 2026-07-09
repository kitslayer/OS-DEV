/*
 * sheet.c — a spreadsheet, a userspace program (M1696).
 *
 * The OS had a calculator (calc), but no grid-with-formulas spreadsheet — the
 * classic "makes a desktop feel real" application. This is a from-scratch one,
 * modelled on the workflow of the Unix `sc`/`sc-im` calculators (modal, fully
 * keyboard-driven) and Excel/VisiCalc (a cell grid with live-recalculated
 * formulas). Ring-3, like the games; built with SSE for the IEEE-754 formula
 * evaluator. The whole computational core lives in user/sheeteval.h (cell model,
 * evaluator, recalc, formatting) so it can be host-unit-tested by tests/sheet,
 * exactly like calc's evaluator is split into calceval.h + tests/calc. This file
 * is just the terminal UI, the file load/save, and the keyboard loop.
 *
 * A cell holds raw text. '=' prefix -> FORMULA; a pure number -> NUMBER; else a
 * TEXT label. The sheet recalculates after every edit (circular refs -> #CIRC).
 *
 *   Formulas: = expr        e.g.  =B2+C2   =(A1+A2)/2   =SUM(B2:B5)*1.1
 *   Operators: + - * / % ^ (power, right-assoc), unary -, parentheses.
 *   Cell refs: A1 .. Z100 (case-insensitive).  Ranges: A1:B10 (inside a fn).
 *   Functions: SUM AVERAGE(=AVG) MIN MAX COUNT COUNTA PRODUCT
 *              SQRT ABS INT FLOOR CEIL(=CEILING) ROUND(x[,dp]) MOD POW(=POWER)
 *   Constants: PI E.  A referenced empty/text cell counts as 0 in arithmetic.
 *
 * Keys — Normal mode (default):
 *   arrows      move the selection
 *   type a char begin editing the cell (seeded with that character)
 *   Enter       begin editing the cell, preloaded with its current contents
 *   Backspace   clear the current cell
 *   colon       command line (see below)
 * Edit mode:  type to append; Backspace deletes; Enter commits + moves down;
 *   an arrow commits + moves that way (Excel-style); Esc cancels.
 * Command line (after ':'):  w [file] (save) · q · q! · wq [file] · a cell ref
 *   like C10 (jump there) · Esc cancels.
 *
 * Launch: `sheet [file]` from the shell, or the Apps menu (loads a demo sheet).
 * The native file format is one `CELLREF rawtext` line per non-empty cell.
 * Kernel cooks no PgUp/Ctrl/function keys to apps, so navigation is arrows-only
 * and the grid auto-scrolls to follow the selection.
 */
#include "ulib.h"
#include "sheeteval.h"      /* the pure cell model + formula engine (host-tested by tests/sheet) */

#define FIELDW   8              /* on-screen width of a cell's value field */
#define RHW      4              /* row-header width ("100 ") */
#define VIEWROWS 18             /* visible data rows (fits the default 80x24 window) */
#define VIEWCOLS 8              /* visible columns: RHW + 8*(FIELDW+1) = 76 <= 80 */
#define IOMAX    65536          /* load/save scratch buffer */

enum { MODE_NAV, MODE_EDIT, MODE_CMD };

static char iobuf[IOMAX];
static int  cur_r, cur_c;            /* selected cell */
static int  top_row, left_col;       /* scroll origin */
static int  mode = MODE_NAV;
static int  modified;
static char fname[64];
static char status[72];              /* transient status/help message */
static char edit_buf[RAWMAX];  static int edit_len;
static char cmd_buf[72];        static int cmd_len;

/* ---- file load / save -----------------------------------------------------*/
static void load_file(void) {
    long n = sys_readfile(fname, iobuf, IOMAX - 1);
    if (n < 0) { scopy(status, "new file", sizeof status); return; }   /* absent -> fresh sheet */
    iobuf[n] = 0;
    long i = 0;
    while (i < n) {
        long ls = i; while (i < n && iobuf[i] != '\n') i++;
        long le = i; if (i < n) i++;
        char *line = iobuf + ls; line[le - ls] = 0;      /* NUL-terminate this line in place */
        char *p = line; while (*p == ' ') p++;
        if (!*p || *p == '#') continue;
        char ref[8]; int rn = 0;
        while ((is_alpha(*p) || is_digit(*p)) && rn < 7) ref[rn++] = *p++;
        ref[rn] = 0;
        if (*p == ' ') p++;
        int r, c;
        if (parse_whole_ref(ref, &r, &c)) set_raw(r, c, p);
    }
    scopy(status, "loaded", sizeof status);
}

static void save_file(void) {
    if (!fname[0]) { scopy(status, "no filename: use :w NAME", sizeof status); return; }
    int p = 0;
    for (int r = 0; r < NROWS && p < IOMAX - RAWMAX - 8; r++)
        for (int c = 0; c < NCOLS && p < IOMAX - RAWMAX - 8; c++) {
            const char *raw = CELL(r, c)->raw;
            if (!raw[0]) continue;
            iobuf[p++] = (char)('A' + c);
            int row = r + 1; char d[4]; int dn = 0;
            while (row) { d[dn++] = (char)('0' + row % 10); row /= 10; }
            while (dn) iobuf[p++] = d[--dn];
            iobuf[p++] = ' ';
            for (int k = 0; raw[k]; k++) iobuf[p++] = raw[k];
            iobuf[p++] = '\n';
        }
    if (sys_writefile(fname, iobuf, p) < 0) { scopy(status, "save FAILED", sizeof status); return; }
    modified = 0;
    scopy(status, "saved ", sizeof status);
    int l = slen(status); scopy(status + l, fname, (int)sizeof status - l);
}

/* A small starter sheet so the Apps-menu launch shows the feature immediately. */
static void load_demo(void) {
    set_raw(0, 0, "Region"); set_raw(0, 1, "Q1"); set_raw(0, 2, "Q2"); set_raw(0, 3, "Total");
    set_raw(1, 0, "North");  set_raw(1, 1, "120"); set_raw(1, 2, "150"); set_raw(1, 3, "=B2+C2");
    set_raw(2, 0, "South");  set_raw(2, 1, "90");  set_raw(2, 2, "110"); set_raw(2, 3, "=B3+C3");
    set_raw(3, 0, "East");   set_raw(3, 1, "60");  set_raw(3, 2, "95");  set_raw(3, 3, "=B4+C4");
    set_raw(4, 0, "West");   set_raw(4, 1, "200"); set_raw(4, 2, "180"); set_raw(4, 3, "=B5+C5");
    set_raw(5, 0, "Total");  set_raw(5, 1, "=SUM(B2:B5)"); set_raw(5, 2, "=SUM(C2:C5)"); set_raw(5, 3, "=SUM(D2:D5)");
    set_raw(7, 0, "Avg/qtr"); set_raw(7, 1, "=AVERAGE(B2:B5)"); set_raw(7, 2, "=AVERAGE(C2:C5)");
    set_raw(7, 3, "=AVERAGE(D2:D5)");
    set_raw(9, 0, "Best");   set_raw(9, 1, "=MAX(D2:D5)");
    scopy(status, "demo sheet -- edit freely, :w NAME to save", sizeof status);
}

/* ---- rendering ------------------------------------------------------------*/
static void putn_pad(long v, int w) {           /* right-align a small integer in w cols */
    char d[12]; int n = 0; long a = v < 0 ? -v : v;
    if (a == 0) d[n++] = '0'; else while (a) { d[n++] = (char)('0' + a % 10); a /= 10; }
    if (v < 0) d[n++] = '-';
    for (int i = n; i < w; i++) print(" ");
    char s[2] = { 0, 0 }; while (n) { s[0] = d[--n]; print(s); }
}
static void putstr_pad_left(const char *s, int w) {   /* right-align text in w cols */
    int l = slen(s); if (l > w) l = w;
    for (int i = l; i < w; i++) print(" ");
    char c[2] = { 0, 0 }; for (int i = 0; i < l; i++) { c[0] = s[i]; print(c); }
}
static void putstr_pad_right(const char *s, int w) {  /* left-align text in w cols, truncate */
    int l = slen(s); if (l > w) l = w;
    char c[2] = { 0, 0 }; for (int i = 0; i < l; i++) { c[0] = s[i]; print(c); }
    for (int i = l; i < w; i++) print(" ");
}
static void putch(char ch) { char s[2] = { ch, 0 }; print(s); }

static void ensure_visible(void) {
    if (cur_r < top_row) top_row = cur_r;
    if (cur_r >= top_row + VIEWROWS) top_row = cur_r - VIEWROWS + 1;
    if (cur_c < left_col) left_col = cur_c;
    if (cur_c >= left_col + VIEWCOLS) left_col = cur_c - VIEWCOLS + 1;
    if (top_row < 0) top_row = 0;
    if (left_col < 0) left_col = 0;
}

static void render(void) {
    sys_clear();
    /* title bar: name/modified + current ref + its raw content (+ value) */
    sys_setcolor(4); print(" sheet "); sys_setcolor(1);
    print(fname[0] ? fname : "(untitled)");
    if (modified) { sys_setcolor(7); print(" *"); }
    sys_setcolor(8); print("   ");
    putch((char)('A' + cur_c)); putn_pad(cur_r + 1, 0); print(": ");
    cell_t *sel = CELL(cur_r, cur_c);
    sys_setcolor(sel->raw[0] == '=' ? 10 : sel->kind == K_TEXT ? 6 : 1);
    if (sel->raw[0]) print(sel->raw); else { sys_setcolor(8); print("(empty)"); }
    if (sel->raw[0] == '=' && !sel->err) { sys_setcolor(8); print("  = "); sys_setcolor(3); print(fmt_value(sel->val, 20)); }
    if (sel->err) { sys_setcolor(2); print(sel->err == ERR_CIRC ? "  #CIRC" : "  #ERR"); }
    sys_setcolor(0); print("\n");

    /* column-letter header */
    for (int i = 0; i < RHW; i++) print(" ");
    for (int vc = 0; vc < VIEWCOLS; vc++) {
        int c = left_col + vc; if (c >= NCOLS) break;
        sys_setcolor(c == cur_c ? 3 : 8);                /* highlight the active column */
        print("   "); putch((char)('A' + c)); print("    ");   /* letter centred in FIELDW=8 */
        sys_setcolor(0); print(" ");
    }
    print("\n");

    /* data rows */
    for (int vr = 0; vr < VIEWROWS; vr++) {
        int r = top_row + vr; if (r >= NROWS) { print("\n"); continue; }
        sys_setcolor(r == cur_r ? 3 : 8);                /* highlight the active row number */
        putn_pad(r + 1, RHW - 1); print(" ");
        for (int vc = 0; vc < VIEWCOLS; vc++) {
            int c = left_col + vc; if (c >= NCOLS) break;
            cell_t *cell = CELL(r, c);
            int sel_here = (r == cur_r && c == cur_c);
            if (sel_here) sys_setcolor(4);               /* selection: cyan */
            else if (cell->err) sys_setcolor(2);         /* error: red */
            else if (cell->kind == K_TEXT) sys_setcolor(6);     /* text: blue */
            else if (cell->kind == K_FORMULA) sys_setcolor(10); /* formula result: teal */
            else sys_setcolor(1);                        /* plain number: white */
            if (cell->err) putstr_pad_left(cell->err == ERR_CIRC ? "#CIRC" : "#ERR", FIELDW);
            else if (cell->is_num) putstr_pad_left(fmt_value(cell->val, FIELDW), FIELDW);
            else if (cell->kind == K_TEXT) putstr_pad_right(cell->raw, FIELDW);
            else { for (int i = 0; i < FIELDW; i++) print(" "); }
            sys_setcolor(0); print(" ");
        }
        print("\n");
    }

    /* bottom line: edit input, command line, or status/help */
    if (mode == MODE_EDIT) {
        sys_setcolor(3); putch((char)('A' + cur_c)); putn_pad(cur_r + 1, 0); print(" = ");
        sys_setcolor(1); print(edit_buf);
    } else if (mode == MODE_CMD) {
        sys_setcolor(4); print(":"); sys_setcolor(1); print(cmd_buf);
    } else if (status[0]) {
        sys_setcolor(8); print(" "); print(status);
    } else {
        sys_setcolor(8); print(" arrows move  type/Enter edit  Bksp clear  :w save  :q quit");
    }
    sys_setcolor(0);
}

/* ---- editing / commands ---------------------------------------------------*/
static void begin_edit(int seed) {
    mode = MODE_EDIT; edit_len = 0; edit_buf[0] = 0;
    if (seed == 0) { scopy(edit_buf, CELL(cur_r, cur_c)->raw, RAWMAX); edit_len = slen(edit_buf); }
    else if (edit_len < RAWMAX - 1) { edit_buf[edit_len++] = (char)seed; edit_buf[edit_len] = 0; }
}
static void commit_edit(void) {
    scopy(CELL(cur_r, cur_c)->raw, edit_buf, RAWMAX);
    modified = 1; mode = MODE_NAV; status[0] = 0;
    recompute();
}
static void move_sel(int dr, int dc) {
    cur_r += dr; cur_c += dc;
    if (cur_r < 0) cur_r = 0; if (cur_r >= NROWS) cur_r = NROWS - 1;
    if (cur_c < 0) cur_c = 0; if (cur_c >= NCOLS) cur_c = NCOLS - 1;
}

static int exec_cmd(void) {                      /* returns 1 to quit */
    char *c = cmd_buf; while (*c == ' ') c++;
    if (streq(c, "q"))  { if (modified) { scopy(status, "unsaved -- :w to save or :q! to discard", sizeof status); return 0; } return 1; }
    if (streq(c, "q!")) return 1;
    if (streq(c, "w"))  { save_file(); return 0; }
    if (streq(c, "wq") || streq(c, "x")) { save_file(); return !streq(status, "save FAILED"); }
    if (startswith(c, "w "))  { char *n = c + 2; while (*n == ' ') n++; scopy(fname, n, sizeof fname); save_file(); return 0; }
    if (startswith(c, "wq ")) { char *n = c + 3; while (*n == ' ') n++; scopy(fname, n, sizeof fname); save_file(); return !streq(status, "save FAILED"); }
    int r, cc;
    if (parse_whole_ref(c, &r, &cc)) { cur_r = r; cur_c = cc; return 0; }
    scopy(status, "unknown command", sizeof status);
    return 0;
}

int main(void) {
    char arg[64];
    int have = sys_getarg(arg, sizeof arg) > 0;
    if (have && !streq(arg, "demo")) { scopy(fname, arg, sizeof fname); load_file(); }
    else load_demo();
    recompute();
    ensure_visible();
    render();

    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(15); continue; }

        if (mode == MODE_EDIT) {
            if (k == 27) { mode = MODE_NAV; status[0] = 0; }
            else if (k == '\n' || k == '\r') { commit_edit(); move_sel(1, 0); }
            else if (k == 8 || k == 127) { if (edit_len > 0) edit_buf[--edit_len] = 0; }
            else if (k == 0x11) { commit_edit(); move_sel(-1, 0); }
            else if (k == 0x12) { commit_edit(); move_sel(1, 0); }
            else if (k == 0x13) { commit_edit(); move_sel(0, -1); }
            else if (k == 0x14) { commit_edit(); move_sel(0, 1); }
            else if (k >= 32 && k < 127) { if (edit_len < RAWMAX - 1) { edit_buf[edit_len++] = (char)k; edit_buf[edit_len] = 0; } }
        } else if (mode == MODE_CMD) {
            if (k == 27) { mode = MODE_NAV; status[0] = 0; }
            else if (k == '\n' || k == '\r') { mode = MODE_NAV; if (exec_cmd()) break; }
            else if (k == 8 || k == 127) { if (cmd_len > 0) cmd_buf[--cmd_len] = 0; }
            else if (k >= 32 && k < 127) { if (cmd_len < (int)sizeof cmd_buf - 1) { cmd_buf[cmd_len++] = (char)k; cmd_buf[cmd_len] = 0; } }
        } else {   /* MODE_NAV */
            status[0] = 0;
            if (k == 0x11) move_sel(-1, 0);
            else if (k == 0x12) move_sel(1, 0);
            else if (k == 0x13) move_sel(0, -1);
            else if (k == 0x14) move_sel(0, 1);
            else if (k == 8 || k == 127) { CELL(cur_r, cur_c)->raw[0] = 0; modified = 1; recompute(); }
            else if (k == '\n' || k == '\r') begin_edit(0);
            else if (k == ':') { mode = MODE_CMD; cmd_len = 0; cmd_buf[0] = 0; }
            else if (k >= 32 && k < 127) begin_edit(k);
        }
        ensure_visible();
        render();
    }
    sys_clear();
    return 0;
}
