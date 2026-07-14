/* ploteval.h — a tiny expression evaluator with a variable `x`, for the graphing
 * calculator (user/plot.c, M1700). Pure (no syscalls), so it is host-unit-tested
 * by tests/plot exactly like calc's calceval.h and the spreadsheet's sheeteval.h.
 * It reuses dmath.h (the from-scratch IEEE-754 math copied verbatim from the JS
 * engine), so it needs SSE + -fwrapv, like calc/sheet.
 *
 * The plotter evaluates y = f(x) once per screen column, so `plot_eval` re-parses
 * the (short) formula each call — same model as calc/sheet, simple and fast
 * enough (the cost is dmath's transcendental functions, not the parse).
 *
 * Grammar:  expr := term (('+'|'-') term)* ;  term := power (('*'|'/'|'%') power)* ;
 *           power := factor ('^' power)? ;
 *           factor := number | '(' expr ')' | ('-'|'+') factor | name '(' expr ')'
 *                   | name        (the variable x, or the constants pi / e)
 * Functions: sin cos tan asin acos atan sqrt abs ln log log2 log10 exp floor ceil round trunc sign
 *            sinh cosh tanh cbrt  (hyperbolics/cube-root, matching the calculator)
 *            + the two-argument min(a,b) max(a,b) hypot(a,b) atan2(y,x)
 */
#ifndef PLOTEVAL_H
#define PLOTEVAL_H

#include "dmath.h"      /* js_sin/js_ln/js_pow/... (verbatim from js.c) */

static double      pe_x;      /* the variable `x` (set per evaluation) */
static const char *pe_cur;    /* parse cursor */
static int         pe_err;    /* set nonzero on a syntax/parse error */
static int         pe_angle_deg = 0;   /* 0 = radians (default), 1 = degrees; set by plot.c's :deg/:rad (M1744). Affects only trig. */

static double pe_expr(void);
static double pe_power(void);   /* forward decl: pe_unary() (looser than ^) calls pe_power() (M1784) */

static int  pe_isdigit(char c) { return c >= '0' && c <= '9'; }
static int  pe_isalpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static char pe_lc(char c)      { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static void pe_ws(void)        { while (*pe_cur == ' ' || *pe_cur == '\t') pe_cur++; }

/* case-insensitive match of the n-char identifier at n against keyword kw */
static int pe_kw(const char *id, int n, const char *kw) {
    int i = 0;
    for (; i < n && kw[i]; i++) if (pe_lc(id[i]) != kw[i]) return 0;
    return i == n && kw[i] == 0;
}

/* scan a decimal number (12, 3.14, .5, 1e3, 1.5e-2) at pe_cur */
static double pe_num(void) {
    double v = 0; int any = 0;
    while (pe_isdigit(*pe_cur)) { v = v * 10 + (*pe_cur - '0'); pe_cur++; any = 1; }
    if (*pe_cur == '.') { pe_cur++; double f = 0.1; while (pe_isdigit(*pe_cur)) { v += (*pe_cur - '0') * f; f *= 0.1; pe_cur++; any = 1; } }
    if (any && (*pe_cur == 'e' || *pe_cur == 'E')) {
        const char *s = pe_cur; pe_cur++; int neg = 0;
        if (*pe_cur == '+') pe_cur++; else if (*pe_cur == '-') { neg = 1; pe_cur++; }
        int e = 0, ed = 0; while (pe_isdigit(*pe_cur)) { e = e * 10 + (*pe_cur - '0'); pe_cur++; ed = 1; }
        if (ed) v *= js_pow(10.0, neg ? -(double)e : (double)e); else pe_cur = s;
    }
    if (!any) pe_err = 1;
    return v;
}

/* atan2(y,x) from js_atan (dmath has no atan2) with the quadrant fix-up. */
static double pe_atan2(double y, double x) {
    const double PI = 3.14159265358979;
    if (x > 0) return js_atan(y / x);
    if (x < 0) return y >= 0 ? js_atan(y / x) + PI : js_atan(y / x) - PI;
    if (y > 0) return PI / 2;
    if (y < 0) return -PI / 2;
    return 0;
}
/* degree<->radian conversion honouring pe_angle_deg (no-ops in radian mode):
 * pe_a2r converts a trig ARGUMENT to radians, pe_r2a a trig RESULT back to the
 * current angle unit. Mirrors calc's calc_a2r/calc_r2a (M1738/M1744). */
static double pe_a2r(double a) { return pe_angle_deg ? a * 3.14159265358979 / 180.0 : a; }
static double pe_r2a(double r) { return pe_angle_deg ? r * 180.0 / 3.14159265358979 : r; }
static double pe_factor(void) {
    pe_ws();
    if (*pe_cur == '(') { pe_cur++; double v = pe_expr(); pe_ws(); if (*pe_cur == ')') pe_cur++; else pe_err = 1; return v; }
    /* unary -/+ moved to pe_unary() (M1784) so ^ binds tighter: -2^2 == -(2^2) */
    if (pe_isdigit(*pe_cur) || *pe_cur == '.') return pe_num();
    if (pe_isalpha(*pe_cur)) {
        const char *s = pe_cur; int n = 0;
        while (pe_isalpha(*pe_cur) || pe_isdigit(*pe_cur)) { pe_cur++; n++; }
        pe_ws();
        if (*pe_cur == '(') {                        /* function call */
            pe_cur++; double a = pe_expr(); pe_ws();
            double b = 0; int two = 0;
            if (*pe_cur == ',') { pe_cur++; b = pe_expr(); pe_ws(); two = 1; }   /* optional 2nd arg */
            if (*pe_cur == ')') pe_cur++; else pe_err = 1;
            if (two) {                               /* two-argument functions (M1729) */
                if (pe_kw(s, n, "min"))   return a < b ? a : b;
                if (pe_kw(s, n, "max"))   return a > b ? a : b;
                if (pe_kw(s, n, "hypot")) return js_sqrt(a * a + b * b);
                if (pe_kw(s, n, "atan2")) return pe_r2a(pe_atan2(a, b));
                pe_err = 1; return 0;
            }
            if (pe_kw(s, n, "sin"))   return js_sin(pe_a2r(a));
            if (pe_kw(s, n, "cos"))   return js_cos(pe_a2r(a));
            if (pe_kw(s, n, "tan"))   return js_tan(pe_a2r(a));
            if (pe_kw(s, n, "asin"))  return pe_r2a(js_asin(a));
            if (pe_kw(s, n, "acos"))  return pe_r2a(js_acos(a));
            if (pe_kw(s, n, "atan"))  return pe_r2a(js_atan(a));
            if (pe_kw(s, n, "sqrt"))  return js_sqrt(a);
            if (pe_kw(s, n, "abs"))   return js_fabs(a);
            if (pe_kw(s, n, "ln"))    return js_ln(a);
            if (pe_kw(s, n, "log"))   return js_ln(a) / js_ln(10.0);
            if (pe_kw(s, n, "log2"))  return js_ln(a) / js_ln(2.0);
            if (pe_kw(s, n, "exp"))   return js_exp(a);
            if (pe_kw(s, n, "floor")) return js_floor(a);
            if (pe_kw(s, n, "ceil"))  return js_ceil(a);
            if (pe_kw(s, n, "round")) return js_round(a);
            if (pe_kw(s, n, "sign"))  return a > 0 ? 1.0 : a < 0 ? -1.0 : 0.0;
            if (pe_kw(s, n, "sinh"))  return (js_exp(a) - js_exp(-a)) * 0.5;                        /* hyperbolics: real arg, not DEG-converted (M1836) */
            if (pe_kw(s, n, "cosh"))  return (js_exp(a) + js_exp(-a)) * 0.5;
            if (pe_kw(s, n, "tanh"))  { double x = a < 0 ? -a : a, e = js_exp(-2.0 * x), t = (1.0 - e) / (1.0 + e); return a < 0 ? -t : t; }
            if (pe_kw(s, n, "cbrt"))  { double r = js_pow(a < 0 ? -a : a, 1.0 / 3.0); return a < 0 ? -r : r; }   /* sign-aware real cube root */
            if (pe_kw(s, n, "log10")) return js_ln(a) / js_ln(10.0);
            if (pe_kw(s, n, "trunc")) return js_trunc(a);
            pe_err = 1; return 0;
        }
        if (pe_kw(s, n, "x"))  return pe_x;           /* the variable */
        if (pe_kw(s, n, "pi")) return 3.14159265358979;
        if (pe_kw(s, n, "e"))  return 2.71828182845905;
        pe_err = 1; return 0;                         /* unknown name */
    }
    pe_err = 1; return 0;
}

static double pe_unary(void) {                        /* unary -/+ : looser than ^ (so -2^2 == -(2^2)), exponent stays unary (2^-2) — M1784 */
    pe_ws();
    if (*pe_cur == '-') { pe_cur++; return -pe_unary(); }
    if (*pe_cur == '+') { pe_cur++; return pe_unary(); }
    return pe_power();
}
static double pe_power(void) {                        /* right-associative ^, binds tighter than unary and * / % */
    double b = pe_factor(); pe_ws();
    if (*pe_cur == '^') { pe_cur++; double e = pe_unary(); return js_pow(b, e); }   /* M1784: exponent is a unary-expr, so 2^-2 works */
    return b;
}
static double pe_term(void) {
    double v = pe_unary();
    for (;;) { pe_ws();
        if (*pe_cur == '*') { pe_cur++; v *= pe_unary(); }
        else if (*pe_cur == '/') { pe_cur++; v /= pe_unary(); }
        else if (*pe_cur == '%') { pe_cur++; v = js_fmod(v, pe_unary()); }
        else break;
    }
    return v;
}
static double pe_expr(void) {
    double v = pe_term();
    for (;;) { pe_ws();
        if (*pe_cur == '+') { pe_cur++; v += pe_term(); }
        else if (*pe_cur == '-') { pe_cur++; v -= pe_term(); }
        else break;
    }
    return v;
}

/* Evaluate `expr` with the variable x = xval. *err (if non-NULL) is set nonzero
 * on a syntax error or trailing junk. NaN/Inf from real math pass through. */
static double plot_eval(const char *expr, double xval, int *err) {
    pe_x = xval; pe_cur = expr; pe_err = 0;
    double v = pe_expr(); pe_ws();
    if (*pe_cur) pe_err = 1;                           /* trailing junk */
    if (err) *err = pe_err;
    return v;
}

/* Definite integral of `expr` over [a,b] by composite Simpson's rule with n
 * subintervals (forced even). *err is set nonzero if any sample is a parse
 * error or non-finite (e.g. a singularity in range) — the integral is then
 * undefined. Pure; drives the plotter's :int and is host-tested by tests/plot. */
static double plot_integral(const char *expr, double a, double b, int n, int *err) {
    if (n < 2) n = 2;
    if (n & 1) n++;                                    /* Simpson needs an even count */
    double h = (b - a) / (double)n, sum = 0;
    for (int i = 0; i <= n; i++) {
        int e; double y = plot_eval(expr, a + (double)i * h, &e);
        if (e || js_isnan(y) || !js_isfinite(y)) { if (err) *err = 1; return 0; }
        double w = (i == 0 || i == n) ? 1.0 : (i & 1) ? 4.0 : 2.0;
        sum += w * y;
    }
    if (err) *err = 0;
    return sum * h / 3.0;
}

/* Numeric derivative f'(x) by the symmetric central difference
 * (f(x+h) - f(x-h)) / 2h. *err is set nonzero if either sample is a parse error
 * or non-finite. Pure; drives the plotter's :der overlay, host-tested. */
static double plot_derivative(const char *expr, double x, double h, int *err) {
    int e1, e2;
    double f1 = plot_eval(expr, x + h, &e1);
    double f2 = plot_eval(expr, x - h, &e2);
    if (e1 || e2 || js_isnan(f1) || js_isnan(f2) || !js_isfinite(f1) || !js_isfinite(f2)) {
        if (err) *err = 1; return 0;
    }
    if (err) *err = 0;
    return (f1 - f2) / (2.0 * h);
}

/* Find the real roots (zeros) of `expr` in [a,b]: sample `samples` sub-intervals
 * and, wherever f changes sign across a step (or hits exactly 0 at a sample),
 * bisect to refine one root. Roots are written ascending into out[0..ret-1], up
 * to `maxroots`. A step whose either end is non-finite (a pole/discontinuity) is
 * skipped — a sign flip across a pole is not a root. Returns the count found.
 * Pure; drives the plotter's :root and is host-tested by tests/plot. */
static int plot_find_roots(const char *expr, double a, double b, int samples,
                           double *out, int maxroots) {
    int n = 0;
    if (samples < 2) samples = 2;
    if (maxroots <= 0) return 0;
    double h = (b - a) / (double)samples;
    double span = b - a; if (span < 0) span = -span;
    double tol = span * 1e-6;               /* de-dup adjacent hits within this */
    int prev_ok = 0; double prev_x = a, prev_y = 0;
    for (int i = 0; i <= samples && n < maxroots; i++) {
        double x = a + (double)i * h;
        int e; double y = plot_eval(expr, x, &e);
        int ok = !(e || js_isnan(y) || !js_isfinite(y));
        if (ok && y == 0.0) {                                   /* exact zero at a sample */
            if (n == 0 || (x - out[n - 1]) > tol) out[n++] = x;
        } else if (ok && prev_ok && ((prev_y < 0 && y > 0) || (prev_y > 0 && y < 0))) {
            double lo = prev_x, hi = x, ylo = prev_y;           /* bisect the sign-change bracket */
            for (int it = 0; it < 60; it++) {
                double mid = (lo + hi) * 0.5;
                int e2; double ym = plot_eval(expr, mid, &e2);
                if (e2 || js_isnan(ym) || !js_isfinite(ym)) break;
                if ((ylo < 0 && ym < 0) || (ylo > 0 && ym > 0)) { lo = mid; ylo = ym; }
                else hi = mid;
            }
            double r = (lo + hi) * 0.5;
            if (n == 0 || (r - out[n - 1]) > tol) out[n++] = r;
        }
        prev_ok = ok; prev_x = x; prev_y = y;
    }
    return n;
}

#endif /* PLOTEVAL_H */
