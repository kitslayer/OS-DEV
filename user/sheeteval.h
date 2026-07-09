/* sheeteval.h — the spreadsheet's cell model + formula engine (M1696).
 *
 * Split out from user/sheet.c the same way calceval.h is split from calc.c: the
 * whole computational core (the cell grid, a recursive-descent formula evaluator
 * with cell references / ranges / functions, the recalc pass with circular-
 * reference detection, and number formatting) is PURE — no syscalls — so it is
 * host-unit-tested by tests/sheet while sheet.c keeps only the UI, file I/O and
 * the input loop. dmath.h supplies the from-scratch IEEE-754 math (verbatim from
 * the JS engine), so this needs SSE and -fwrapv, exactly like calc.
 *
 * A cell stores the raw text you typed. '=' prefix -> FORMULA; otherwise a pure
 * number -> NUMBER; anything else -> TEXT. eval_cell() computes one cell (memoised
 * per recompute pass, with a recursion cap and cycle detection); recompute() runs
 * the whole grid. A referenced empty/text cell reads as 0 in arithmetic; a range
 * (A1:B10) is only meaningful inside a function's argument list.
 *
 * Grammar:  compare := expr (('='|'<>'|'<'|'<='|'>'|'>=') expr)*   (yields 1/0)
 *           expr := term (('+'|'-') term)* ; term := power (('*'|'/'|'%') power)* ;
 *           power := factor ('^' power)? ;  factor := number | '(' compare ')'
 *                  | ('-'|'+') factor | cellref | name '(' args ')' | PI | E
 * Comparisons are the lowest precedence and evaluate to 1.0 (true) / 0.0 (false).
 * Functions: SUM AVERAGE/AVG MIN MAX COUNT COUNTA PRODUCT STDEV/STDEVP VAR/VARP
 *              (variadic, take ranges);  IF(c,a[,b]) AND/OR(...) NOT(x)  (logical);
 *            SQRT ABS INT FLOOR CEIL/CEILING ROUND(x[,dp]) TRUNC(x[,dp]) MOD POW/POWER
 *            SIGN LN LOG(x[,base]) LOG10 LOG2 EXP SIN COS TAN ASIN ACOS ATAN
 */
#ifndef SHEETEVAL_H
#define SHEETEVAL_H

#include "dmath.h"      /* js_pow/js_sqrt/js_floor/js_fmod/... + dnum_to_str (verbatim from js.c) */

#define NCOLS   26              /* columns A..Z */
#define NROWS   100             /* rows 1..100 */
#define RAWMAX  48              /* max characters of raw text per cell */
#define EVAL_MAX_DEPTH 100      /* formula recursion cap (guards the 512 KiB user stack) */

enum { K_EMPTY, K_NUM, K_TEXT, K_FORMULA };         /* cell kinds */
enum { ERR_OK = 0, ERR_SYNTAX = 1, ERR_CIRC = 2 };  /* cell error codes */
enum { ST_UNVISITED, ST_INPROG, ST_DONE };          /* per-recompute-pass memo state */

typedef struct {
    char          raw[RAWMAX];  /* exactly what the user typed; "" = empty */
    double        val;          /* computed numeric value (valid when is_num) */
    unsigned char kind;         /* K_* */
    unsigned char err;          /* ERR_* */
    unsigned char state;        /* ST_* (reset each recompute) */
    unsigned char is_num;       /* computed result is a usable number */
} cell_t;

static cell_t cells[NROWS][NCOLS];   /* BSS (the kernel defers BSS zeroing to a VMA) */

/* evaluator globals (single-threaded, one recompute at a time — like calceval) */
static const char *pcur;       /* parse cursor */
static int         perr;       /* ERR_* for the formula being parsed, 0 = ok */
static int         eval_depth; /* recursion guard */

static void   eval_cell(int r, int c);
static double eval_expr(void);
static double eval_compare(void);

static cell_t *CELL(int r, int c) { return &cells[r][c]; }

static int  is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int  is_digit(char c) { return c >= '0' && c <= '9'; }
static char up(char c)       { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* case-insensitive equality of a read identifier vs a keyword */
static int nameeq(const char *n, const char *kw) {
    int i = 0;
    for (; n[i] && kw[i]; i++) if (up(n[i]) != up(kw[i])) return 0;
    return n[i] == 0 && kw[i] == 0;
}

static void scopy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }
static int  slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void set_raw(int r, int c, const char *s) { scopy(CELL(r, c)->raw, s, RAWMAX); }

static void skipws(void) { while (*pcur == ' ' || *pcur == '\t') pcur++; }

/* Scan a decimal number (12, 3.14, .5, 1e3, 1.5e-2) at *pp; advance + return 1 on
 * success, else 0. No leading sign here (unary minus is handled in factor). */
static int scan_number(const char **pp, double *out) {
    const char *p = *pp; double v = 0; int any = 0;
    while (is_digit(*p)) { v = v * 10.0 + (*p - '0'); p++; any = 1; }
    if (*p == '.') { p++; double f = 0.1; while (is_digit(*p)) { v += (*p - '0') * f; f *= 0.1; p++; any = 1; } }
    if (any && (*p == 'e' || *p == 'E')) {
        const char *s = p; p++; int neg = 0;
        if (*p == '+') p++; else if (*p == '-') { neg = 1; p++; }
        int e = 0, ed = 0; while (is_digit(*p)) { e = e * 10 + (*p - '0'); p++; ed = 1; }
        if (ed) v *= js_pow(10.0, neg ? -(double)e : (double)e); else p = s;
    }
    if (!any) return 0;
    *pp = p; *out = v; return 1;
}

/* Parse a cell reference (letter + digits, e.g. B12) at pcur. On success advance
 * pcur, fill rr and cc (0-based) and return 1; else leave pcur untouched, return 0. */
static int parse_ref_cursor(int *rr, int *cc) {
    const char *p = pcur;
    if (!is_alpha(*p)) return 0;
    int col = up(*p) - 'A'; p++;
    if (!is_digit(*p)) return 0;
    int row = 0; while (is_digit(*p)) { row = row * 10 + (*p - '0'); p++; }
    row--;
    if (is_alpha(*p)) return 0;                         /* e.g. "A1B" is not a clean ref */
    if (col < 0 || col >= NCOLS || row < 0 || row >= NROWS) return 0;
    pcur = p; *rr = row; *cc = col; return 1;
}

/* Evaluate cell (r,c) if needed and return its numeric value (0 for empty/text);
 * propagate its error into the current formula via perr. */
static double get_cell_value(int r, int c) {
    if (r < 0 || r >= NROWS || c < 0 || c >= NCOLS) { perr = ERR_SYNTAX; return 0; }
    eval_cell(r, c);                                    /* saves/restores pcur/perr itself */
    cell_t *cell = CELL(r, c);
    if (cell->err) perr = cell->err;
    return cell->is_num ? cell->val : 0.0;
}

/* aggregate accumulator for SUM/AVG/MIN/MAX/COUNT/COUNTA/PRODUCT/STDEV/VAR
 * (sumsq lets STDEV/VAR compute in a single pass, no value buffer). */
typedef struct { double sum, sumsq, prod, mn, mx; int countnum, nonempty; } agg_t;
static void agg_init(agg_t *a) { a->sum = 0; a->sumsq = 0; a->prod = 1; a->mn = 0; a->mx = 0; a->countnum = 0; a->nonempty = 0; }
static void agg_add(agg_t *a, double v) {
    a->sum += v; a->sumsq += v * v; a->prod *= v;
    if (a->countnum == 0) { a->mn = a->mx = v; }
    else { if (v < a->mn) a->mn = v; if (v > a->mx) a->mx = v; }
    a->countnum++;
}
static void agg_range(agg_t *a, int r1, int c1, int r2, int c2) {
    if (r1 > r2) { int t = r1; r1 = r2; r2 = t; }
    if (c1 > c2) { int t = c1; c1 = c2; c2 = t; }
    for (int r = r1; r <= r2; r++) for (int c = c1; c <= c2; c++) {
        eval_cell(r, c);
        cell_t *cell = CELL(r, c);
        if (cell->raw[0]) a->nonempty++;
        if (cell->err) perr = cell->err;
        else if (cell->is_num) agg_add(a, cell->val);
    }
}

/* Parse a function argument list "arg,arg,...)" where each arg is a range
 * (A1:B2) or a scalar expression; feed everything into the accumulator. */
static void parse_agg_args(agg_t *a) {
    skipws();
    if (*pcur == ')') { pcur++; return; }
    for (;;) {
        skipws();
        const char *save = pcur;
        int r1, c1, r2, c2;
        if (parse_ref_cursor(&r1, &c1)) {
            skipws();
            if (*pcur == ':') {                         /* it is a range */
                pcur++; skipws();
                if (parse_ref_cursor(&r2, &c2)) agg_range(a, r1, c1, r2, c2);
                else perr = ERR_SYNTAX;
            } else {                                    /* a lone ref used as a scalar */
                pcur = save; double v = eval_compare(); agg_add(a, v); a->nonempty++;
            }
        } else { pcur = save; double v = eval_compare(); agg_add(a, v); a->nonempty++; }
        skipws();
        if (*pcur == ',') { pcur++; continue; }
        if (*pcur == ')') { pcur++; break; }
        perr = ERR_SYNTAX; break;
    }
}

/* Parse up to `max` comma-separated scalar expressions "e,e,...)"; return count. */
static int parse_scalar_args(double *out, int max) {
    skipws();
    if (*pcur == ')') { pcur++; return 0; }
    int n = 0;
    for (;;) {
        double v = eval_compare();
        if (n < max) out[n] = v;
        n++;
        skipws();
        if (*pcur == ',') { pcur++; continue; }
        if (*pcur == ')') { pcur++; break; }
        perr = ERR_SYNTAX; break;
    }
    return n;
}

/* Dispatch a function by name; '(' has already been consumed. */
static double call_function(const char *name) {
    /* range/variadic aggregates (take ranges or scalar lists) */
    if (nameeq(name, "SUM") || nameeq(name, "AVERAGE") || nameeq(name, "AVG") ||
        nameeq(name, "MIN") || nameeq(name, "MAX") || nameeq(name, "COUNT") ||
        nameeq(name, "COUNTA") || nameeq(name, "PRODUCT") ||
        nameeq(name, "STDEV") || nameeq(name, "STDEVP") ||
        nameeq(name, "VAR") || nameeq(name, "VARP")) {
        agg_t a; agg_init(&a); parse_agg_args(&a);
        if (nameeq(name, "SUM"))     return a.sum;
        if (nameeq(name, "PRODUCT")) return a.countnum ? a.prod : 0;
        if (nameeq(name, "MIN"))     return a.countnum ? a.mn : 0;
        if (nameeq(name, "MAX"))     return a.countnum ? a.mx : 0;
        if (nameeq(name, "COUNT"))   return (double)a.countnum;
        if (nameeq(name, "COUNTA"))  return (double)a.nonempty;
        if (nameeq(name, "VAR") || nameeq(name, "VARP") ||
            nameeq(name, "STDEV") || nameeq(name, "STDEVP")) {   /* one-pass sum-of-squared-deviations */
            int pop = nameeq(name, "VARP") || nameeq(name, "STDEVP");
            int denom = pop ? a.countnum : a.countnum - 1;       /* sample uses n-1 */
            if (a.countnum < 1 || denom < 1) return 0;
            double ss = a.sumsq - a.sum * a.sum / (double)a.countnum;
            if (ss < 0) ss = 0;                                  /* guard tiny fp negatives */
            double var = ss / (double)denom;
            return (nameeq(name, "VAR") || nameeq(name, "VARP")) ? var : js_sqrt(var);
        }
        return a.countnum ? a.sum / (double)a.countnum : 0;   /* AVERAGE / AVG */
    }

    /* control-flow / variadic-logical functions evaluate their own arguments
     * (IF short-circuits which branch's errors count; AND/OR are variadic) */
    if (nameeq(name, "IF")) {                     /* IF(cond, then [, else]) */
        double cond = eval_compare();
        skipws();
        if (*pcur == ',') pcur++; else { perr = ERR_SYNTAX; return 0; }
        int take = (cond != 0.0);
        int saved = perr; double thenv = eval_compare();
        if (!take) perr = saved;                  /* discard an error from the untaken then-branch */
        skipws();
        double elsev = 0;
        if (*pcur == ',') { pcur++; saved = perr; elsev = eval_compare(); if (take) perr = saved; }
        skipws();
        if (*pcur == ')') pcur++; else perr = ERR_SYNTAX;
        return take ? thenv : elsev;
    }
    if (nameeq(name, "AND") || nameeq(name, "OR")) {   /* variadic; each arg a scalar/comparison */
        int is_and = nameeq(name, "AND"), result = is_and ? 1 : 0;
        skipws();
        if (*pcur == ')') { pcur++; return 0; }
        for (;;) {
            double v = eval_compare();
            if (is_and) { if (v == 0.0) result = 0; } else { if (v != 0.0) result = 1; }
            skipws();
            if (*pcur == ',') { pcur++; continue; }
            if (*pcur == ')') { pcur++; break; }
            perr = ERR_SYNTAX; break;
        }
        return (double)result;
    }

    /* fixed-arity scalar functions */
    double a[3]; int n = parse_scalar_args(a, 3);
    if (n < 1) { perr = ERR_SYNTAX; return 0; }
    if (nameeq(name, "SQRT"))  return js_sqrt(a[0]);
    if (nameeq(name, "ABS"))   return js_fabs(a[0]);
    if (nameeq(name, "INT") || nameeq(name, "FLOOR")) return js_floor(a[0]);
    if (nameeq(name, "CEIL") || nameeq(name, "CEILING")) return js_ceil(a[0]);
    if (nameeq(name, "ROUND")) {
        if (n >= 2) { double m = js_pow(10.0, a[1]); return js_round(a[0] * m) / m; }
        return js_round(a[0]);
    }
    if (nameeq(name, "TRUNC")) {
        if (n >= 2) { double m = js_pow(10.0, a[1]); return js_trunc(a[0] * m) / m; }
        return js_trunc(a[0]);
    }
    if (nameeq(name, "MOD"))   { if (n < 2) { perr = ERR_SYNTAX; return 0; } return js_fmod(a[0], a[1]); }
    if (nameeq(name, "POW") || nameeq(name, "POWER")) { if (n < 2) { perr = ERR_SYNTAX; return 0; } return js_pow(a[0], a[1]); }
    if (nameeq(name, "NOT"))   return a[0] == 0.0 ? 1.0 : 0.0;
    if (nameeq(name, "SIGN"))  return a[0] > 0 ? 1.0 : a[0] < 0 ? -1.0 : 0.0;
    if (nameeq(name, "LN"))    return js_ln(a[0]);
    if (nameeq(name, "LOG"))   return n >= 2 ? js_ln(a[0]) / js_ln(a[1]) : js_ln(a[0]) / js_ln(10.0);
    if (nameeq(name, "LOG10")) return js_ln(a[0]) / js_ln(10.0);
    if (nameeq(name, "LOG2"))  return js_ln(a[0]) / js_ln(2.0);
    if (nameeq(name, "EXP"))   return js_exp(a[0]);
    if (nameeq(name, "SIN"))   return js_sin(a[0]);
    if (nameeq(name, "COS"))   return js_cos(a[0]);
    if (nameeq(name, "TAN"))   return js_tan(a[0]);
    if (nameeq(name, "ASIN"))  return js_asin(a[0]);
    if (nameeq(name, "ACOS"))  return js_acos(a[0]);
    if (nameeq(name, "ATAN"))  return js_atan(a[0]);
    perr = ERR_SYNTAX; return 0;
}

/* factor := number | '(' expr ')' | -factor | +factor | cellref | fn(args) | PI | E */
static double factor(void) {
    skipws();
    if (*pcur == '(') { pcur++; double v = eval_compare(); skipws(); if (*pcur == ')') pcur++; else perr = ERR_SYNTAX; return v; }
    if (*pcur == '-') { pcur++; return -factor(); }
    if (*pcur == '+') { pcur++; return factor(); }
    if (is_digit(*pcur) || *pcur == '.') { double v; if (!scan_number(&pcur, &v)) perr = ERR_SYNTAX; return v; }
    if (is_alpha(*pcur)) {
        char name[16]; int nl = 0;
        while ((is_alpha(*pcur) || is_digit(*pcur)) && nl < 15) name[nl++] = *pcur++;
        name[nl] = 0;
        if (*pcur == '(') { pcur++; return call_function(name); }
        /* cellref: one column letter then a row number */
        int li = 0; while (name[li] && is_alpha(name[li])) li++;
        if (li == 1 && name[li]) {
            int ok = 1; for (int j = li; name[j]; j++) if (!is_digit(name[j])) { ok = 0; break; }
            if (ok) {
                int col = up(name[0]) - 'A';
                int row = 0; for (int j = li; name[j]; j++) row = row * 10 + (name[j] - '0');
                row--;
                if (col >= 0 && col < NCOLS && row >= 0 && row < NROWS) return get_cell_value(row, col);
            }
        }
        if (nameeq(name, "PI")) return 3.14159265358979;
        if (nameeq(name, "E"))  return 2.71828182845905;
        perr = ERR_SYNTAX; return 0;
    }
    perr = ERR_SYNTAX; return 0;
}

static double power(void) {                              /* right-assoc ^, binds tighter than * / % */
    double b = factor(); skipws();
    if (*pcur == '^') { pcur++; double e = power(); return js_pow(b, e); }
    return b;
}
static double term(void) {
    double v = power();
    for (;;) { skipws();
        if (*pcur == '*') { pcur++; v *= power(); }
        else if (*pcur == '/') { pcur++; v /= power(); }
        else if (*pcur == '%') { pcur++; v = js_fmod(v, power()); }
        else break;
    }
    return v;
}
static double eval_expr(void) {
    double v = term();
    for (;;) { skipws();
        if (*pcur == '+') { pcur++; v += term(); }
        else if (*pcur == '-') { pcur++; v -= term(); }
        else break;
    }
    return v;
}

/* Comparison level — the lowest precedence, left-associative, yielding 1.0/0.0
 * like a boolean (Excel-style). Operators: = (equal), <> (not equal), < <= > >=.
 * This is the top of the grammar: a formula body, a parenthesised group and a
 * function argument all parse at this level, so `IF(A1>0, ...)` and `(x<y)*3`
 * work. Chains left-associatively: `1<2<3` -> `(1<2)<3` -> `1<3` -> 1. */
static double eval_compare(void) {
    double v = eval_expr();
    for (;;) {
        skipws();
        char c = *pcur;
        if (c == '=') { pcur++; double r = eval_expr(); v = (v == r); }
        else if (c == '<') {
            pcur++;
            if (*pcur == '=')      { pcur++; double r = eval_expr(); v = (v <= r); }
            else if (*pcur == '>') { pcur++; double r = eval_expr(); v = (v != r); }
            else                   {         double r = eval_expr(); v = (v <  r); }
        } else if (c == '>') {
            pcur++;
            if (*pcur == '=') { pcur++; double r = eval_expr(); v = (v >= r); }
            else              {         double r = eval_expr(); v = (v >  r); }
        } else break;
    }
    return v;
}

/* Is `raw` (ignoring surrounding spaces) a pure number? If so *out gets it. */
static int cell_is_number(const char *raw, double *out) {
    const char *p = raw; while (*p == ' ') p++;
    int neg = 0; if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    double v; if (!scan_number(&p, &v)) return 0;
    while (*p == ' ') p++;
    if (*p) return 0;
    *out = neg ? -v : v; return 1;
}

/* Evaluate one cell into its cached fields (memoised per recompute pass). */
static void eval_cell(int r, int c) {
    cell_t *cell = CELL(r, c);
    if (cell->state == ST_DONE) return;
    if (cell->state == ST_INPROG) { cell->err = ERR_CIRC; cell->is_num = 0; cell->val = 0; return; }
    if (cell->raw[0] == 0) { cell->kind = K_EMPTY; cell->val = 0; cell->is_num = 0; cell->err = ERR_OK; cell->state = ST_DONE; return; }

    cell->state = ST_INPROG; cell->err = ERR_OK;
    if (cell->raw[0] == '=') {
        cell->kind = K_FORMULA;
        if (++eval_depth > EVAL_MAX_DEPTH) { cell->err = ERR_CIRC; cell->is_num = 0; cell->val = 0; eval_depth--; cell->state = ST_DONE; return; }
        const char *save_cur = pcur; int save_err = perr;   /* protect the caller's parse state */
        pcur = cell->raw + 1; perr = ERR_OK;
        double v = eval_compare(); skipws();
        if (*pcur) perr = ERR_SYNTAX;                        /* trailing junk */
        if (perr) { cell->err = (unsigned char)perr; cell->is_num = 0; cell->val = 0; }
        else { cell->is_num = 1; cell->val = v; }            /* NaN/Inf from real math still shows */
        pcur = save_cur; perr = save_err;
        eval_depth--;
    } else {
        double v;
        if (cell_is_number(cell->raw, &v)) { cell->kind = K_NUM; cell->val = v; cell->is_num = 1; }
        else { cell->kind = K_TEXT; cell->val = 0; cell->is_num = 0; }
    }
    cell->state = ST_DONE;
}

static void recompute(void) {
    for (int r = 0; r < NROWS; r++) for (int c = 0; c < NCOLS; c++) cells[r][c].state = ST_UNVISITED;
    eval_depth = 0; pcur = ""; perr = ERR_OK;
    for (int r = 0; r < NROWS; r++) for (int c = 0; c < NCOLS; c++)
        if (cells[r][c].raw[0]) eval_cell(r, c);
}

/* Parse a whole string as a cell ref (for ':'-goto and the file format). */
static int parse_whole_ref(const char *s, int *rr, int *cc) {
    while (*s == ' ') s++;
    if (!is_alpha(*s)) return 0;
    int col = up(*s) - 'A'; s++;
    if (!is_digit(*s)) return 0;
    int row = 0; while (is_digit(*s)) { row = row * 10 + (*s - '0'); s++; }
    while (*s == ' ') s++;
    if (*s) return 0;
    row--;
    if (col < 0 || col >= NCOLS || row < 0 || row >= NROWS) return 0;
    *rr = row; *cc = col; return 1;
}

/* ---- number formatting (pure; shared by the UI and the host test) ---------*/
/* Format v with exactly `dec` fractional digits into out; returns length. */
static int fmt_fixed(double v, int dec, char *out) {
    int p = 0;
    if (v < 0) { out[p++] = '-'; v = -v; }
    if (v >= 9.2e18) { const char *s = dnum_to_str(v); int i = 0; while (s[i]) out[p++] = s[i++]; out[p] = 0; return p; }
    unsigned long long ip = (unsigned long long)v;
    double frac = v - (double)ip;
    double scale = 1; for (int i = 0; i < dec; i++) scale *= 10.0;
    unsigned long long fr = (unsigned long long)(frac * scale + 0.5);
    if (dec > 0 && fr >= (unsigned long long)scale) { ip++; fr = 0; }   /* rounding carry */
    char tmp[24]; int tn = 0;
    if (ip == 0) tmp[tn++] = '0'; else while (ip) { tmp[tn++] = (char)('0' + ip % 10); ip /= 10; }
    while (tn) out[p++] = tmp[--tn];
    if (dec > 0) {
        out[p++] = '.';
        char fb[24]; int fn = 0;
        for (int i = 0; i < dec; i++) { fb[fn++] = (char)('0' + fr % 10); fr /= 10; }
        while (fn) out[p++] = fb[--fn];
    }
    out[p] = 0; return p;
}

/* Format a numeric value to fit `w` columns; returns a static buffer. */
static const char *fmt_value(double v, int w) {
    static char buf[48];
    int i;
    if (js_isnan(v)) { buf[0] = 'N'; buf[1] = 'a'; buf[2] = 'N'; buf[3] = 0; return buf; }
    if (!js_isfinite(v)) { const char *s = v < 0 ? "-inf" : "inf"; for (i = 0; s[i]; i++) buf[i] = s[i]; buf[i] = 0; return buf; }
    const char *s = dnum_to_str(v);                      /* shortest round-trip form */
    int sl = 0; while (s[sl]) sl++;
    if (sl <= w) { for (i = 0; i <= sl; i++) buf[i] = s[i]; return buf; }
    for (int dec = w - 2; dec >= 0; dec--) {             /* too wide: try fewer decimals */
        int n = fmt_fixed(v, dec, buf);
        if (n <= w) return buf;
    }
    for (i = 0; i < w; i++) buf[i] = '#';                /* still too wide: Excel-style overflow */
    buf[w] = 0; return buf;
}

#endif /* SHEETEVAL_H */
