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
 *              MEDIAN MODE (variadic, over ranges)
 *              (variadic, take ranges);  SUMIF/COUNTIF/AVERAGEIF(range, [op]value)
 *              where op is = <> < <= > >= (bare value means "=");  IF(c,a[,b])
 *              AND/OR(...) NOT(x)  (logical);
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

/* Parse a cell reference (letter + digits, e.g. B12) at pcur, with optional '$'
 * absolute markers ($A$1 / $A1 / A$1 -- pinning has no effect on the VALUE, only
 * on how copy/paste/fill shift the ref, see adjust_refs). On success advance
 * pcur, fill rr and cc (0-based) and return 1; else leave pcur untouched, return 0. */
static int parse_ref_cursor(int *rr, int *cc) {
    const char *p = pcur;
    if (*p == '$') p++;                                 /* optional $ absolute column */
    if (!is_alpha(*p)) return 0;
    int col = up(*p) - 'A'; p++;
    if (*p == '$') p++;                                 /* optional $ absolute row */
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
            } else if (*pcur == ',' || *pcur == ')') {  /* M1760: a lone BARE ref -> treat exactly like a 1-cell range, so an empty/text cell is SKIPPED (COUNT/SUM/AVG/MIN/MAX/PRODUCT), not folded in as a phantom 0 the way the range form A1:A1 already skips it */
                eval_cell(r1, c1);
                cell_t *cell = CELL(r1, c1);
                if (cell->raw[0]) a->nonempty++;
                if (cell->err) perr = cell->err;
                else if (cell->is_num) agg_add(a, cell->val);
            } else {                                    /* the ref begins a larger scalar expression (e.g. A1+1): evaluate the whole thing */
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

/* Collect the numeric values of range [r1,c1]-[r2,c2] into buf (up to cap). */
static void collect_range_vals(int r1, int c1, int r2, int c2, double *buf, int cap, int *n) {
    if (r1 > r2) { int t = r1; r1 = r2; r2 = t; }
    if (c1 > c2) { int t = c1; c1 = c2; c2 = t; }
    for (int r = r1; r <= r2; r++) for (int c = c1; c <= c2; c++) {
        eval_cell(r, c);
        cell_t *cell = CELL(r, c);
        if (cell->err) perr = cell->err;
        else if (cell->is_num && *n < cap) buf[(*n)++] = cell->val;
    }
}
/* Parse a function arg list (ranges + scalar expressions) collecting every
 * numeric value into buf (<= cap). Mirrors parse_agg_args but keeps the actual
 * values — for MEDIAN/MODE, which need the distribution, not just running sums.
 * buf is the caller's (usually stack), so nested MEDIAN/MODE calls don't clash. */
static void collect_agg_values(double *buf, int cap, int *n) {
    *n = 0;
    skipws();
    if (*pcur == ')') { pcur++; return; }
    for (;;) {
        skipws();
        const char *save = pcur;
        int r1, c1, r2, c2;
        if (parse_ref_cursor(&r1, &c1)) {
            skipws();
            if (*pcur == ':') {
                pcur++; skipws();
                if (parse_ref_cursor(&r2, &c2)) collect_range_vals(r1, c1, r2, c2, buf, cap, n);
                else perr = ERR_SYNTAX;
            } else { pcur = save; double v = eval_compare(); if (*n < cap) buf[(*n)++] = v; }
        } else { pcur = save; double v = eval_compare(); if (*n < cap) buf[(*n)++] = v; }
        skipws();
        if (*pcur == ',') { pcur++; continue; }
        if (*pcur == ')') { pcur++; break; }
        perr = ERR_SYNTAX; break;
    }
}
static void dsort(double *v, int n) {                  /* insertion sort (n is small: <= a column or two) */
    for (int i = 1; i < n; i++) { double k = v[i]; int j = i - 1; while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; } v[j + 1] = k; }
}

/* Dispatch a function by name; '(' has already been consumed. */
static double call_function(const char *name) {
    if (nameeq(name, "MEDIAN") || nameeq(name, "MODE")) {       /* need the sorted value distribution */
        double vals[512]; int nv;
        collect_agg_values(vals, 512, &nv);
        if (nv <= 0) return 0;
        dsort(vals, nv);
        if (nameeq(name, "MEDIAN"))
            return (nv & 1) ? vals[nv / 2] : (vals[nv / 2 - 1] + vals[nv / 2]) / 2.0;
        double best = vals[0]; int bestc = 1, curc = 1;         /* MODE: value of the longest equal run */
        for (int i = 1; i < nv; i++) {
            if (vals[i] == vals[i - 1]) curc++; else curc = 1;
            if (curc > bestc) { bestc = curc; best = vals[i]; }
        }
        return best;
    }
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

    /* conditional aggregates: SUMIF/COUNTIF/AVERAGEIF(range, criterion), where the
     * criterion is an optional comparison operator (= <> < <= > >=) then a value
     * (a bare value means "= value"), e.g. SUMIF(A1:A9, ">3"). */
    if (nameeq(name, "SUMIF") || nameeq(name, "COUNTIF") || nameeq(name, "AVERAGEIF")) {
        skipws();
        int r1, c1, r2, c2;
        if (!parse_ref_cursor(&r1, &c1)) { perr = ERR_SYNTAX; return 0; }
        skipws();
        if (*pcur == ':') { pcur++; skipws(); if (!parse_ref_cursor(&r2, &c2)) { perr = ERR_SYNTAX; return 0; } }
        else { r2 = r1; c2 = c1; }
        skipws();
        if (*pcur != ',') { perr = ERR_SYNTAX; return 0; }
        pcur++; skipws();
        int op = 0;                                          /* 0 '=' 1 '<' 2 '<=' 3 '>' 4 '>=' 5 '<>' */
        if (*pcur == '<') { pcur++; if (*pcur == '=') { pcur++; op = 2; } else if (*pcur == '>') { pcur++; op = 5; } else op = 1; }
        else if (*pcur == '>') { pcur++; if (*pcur == '=') { pcur++; op = 4; } else op = 3; }
        else if (*pcur == '=') { pcur++; op = 0; }
        double thr = eval_expr();                            /* the threshold value */
        skipws();
        if (*pcur == ')') pcur++; else perr = ERR_SYNTAX;
        if (r1 > r2) { int t = r1; r1 = r2; r2 = t; }
        if (c1 > c2) { int t = c1; c1 = c2; c2 = t; }
        double sum = 0; int cnt = 0;
        for (int r = r1; r <= r2; r++) for (int c = c1; c <= c2; c++) {
            eval_cell(r, c); cell_t *cell = CELL(r, c);
            if (cell->err) perr = cell->err;
            if (!cell->is_num) continue;
            double v = cell->val; int m = 0;
            switch (op) {
                case 0: m = (v == thr); break; case 1: m = (v < thr); break; case 2: m = (v <= thr); break;
                case 3: m = (v > thr); break; case 4: m = (v >= thr); break; case 5: m = (v != thr); break;
            }
            if (m) { sum += v; cnt++; }
        }
        if (nameeq(name, "COUNTIF"))   return (double)cnt;
        if (nameeq(name, "AVERAGEIF")) return cnt ? sum / (double)cnt : 0;
        return sum;                                          /* SUMIF */
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
    /* cell reference, optionally $-anchored ($A$1 / $A1 / A$1 / A1) -- tried
     * before the name lexer. parse_ref_cursor rejects function/constant names
     * (a letter not followed by a digit), so SUM( and PI/E still fall through. */
    if (*pcur == '$' || is_alpha(*pcur)) {
        const char *save = pcur;
        int rr, cc;
        if (parse_ref_cursor(&rr, &cc)) {
            skipws();
            if (*pcur != '(') return get_cell_value(rr, cc);
        }
        pcur = save;                                    /* a function/constant name, not a bare ref */
    }
    if (is_alpha(*pcur)) {
        char name[16]; int nl = 0;
        while ((is_alpha(*pcur) || is_digit(*pcur)) && nl < 15) name[nl++] = *pcur++;
        name[nl] = 0;
        if (*pcur == '(') { pcur++; return call_function(name); }
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

/* Parse "A1:B10" (or a single "A1") into an inclusive, normalised rect
 * (r1<=r2, c1<=c2), all 0-based. Returns 1 on success, 0 on malformed input or
 * an out-of-range endpoint. Used by the :chart command. */
static int parse_range(const char *s, int *r1o, int *c1o, int *r2o, int *c2o) {
    while (*s == ' ') s++;
    if (!is_alpha(*s)) return 0;
    int c1 = up(*s) - 'A'; s++;
    if (!is_digit(*s)) return 0;
    int r1 = 0; while (is_digit(*s)) { r1 = r1 * 10 + (*s - '0'); s++; }
    r1--;
    int r2 = r1, c2 = c1;
    while (*s == ' ') s++;
    if (*s == ':') {
        s++; while (*s == ' ') s++;
        if (!is_alpha(*s)) return 0;
        c2 = up(*s) - 'A'; s++;
        if (!is_digit(*s)) return 0;
        r2 = 0; while (is_digit(*s)) { r2 = r2 * 10 + (*s - '0'); s++; }
        r2--;
        while (*s == ' ') s++;
    }
    if (*s) return 0;                                   /* trailing junk */
    if (c1 < 0 || c1 >= NCOLS || r1 < 0 || r1 >= NROWS ||
        c2 < 0 || c2 >= NCOLS || r2 < 0 || r2 >= NROWS) return 0;
    if (r1 > r2) { int t = r1; r1 = r2; r2 = t; }
    if (c1 > c2) { int t = c1; c1 = c2; c2 = t; }
    *r1o = r1; *c1o = c1; *r2o = r2; *c2o = c2; return 1;
}

/* Rewrite a formula/text `src` into `out`, shifting every relative cell
 * reference by (dr, dc) rows/cols, clamped to the grid — the ref-adjustment
 * behind copy/paste ("fill a formula down": yanking =A1+B1 from D1 and pasting
 * into D2 gives =A2+B2). This mirrors the evaluator's lexer exactly so it only
 * touches genuine references: a number literal (incl. an exponent like 1e5) is
 * copied verbatim so its 'e5' is never mistaken for a ref, an identifier
 * immediately followed by '(' is a function name (SUM, IF), a multi-letter or
 * lone-letter identifier is a name/constant (PI, E) — only a single column
 * letter followed by digits (A1, Z100), and not a function call, is a ref. A '$'
 * before the column and/or row ($A$1, $A1, A$1) anchors that part so it does NOT
 * shift (adjust_one_ref handles these first, preserving the '$'). Pure. */
/* Parse a $-anchored-or-plain cell reference ($?[A-Z]$?[0-9]+, not a function
 * call) at *pp. On a match, emit it into out shifted by (dr,dc) -- but a
 * $-anchored column or row is NOT shifted (that is the whole point of $) and its
 * '$' is preserved -- advance *pp and *po, return 1; else touch nothing, 0. */
static int adjust_one_ref(const char **pp, int dr, int dc, char *out, int *po, int max) {
    const char *p = *pp;
    int abscol = 0, absrow = 0;
    if (*p == '$') { abscol = 1; p++; }
    if (!is_alpha(*p)) return 0;
    int col = up(*p) - 'A'; p++;
    if (*p == '$') { absrow = 1; p++; }
    if (!is_digit(*p)) return 0;
    int row = 0; while (is_digit(*p)) { row = row * 10 + (*p - '0'); p++; }
    row--;
    if (is_alpha(*p) || *p == '(') return 0;            /* "A1B" / a function call is not a ref */
    if (col < 0 || col >= NCOLS || row < 0 || row >= NROWS) return 0;
    int nc = abscol ? col : col + dc, nr = absrow ? row : row + dr;
    if (nc < 0) nc = 0; if (nc >= NCOLS) nc = NCOLS - 1;
    if (nr < 0) nr = 0; if (nr >= NROWS) nr = NROWS - 1;
    int o = *po;
    if (abscol && o < max - 1) out[o++] = '$';
    if (o < max - 1) out[o++] = (char)('A' + nc);
    if (absrow && o < max - 1) out[o++] = '$';
    char d[8]; int dn = 0, rr = nr + 1;
    while (rr) { d[dn++] = (char)('0' + rr % 10); rr /= 10; }
    while (dn && o < max - 1) out[o++] = d[--dn];
    *po = o; *pp = p; return 1;
}
static void adjust_refs(const char *src, int dr, int dc, char *out, int max) {
    int o = 0; const char *p = src;
    while (*p && o < max - 1) {
        if (adjust_one_ref(&p, dr, dc, out, &o, max)) continue;   /* $-aware cell ref (shifts only non-$ parts) */
        if (is_digit(*p) || (*p == '.' && is_digit(p[1]))) {     /* number literal — verbatim */
            while ((is_digit(*p) || *p == '.') && o < max - 1) out[o++] = *p++;
            if ((*p == 'e' || *p == 'E') &&
                (is_digit(p[1]) || ((p[1] == '+' || p[1] == '-') && is_digit(p[2])))) {
                if (o < max - 1) out[o++] = *p++;                /* e/E */
                if ((*p == '+' || *p == '-') && o < max - 1) out[o++] = *p++;
                while (is_digit(*p) && o < max - 1) out[o++] = *p++;
            }
            continue;
        }
        if (is_alpha(*p)) {                                      /* identifier: name, constant or ref */
            const char *s = p; char id[24]; int n = 0;
            while (is_alpha(*p) || is_digit(*p)) { if (n < 23) id[n++] = *p; p++; }
            id[n] = 0;
            int isref = (*p != '(' && is_alpha(id[0]) && !is_alpha(id[1]) && is_digit(id[1]));
            int col = 0, row = 0;
            if (isref) {
                col = up(id[0]) - 'A';
                for (int i = 1; id[i]; i++) { if (!is_digit(id[i])) { isref = 0; break; } row = row * 10 + (id[i] - '0'); }
                row--;
            }
            if (isref) {
                int nc = col + dc, nr = row + dr;
                if (nc < 0) nc = 0; if (nc >= NCOLS) nc = NCOLS - 1;
                if (nr < 0) nr = 0; if (nr >= NROWS) nr = NROWS - 1;
                if (o < max - 1) out[o++] = (char)('A' + nc);
                char d[8]; int dn = 0, rr = nr + 1;
                while (rr) { d[dn++] = (char)('0' + rr % 10); rr /= 10; }
                while (dn && o < max - 1) out[o++] = d[--dn];
            } else {
                for (const char *q = s; q < p && o < max - 1; q++) out[o++] = *q;   /* verbatim span */
            }
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

/* ---- row sort (pure; host-tested) — the :sort command ---------------------
 * Reorder the rows in [r0..r1] by the key column `keyc`, ascending (desc=0) or
 * descending (desc=1). The WHOLE row (every column) moves as a unit and each
 * moved formula's relative refs are shifted by its row delta via adjust_refs, so
 * a self-contained table's intra-row formulas stay correct after the sort — e.g.
 * a Total column of =B2+C2, =B3+C3, … keeps computing each row's own B+C once
 * the rows are rearranged. (This engine has no absolute refs, so a formula that
 * points OUTSIDE the sorted rows is not tracked.) Empty key cells sort last in
 * both directions; numeric keys order before text; the sort is stable.
 * recompute() must have run (keys read computed values); the caller recomputes
 * again afterwards. */
static char sort_snap[NROWS][NCOLS][RAWMAX];    /* scratch: raw-text snapshot of the block being sorted (BSS) */
static int  sort_perm[NROWS];                   /* the sorted order of absolute row indices */

static int sort_scmp(const char *a, const char *b) {            /* lexicographic byte compare */
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}
/* Ascending order of two NON-empty key cells: a number sorts before text;
 * numbers by value; text lexicographically. (Empties handled by sort_order.) */
static int sort_key_cmp(int ra, int rb, int keyc) {
    cell_t *a = CELL(ra, keyc), *b = CELL(rb, keyc);
    if (a->is_num && b->is_num) return a->val < b->val ? -1 : a->val > b->val ? 1 : 0;
    if (a->is_num != b->is_num) return a->is_num ? -1 : 1;
    return sort_scmp(a->raw, b->raw);
}
/* Full ordering: an empty key cell always sorts last (stable when both empty);
 * two non-empty cells compare ascending, negated for descending. */
static int sort_order(int ra, int rb, int keyc, int desc) {
    int ea = (CELL(ra, keyc)->raw[0] == 0), eb = (CELL(rb, keyc)->raw[0] == 0);
    if (ea || eb) return (ea ? 1 : 0) - (eb ? 1 : 0);
    int c = sort_key_cmp(ra, rb, keyc);
    return desc ? -c : c;
}
/* Returns 1 if it actually reordered rows, 0 if the block was already sorted or
 * the range was degenerate (M1767: lets the caller avoid a spurious dirty flag /
 * "sorted" status / wasted undo on a no-op :sort). */
static int sort_rows(int r0, int r1, int keyc, int desc) {
    if (r0 < 0) r0 = 0;
    if (r1 >= NROWS) r1 = NROWS - 1;
    if (r0 >= r1 || keyc < 0 || keyc >= NCOLS) return 0;        /* nothing to do */
    int n = r1 - r0 + 1;
    for (int i = 0; i < n; i++) sort_perm[i] = r0 + i;
    for (int i = 1; i < n; i++) {                               /* stable insertion sort */
        int key = sort_perm[i], j = i - 1;
        while (j >= 0 && sort_order(sort_perm[j], key, keyc, desc) > 0) { sort_perm[j + 1] = sort_perm[j]; j--; }
        sort_perm[j + 1] = key;
    }
    int changed = 0; for (int i = 0; i < n; i++) if (sort_perm[i] != r0 + i) { changed = 1; break; }
    if (!changed) return 0;                                     /* already in order -> a true no-op (no cell rewrite) */
    for (int r = r0; r <= r1; r++)                              /* snapshot raw text before overwriting */
        for (int c = 0; c < NCOLS; c++) scopy(sort_snap[r][c], CELL(r, c)->raw, RAWMAX);
    for (int i = 0; i < n; i++) {
        int src = sort_perm[i], dst = r0 + i, dr = dst - src;
        for (int c = 0; c < NCOLS; c++)
            if (dr == 0) scopy(CELL(dst, c)->raw, sort_snap[src][c], RAWMAX);
            else         adjust_refs(sort_snap[src][c], dr, 0, CELL(dst, c)->raw, RAWMAX);
    }
    return 1;
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
    if (fr >= (unsigned long long)scale) { ip++; fr = 0; }   /* rounding carry (incl. dec==0: 3.7 -> 4) */
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

/* Format v for a column with an explicit display format `fmt`:
 *   'G' or 0 -> general (fmt_value's auto width-fit);  '0'..'6' -> fixed N
 *   decimals;  '%' -> percent (value*100, 1 dp, trailing '%');  '$' -> currency
 *   ('$' + 2 dp, sign ahead of the '$'). Clamped to `w` columns ('#'-filled if it
 *   won't fit), like fmt_value. Pure; host-tested. */
static const char *fmt_value_col(double v, int w, char fmt) {
    static char buf[48];
    if (fmt == 'G' || fmt == 0) return fmt_value(v, w);
    if (js_isnan(v) || !js_isfinite(v)) return fmt_value(v, w);
    int n = -1;
    if (fmt >= '0' && fmt <= '6') {
        n = fmt_fixed(v, fmt - '0', buf);
    } else if (fmt == '%') {
        n = fmt_fixed(v * 100.0, 1, buf);
        if (n >= 0 && n < 46) { buf[n++] = '%'; buf[n] = 0; }
    } else if (fmt == '$') {
        int p = 0; double a = v;
        if (v < 0) { buf[p++] = '-'; a = -v; }
        buf[p++] = '$';
        n = p + fmt_fixed(a, 2, buf + p);
    } else {
        return fmt_value(v, w);                          /* unknown code -> general */
    }
    if (n >= 0 && n <= w) return buf;
    for (int i = 0; i < w; i++) buf[i] = '#';            /* formatted value won't fit the column */
    buf[w] = 0; return buf;
}

/* ---- cell search (pure; host-tested) — the :find command ------------------*/
/* Case-insensitive substring test: does `hay` contain `needle`? ("" -> yes). */
static int ci_substr(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] && up(hay[i + j]) == up(needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}
/* A cell matches query `q` if `q` (case-insensitive) is a substring of its raw
 * text OR, for a numeric cell, of its displayed value (so `:find 270` locates a
 * formula cell that computes 270, not just a literal). */
static int cell_search_match(int r, int c, const char *q) {
    cell_t *cell = CELL(r, c);
    if (cell->raw[0] && ci_substr(cell->raw, q)) return 1;
    if (cell->is_num && ci_substr(fmt_value(cell->val, 40), q)) return 1;
    return 0;
}
/* Find the next cell (row-major, wrapping) strictly after (fr,fc) matching `q`.
 * Fills *ro,*co and returns 1 if found, else 0 (also 0 for an empty query). */
static int sheet_find(const char *q, int fr, int fc, int *ro, int *co) {
    if (!q[0]) return 0;
    int total = NROWS * NCOLS, start = fr * NCOLS + fc;
    for (int k = 1; k <= total; k++) {
        int idx = (start + k) % total, r = idx / NCOLS, c = idx % NCOLS;
        if (cell_search_match(r, c, q)) { *ro = r; *co = c; return 1; }
    }
    return 0;
}

/* ---- CSV import/export (pure; host-tested) --------------------------------
 * Interchange with real tools (RFC 4180). Export writes the computed VALUES
 * (numbers in dnum_to_str's shortest round-trip form; text verbatim), a field
 * quoted only when it contains a comma, quote or newline. Import fills cells
 * from A1 — the cell model then classifies each field (a bare number -> NUMBER,
 * an '='-prefixed field -> FORMULA, else TEXT). Formulas are NOT exported (CSV
 * has no formulas — the native format preserves those); a formula cell exports
 * its result, matching every real spreadsheet's CSV export. */

/* Used extent: rows/cols is one past the last row/column holding any raw text. */
static void sheet_extent(int *rows, int *cols) {
    int mr = 0, mc = 0;
    for (int r = 0; r < NROWS; r++)
        for (int c = 0; c < NCOLS; c++)
            if (CELL(r, c)->raw[0]) { if (r + 1 > mr) mr = r + 1; if (c + 1 > mc) mc = c + 1; }
    *rows = mr; *cols = mc;
}

/* Append one CSV field (RFC-4180 quoting) to out[p]; return the new length. */
static int csv_put_field(char *out, int p, int max, const char *s) {
    int needq = 0;
    for (const char *q = s; *q; q++) if (*q == ',' || *q == '"' || *q == '\n' || *q == '\r') { needq = 1; break; }
    if (!needq) { for (int i = 0; s[i] && p < max; i++) out[p++] = s[i]; return p; }
    if (p < max) out[p++] = '"';
    for (int i = 0; s[i] && p < max - 1; i++) {
        if (s[i] == '"') out[p++] = '"';                 /* escape a quote by doubling it */
        if (p < max) out[p++] = s[i];
    }
    if (p < max) out[p++] = '"';
    return p;
}

/* Serialize the sheet's used extent to CSV in out (<= max-1 bytes + NUL);
 * returns the byte length. recompute() must have run so values are current. */
static int sheet_to_csv(char *out, int max) {
    int rows, cols; sheet_extent(&rows, &cols);
    int p = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (c && p < max) out[p++] = ',';
            cell_t *cell = CELL(r, c);
            if (!cell->raw[0]) continue;                 /* empty -> empty field */
            if (cell->err) p = csv_put_field(out, p, max, cell->err == ERR_CIRC ? "#CIRC" : "#ERR");
            else if (cell->is_num) {
                const char *s = js_isnan(cell->val) ? "NaN"
                              : !js_isfinite(cell->val) ? (cell->val < 0 ? "-inf" : "inf")
                              : dnum_to_str(cell->val);
                p = csv_put_field(out, p, max, s);
            } else p = csv_put_field(out, p, max, cell->raw);   /* text */
        }
        if (p < max) out[p++] = '\n';
    }
    if (p > max - 1) p = max - 1;
    out[p] = 0; return p;
}

/* Read one CSV field at *pp into buf (<= bufmax-1 + NUL); advance past the field
 * and its delimiter. Returns 1 if a comma followed (more fields this record),
 * else 0 (a newline or end of input ended the record). */
static int csv_get_field(const char **pp, char *buf, int bufmax) {
    const char *p = *pp; int n = 0;
    if (*p == '"') {                                     /* quoted field */
        p++;
        for (;;) {
            if (*p == 0) break;
            if (*p == '"') {
                if (p[1] == '"') { if (n < bufmax - 1) buf[n++] = '"'; p += 2; continue; }
                p++; break;                              /* closing quote */
            }
            if (n < bufmax - 1) buf[n++] = *p;
            p++;
        }
        while (*p && *p != ',' && *p != '\n' && *p != '\r') p++;   /* skip to delimiter */
    } else {                                             /* unquoted field */
        while (*p && *p != ',' && *p != '\n' && *p != '\r') { if (n < bufmax - 1) buf[n++] = *p; p++; }
    }
    buf[n] = 0;
    int more = (*p == ',');
    if (*p == ',') p++;
    else { if (*p == '\r') p++; if (*p == '\n') p++; }   /* consume LF or CRLF */
    *pp = p; return more;
}

/* Replace the whole grid with the contents of a CSV string, placed from A1. */
static void sheet_from_csv(const char *in) {
    for (int r = 0; r < NROWS; r++) for (int c = 0; c < NCOLS; c++) CELL(r, c)->raw[0] = 0;
    const char *p = in; int r = 0;
    while (*p && r < NROWS) {
        int c = 0, more;
        do {
            char buf[RAWMAX];
            more = csv_get_field(&p, buf, RAWMAX);
            if (c < NCOLS) set_raw(r, c, buf);           /* extra columns past Z are parsed but dropped */
            c++;
        } while (more);
        r++;
    }
}

#endif /* SHEETEVAL_H */
