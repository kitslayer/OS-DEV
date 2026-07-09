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
 *   Operators: + - * / % ^ (power, right-assoc), unary -, parentheses, and
 *              comparisons  = <> < <= > >=  (lowest precedence, yield 1/0).
 *   Cell refs: A1 .. Z100 (case-insensitive).  Ranges: A1:B10 (inside a fn).
 *   Functions: SUM AVERAGE(=AVG) MIN MAX COUNT COUNTA PRODUCT STDEV STDEVP VAR VARP
 *              MEDIAN MODE
 *              SUMIF/COUNTIF/AVERAGEIF(range, [op]value)  (op = = <> < <= > >=)
 *              IF(cond,then[,else]) AND(...) OR(...) NOT(x)  (logical, 1=true/0=false)
 *              SQRT ABS INT FLOOR CEIL(=CEILING) ROUND(x[,dp]) TRUNC(x[,dp]) MOD POW(=POWER)
 *              SIGN LN LOG(x[,base]) LOG10 LOG2 EXP SIN COS TAN ASIN ACOS ATAN
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
 * Command line (after ':'):  w [file] (save) · q · q! · wq [file] · chart
 *   [range] (a horizontal bar chart of a range, or the current column) · y (yank
 *   the current cell) · p (paste it here, shifting relative refs by the move —
 *   =A1+B1 yanked from D1 pasted at D2 becomes =A2+B2) · fd N / fr N (fill the
 *   current cell down / right N cells with the same ref adjustment — "fill a
 *   formula down a column") · sort RANGE / sortd RANGE (sort those rows by the
 *   range's column, ascending / descending, whole rows moving with formulas
 *   ref-adjusted — e.g. :sort D2:D5) · fmt CODE (set the current COLUMN's number
 *   display format: $ currency, % percent, 0..6 fixed decimals, G general) ·
 *   find TEXT (jump to the next cell whose text or value contains TEXT, case-
 *   insensitive, wrapping; a bare :find repeats the last search) · u / undo
 *   (single-level whole-sheet undo of the last change — typed cell, delete,
 *   paste, fill, sort or format; a second :u redoes it) · a cell ref
 *   like C10 (jump there) · Esc.
 *
 * Launch: `sheet [file]` from the shell, or the Apps menu (loads a demo sheet).
 * The native file format is one `CELLREF rawtext` line per non-empty cell; a
 * name ending in .csv is loaded/saved as CSV instead (RFC 4180 — export writes
 * computed values, so formulas resolve to numbers, like any real spreadsheet).
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

enum { MODE_NAV, MODE_EDIT, MODE_CMD, MODE_CHART };

static char iobuf[IOMAX];
static int  cur_r, cur_c;            /* selected cell */
static int  top_row, left_col;       /* scroll origin */
static int  mode = MODE_NAV;
static int  modified;
static char fname[64];
static char status[72];              /* transient status/help message */
static char edit_buf[RAWMAX];  static int edit_len;
static char cmd_buf[72];        static int cmd_len;
static int  ch_r1, ch_c1, ch_r2, ch_c2;   /* :chart target rect (inclusive, 0-based) */
static char yank_buf[RAWMAX];             /* copy/paste buffer: raw text of the yanked cell */
static int  yank_r, yank_c, have_yank;    /* its origin cell (for relative-ref adjustment on paste) */
static char col_fmt[NCOLS];               /* per-column number display format (0/'G'=general, 0-6/%/$) */
static char find_q[RAWMAX];               /* last :find query (bare :find repeats it) */

/* Format a cell ref ("D12") into buf (for status messages). */
static void ref_str(int r, int c, char *buf) {
    int n = 0; buf[n++] = (char)('A' + c);
    int rr = r + 1; char d[6]; int dn = 0;
    while (rr) { d[dn++] = (char)('0' + rr % 10); rr /= 10; }
    while (dn) buf[n++] = d[--dn];
    buf[n] = 0;
}

/* ---- file load / save -----------------------------------------------------*/
static int is_csv(const char *name) {                    /* case-insensitive ".csv" suffix */
    int n = slen(name);
    return n >= 4 && name[n - 4] == '.' &&
           up(name[n - 3]) == 'C' && up(name[n - 2]) == 'S' && up(name[n - 1]) == 'V';
}

static void load_file(void) {
    long n = sys_readfile(fname, iobuf, IOMAX - 1);
    if (n < 0) { scopy(status, "new file", sizeof status); return; }   /* absent -> fresh sheet */
    iobuf[n] = 0;
    if (is_csv(fname)) { sheet_from_csv(iobuf); scopy(status, "loaded CSV", sizeof status); return; }
    long i = 0;
    while (i < n) {
        long ls = i; while (i < n && iobuf[i] != '\n') i++;
        long le = i; if (i < n) i++;
        char *line = iobuf + ls; line[le - ls] = 0;      /* NUL-terminate this line in place */
        char *p = line; while (*p == ' ') p++;
        if (!*p || *p == '#') continue;
        if (startswith(p, "%fmt ")) {                    /* per-column formats (M1726) */
            char *f = p + 5;
            for (int c = 0; c < NCOLS && *f && *f != '\n'; c++, f++) col_fmt[c] = (*f == 'G') ? 0 : *f;
            continue;
        }
        if (*p == '%') continue;                          /* any other directive line: ignore */
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
    int p;
    if (is_csv(fname)) {
        p = sheet_to_csv(iobuf, IOMAX);                  /* interchange: computed values, RFC-4180 */
    } else {
        p = 0;                                           /* native: one "CELLREF rawtext" line per cell */
        int anyfmt = 0; for (int c = 0; c < NCOLS; c++) if (col_fmt[c]) anyfmt = 1;
        if (anyfmt) {                                    /* per-column formats: "%fmt <26 codes>" (M1726) */
            for (const char *pre = "%fmt "; *pre; pre++) iobuf[p++] = *pre;
            for (int c = 0; c < NCOLS; c++) iobuf[p++] = col_fmt[c] ? col_fmt[c] : 'G';
            iobuf[p++] = '\n';
        }
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
    }
    if (sys_writefile(fname, iobuf, p) < 0) { scopy(status, "save FAILED", sizeof status); return; }
    modified = 0;
    scopy(status, "saved ", sizeof status);
    int l = slen(status); scopy(status + l, fname, (int)sizeof status - l);
}

/* A small starter sheet so the Apps-menu launch shows the feature immediately. */
static void load_demo(void) {
    set_raw(0, 0, "Region"); set_raw(0, 1, "Q1"); set_raw(0, 2, "Q2"); set_raw(0, 3, "Total"); set_raw(0, 4, "Pass?");
    set_raw(1, 0, "North");  set_raw(1, 1, "120"); set_raw(1, 2, "150"); set_raw(1, 3, "=B2+C2"); set_raw(1, 4, "=IF(D2>=250,1,0)");
    set_raw(2, 0, "South");  set_raw(2, 1, "90");  set_raw(2, 2, "110"); set_raw(2, 3, "=B3+C3"); set_raw(2, 4, "=IF(D3>=250,1,0)");
    set_raw(3, 0, "East");   set_raw(3, 1, "60");  set_raw(3, 2, "95");  set_raw(3, 3, "=B4+C4"); set_raw(3, 4, "=IF(D4>=250,1,0)");
    set_raw(4, 0, "West");   set_raw(4, 1, "200"); set_raw(4, 2, "180"); set_raw(4, 3, "=B5+C5"); set_raw(4, 4, "=IF(D5>=250,1,0)");
    set_raw(5, 0, "Total");  set_raw(5, 1, "=SUM(B2:B5)"); set_raw(5, 2, "=SUM(C2:C5)"); set_raw(5, 3, "=SUM(D2:D5)"); set_raw(5, 4, "=SUM(E2:E5)");
    set_raw(7, 0, "Avg/qtr"); set_raw(7, 1, "=AVERAGE(B2:B5)"); set_raw(7, 2, "=AVERAGE(C2:C5)");
    set_raw(7, 3, "=AVERAGE(D2:D5)");
    set_raw(9, 0, "Best");   set_raw(9, 1, "=MAX(D2:D5)");
    set_raw(10, 0, "StdDev"); set_raw(10, 1, "=STDEV(B2:B5)");
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

/* A full-screen horizontal bar chart of the ch_* rect: one labeled bar per
 * numeric cell, scaled to the largest magnitude, teal for +ve / red for -ve. */
#define CHART_MAXBARS 16
static void render_chart(void) {
    sys_clear();
    sys_setcolor(4); print(" chart "); sys_setcolor(1);
    putch((char)('A' + ch_c1)); putn_pad(ch_r1 + 1, 0);
    if (ch_r1 != ch_r2 || ch_c1 != ch_c2) { print(":"); putch((char)('A' + ch_c2)); putn_pad(ch_r2 + 1, 0); }
    sys_setcolor(8); print("    (press any key to return)\n\n");

    double vals[CHART_MAXBARS]; char labs[CHART_MAXBARS][16]; int nb = 0; double peak = 0;
    int single_col = (ch_c1 == ch_c2), single_row = (ch_r1 == ch_r2);
    for (int r = ch_r1; r <= ch_r2 && nb < CHART_MAXBARS; r++)
        for (int c = ch_c1; c <= ch_c2 && nb < CHART_MAXBARS; c++) {
            cell_t *cell = CELL(r, c);
            if (!cell->is_num) continue;                 /* chart only numeric cells */
            vals[nb] = cell->val;
            if (single_col && ch_c1 > 0 && CELL(r, ch_c1 - 1)->raw[0])
                scopy(labs[nb], CELL(r, ch_c1 - 1)->raw, 16);        /* label from the column to the left */
            else if (single_row && ch_r1 > 0 && CELL(ch_r1 - 1, c)->raw[0])
                scopy(labs[nb], CELL(ch_r1 - 1, c)->raw, 16);        /* or the row above */
            else {                                                    /* else the cell's own ref */
                int n = 0; labs[nb][n++] = (char)('A' + c);
                int rr = r + 1; char d[6]; int dn = 0;
                while (rr) { d[dn++] = (char)('0' + rr % 10); rr /= 10; }
                while (dn) labs[nb][n++] = d[--dn];
                labs[nb][n] = 0;
            }
            double a = vals[nb] < 0 ? -vals[nb] : vals[nb];
            if (a > peak) peak = a;
            nb++;
        }
    if (!nb) { sys_setcolor(2); print(" no numeric values in that range\n"); sys_setcolor(0); return; }

    int labw = 8, valw = 10, barmax = 76 - labw - valw - 1; if (barmax < 4) barmax = 4;
    for (int i = 0; i < nb; i++) {
        sys_setcolor(6); putstr_pad_right(labs[i], labw); print(" ");
        double a = vals[i] < 0 ? -vals[i] : vals[i];
        int blen = peak > 0 ? (int)(a / peak * barmax + 0.5) : 0;
        if (blen < 1 && a > 0) blen = 1;                 /* a sliver for tiny non-zero values */
        sys_setcolor(vals[i] < 0 ? 2 : 10);
        for (int k = 0; k < blen; k++) putch('#');
        for (int k = blen; k < barmax; k++) print(" ");
        sys_setcolor(3); print(" "); print(fmt_value(vals[i], valw));
        sys_setcolor(0); print("\n");
    }
    sys_setcolor(8); print("\n longest bar = "); print(fmt_value(peak, 12));
    if (nb >= CHART_MAXBARS) { print("   (first "); putn_pad(CHART_MAXBARS, 0); print(" values)"); }
    sys_setcolor(0);
}

static void render(void) {
    if (mode == MODE_CHART) { render_chart(); return; }
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
    if (sel->raw[0] == '=' && !sel->err) { sys_setcolor(8); print("  = "); sys_setcolor(3); print(fmt_value_col(sel->val, 20, col_fmt[cur_c])); }
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
            else if (cell->is_num) putstr_pad_left(fmt_value_col(cell->val, FIELDW, col_fmt[c]), FIELDW);
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
        sys_setcolor(8); print(" arrows  :y/:p copy  :fd/:fr fill  :sort :fmt :find  :u undo  :w  :q");
    }
    sys_setcolor(0);
}

/* ---- editing / commands ---------------------------------------------------*/
static void begin_edit(int seed) {
    mode = MODE_EDIT; edit_len = 0; edit_buf[0] = 0;
    if (seed == 0) { scopy(edit_buf, CELL(cur_r, cur_c)->raw, RAWMAX); edit_len = slen(edit_buf); }
    else if (edit_len < RAWMAX - 1) { edit_buf[edit_len++] = (char)seed; edit_buf[edit_len] = 0; }
}
/* ---- single-level whole-grid undo (M1736) ---------------------------------*
 * Snapshot every cell's raw text + the per-column formats before each mutating
 * operation; :u swaps the live grid with the snapshot, so a second :u redoes.
 * One buffer (same footprint as the sort scratch) covers every edit uniformly
 * -- typed cells, delete, paste, fill, sort, format. raw text is the source of
 * truth; recompute() rebuilds each cell's kind/value from it after a swap. */
static char undo_buf[NROWS][NCOLS][RAWMAX];
static char undo_fmt[NCOLS];
static int  undo_have = 0;               /* a snapshot exists */
static int  undo_done = 0;               /* the last :u left us in the undone state */
static void save_undo(void) {
    for (int r = 0; r < NROWS; r++)
        for (int c = 0; c < NCOLS; c++) scopy(undo_buf[r][c], CELL(r, c)->raw, RAWMAX);
    for (int c = 0; c < NCOLS; c++) undo_fmt[c] = col_fmt[c];
    undo_have = 1; undo_done = 0;
}
static void do_undo(void) {
    if (!undo_have) { scopy(status, "nothing to undo", sizeof status); return; }
    for (int r = 0; r < NROWS; r++)
        for (int c = 0; c < NCOLS; c++) {
            char tmp[RAWMAX];
            scopy(tmp, CELL(r, c)->raw, RAWMAX);
            scopy(CELL(r, c)->raw, undo_buf[r][c], RAWMAX);
            scopy(undo_buf[r][c], tmp, RAWMAX);
        }
    for (int c = 0; c < NCOLS; c++) { char t = col_fmt[c]; col_fmt[c] = undo_fmt[c]; undo_fmt[c] = t; }
    modified = 1; recompute();
    undo_done = !undo_done;
    scopy(status, undo_done ? "undone -- :u again to redo" : "redone -- :u again to undo", sizeof status);
}

static void commit_edit(void) {
    save_undo();                         /* snapshot the pre-edit grid for :u */
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
    if (streq(c, "chart") || startswith(c, "chart ")) {
        const char *rng = c + 5; while (*rng == ' ') rng++;
        if (!*rng) {                                       /* no range: chart this column's used cells */
            int top = -1, bot = -1;
            for (int r = 0; r < NROWS; r++) if (CELL(r, cur_c)->raw[0]) { if (top < 0) top = r; bot = r; }
            if (top < 0) { scopy(status, "chart: this column is empty", sizeof status); return 0; }
            ch_r1 = top; ch_r2 = bot; ch_c1 = ch_c2 = cur_c; mode = MODE_CHART; status[0] = 0;
        } else if (parse_range(rng, &ch_r1, &ch_c1, &ch_r2, &ch_c2)) {
            mode = MODE_CHART; status[0] = 0;
        } else scopy(status, "chart: usage  :chart B2:B5  (or :chart for this column)", sizeof status);
        return 0;
    }
    if (streq(c, "u") || streq(c, "undo")) { do_undo(); return 0; }   /* single-level undo/redo (toggles) */
    if (streq(c, "y")) {                             /* yank the current cell (copy) */
        scopy(yank_buf, CELL(cur_r, cur_c)->raw, RAWMAX);
        yank_r = cur_r; yank_c = cur_c; have_yank = 1;
        char ref[8]; ref_str(cur_r, cur_c, ref);
        scopy(status, "yanked ", sizeof status);
        int l = slen(status); scopy(status + l, ref, (int)sizeof status - l);
        return 0;
    }
    if (streq(c, "p")) {                             /* paste into the current cell, ref-adjusted */
        if (!have_yank) { scopy(status, "nothing yanked -- :y a cell first", sizeof status); return 0; }
        save_undo();
        char out[RAWMAX];
        adjust_refs(yank_buf, cur_r - yank_r, cur_c - yank_c, out, RAWMAX);
        scopy(CELL(cur_r, cur_c)->raw, out, RAWMAX);
        modified = 1; recompute();
        scopy(status, "pasted (refs adjusted)", sizeof status);
        return 0;
    }
    if (startswith(c, "fd") || startswith(c, "fr")) {   /* fill the current cell down / right N cells */
        int down = (c[1] == 'd');
        const char *a = c + 2; while (*a == ' ') a++;
        int n = 0; while (is_digit(*a)) { n = n * 10 + (*a - '0'); a++; }
        while (*a == ' ') a++;
        if (n < 1 || *a) { scopy(status, down ? "usage: :fd N  (fill cell down N rows)"
                                              : "usage: :fr N  (fill cell right N cols)", sizeof status); return 0; }
        save_undo();
        char src[RAWMAX]; scopy(src, CELL(cur_r, cur_c)->raw, RAWMAX);   /* snapshot before overwriting */
        int filled = 0;
        for (int i = 1; i <= n; i++) {
            int rr = cur_r + (down ? i : 0), cc2 = cur_c + (down ? 0 : i);
            if (rr >= NROWS || cc2 >= NCOLS) break;
            char out[RAWMAX];
            adjust_refs(src, down ? i : 0, down ? 0 : i, out, RAWMAX);
            scopy(CELL(rr, cc2)->raw, out, RAWMAX); filled++;
        }
        modified = 1; recompute();
        scopy(status, filled ? (down ? "filled down" : "filled right") : "nothing to fill", sizeof status);
        return 0;
    }
    if (startswith(c, "find")) {                     /* :find TEXT — jump to the next matching cell (bare :find repeats) */
        const char *a = c + 4; while (*a == ' ') a++;
        if (*a) scopy(find_q, a, sizeof find_q);
        if (!find_q[0]) { scopy(status, "usage: :find TEXT  (then :find repeats)", sizeof status); return 0; }
        int r, cc;
        if (sheet_find(find_q, cur_r, cur_c, &r, &cc)) {
            cur_r = r; cur_c = cc;
            char ref[8]; ref_str(r, cc, ref);
            scopy(status, "found ", sizeof status); int l = slen(status);
            scopy(status + l, ref, (int)sizeof status - l); l = slen(status);
            scopy(status + l, ": ", (int)sizeof status - l); l = slen(status);
            scopy(status + l, find_q, (int)sizeof status - l);
        } else {
            scopy(status, "not found: ", sizeof status);
            int l = slen(status); scopy(status + l, find_q, (int)sizeof status - l);
        }
        return 0;
    }
    if (startswith(c, "fmt")) {                      /* :fmt CODE — set the current column's number format */
        const char *a = c + 3; while (*a == ' ') a++;
        save_undo();
        char code = *a;
        if (code == 0 || code == 'G' || code == 'g' || code == '-') { col_fmt[cur_c] = 0; scopy(status, "format: general", sizeof status); }
        else if ((code >= '0' && code <= '6') || code == '%' || code == '$') {
            col_fmt[cur_c] = code; modified = 1;
            scopy(status, code == '$' ? "format: currency ($)" : code == '%' ? "format: percent (%)" : "format: fixed decimals", sizeof status);
        } else scopy(status, "usage: :fmt $ | % | 0..6 | G   (sets column A..Z display)", sizeof status);
        if (col_fmt[cur_c] == 0) modified = 1;
        return 0;
    }
    if (startswith(c, "sort")) {                     /* :sort D2:D5 (asc) / :sortd D2:D5 (desc) */
        int desc = (c[4] == 'd');
        const char *rng = c + (desc ? 5 : 4); while (*rng == ' ') rng++;
        int r1, c1, r2, c2;
        if (parse_range(rng, &r1, &c1, &r2, &c2)) {
            save_undo();
            sort_rows(r1, r2, c1, desc);             /* rows r1..r2 keyed by the range's column c1 */
            modified = 1; recompute();
            cur_r = r1; cur_c = c1;                  /* park on the top of the sorted block */
            scopy(status, desc ? "sorted (descending)" : "sorted (ascending)", sizeof status);
        } else scopy(status, "usage: :sort D2:D5  (sort rows by column D; :sortd = descending)", sizeof status);
        return 0;
    }
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

        if (mode == MODE_CHART) {
            mode = MODE_NAV; status[0] = 0;               /* any key dismisses the chart */
        } else if (mode == MODE_EDIT) {
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
            else if (k == 8 || k == 127) { if (CELL(cur_r, cur_c)->raw[0]) { save_undo(); CELL(cur_r, cur_c)->raw[0] = 0; modified = 1; recompute(); } }
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
