/* calceval.h — the calculator app's floating-point SCIENTIFIC expression
 * evaluator: recursive descent with C-like precedence — unary - ~, ^ (power,
 * right-assoc, binds tightest), * / %, + -, << >>, & , | (lowest) — plus
 * parentheses, decimal (3.14, .5, 1e3, 1.5e-2) and 0x-hex literals, the
 * constants pi/e, and the functions sqrt sin cos tan asin acos atan ln log
 * log2 log10 exp abs sign floor ceil round trunc, plus the two-argument
 * min(a,b) max(a,b) hypot(a,b) atan2(y,x). Results are IEEE-754 doubles (calc.o is built
 * with SSE). Note: calc uses ^ for POWER (not XOR) and has no relational/
 * logical operators, unlike the shell's $(()) evaluator (shmath.h).
 *
 * Semantics:
 *   - / is real division; 5/0 -> +Infinity, -5/0 -> -Infinity, 0/0 -> NaN
 *     (plain float math — NOT a CPU trap), so the evaluator never crashes.
 *   - % is js_fmod (sign of the dividend; x%0 -> NaN), ^ is js_pow.
 *   - The bitwise/shift operators (& | << >> ~) are integer ops: each operand
 *     is converted to (long) by truncation toward zero (à la JS ToInt32 but
 *     64-bit), the op is done, and the result converted back to double — so
 *     `5 & 3` -> 1.0, `1 << 4` -> 16.0. NaN/Inf operands truncate to 0.
 *   - log is base-10 (ln(x)/ln(10)); ln is natural log.
 *   - A bad/unknown function name, wrong arity (missing '('/')'), an unknown
 *     identifier, a bad token, an unmatched ')', or trailing junk set *err.
 *     Math that is merely undefined (sqrt(-1), ln(0), 1/0) yields NaN/Inf and
 *     does NOT set *err — only *structural* problems do.
 *
 * Pure (no syscalls), so it's host-unit-tested by tests/calc; user/calc.c
 * #includes it for the interactive calculator. dmath.h provides the math
 * (copied verbatim from the JS engine: sqrt=Newton, trig=range-reduced Taylor).
 *
 * The OS-authored userspace is built with -fwrapv (M781); the long bitwise
 * accumulation below therefore wraps (defined), and the host test is built the
 * same way. */
#ifndef CALCEVAL_H
#define CALCEVAL_H

#include "dmath.h"   /* js_pow, js_fmod, js_sqrt, js_sin, ... (verbatim from js.c) */

#ifndef LONG_MIN
#define LONG_MIN (-9223372036854775807L - 1)   /* freestanding: no limits.h */
#endif

static const char *cur;          /* the parse cursor */
static int err;
static double calc_ans = 0;       /* last result; the identifier `ans` reads it. The
                                   * evaluator stays pure — the caller sets this (see
                                   * user/calc.c: `calc_ans = r` after a successful eval)
                                   * so the next expression can chain off the prior one. */
static int calc_angle_deg = 0;    /* 0 = radians (default), 1 = degrees; set by user/calc.c's
                                   * DEG/RAD toggle. Affects only the trig functions: a degree
                                   * argument is converted to radians before sin/cos/tan, and an
                                   * asin/acos/atan/atan2 result is converted back to degrees. */
/* angle-arg -> radians (for sin/cos/tan) and radians-result -> angle (for the
 * inverse trig), honouring calc_angle_deg. No-ops in radian mode. */
static double calc_a2r(double a) { return calc_angle_deg ? a * 3.14159265358979 / 180.0 : a; }
static double calc_r2a(double r) { return calc_angle_deg ? r * 180.0 / 3.14159265358979 : r; }
static double bor(void);          /* the lowest-precedence level (bitwise OR) — the eval entry */
static double power(void);        /* forward decl: unary() (looser than ^) calls power() (M1784) */

static void skipws(void) { while (*cur == ' ' || *cur == '\t') cur++; }

/* Truncate a double to long for the bitwise/shift path (JS-ToInt32-style: a
 * non-finite operand becomes 0; a finite value truncates toward zero). */
static long to_long(double d) {
    if (!js_isfinite(d)) return 0;
    if (d >= 9.2e18) return 0x7fffffffffffffffL;   /* clamp out-of-range to avoid UB on the (long) cast */
    if (d <= -9.2e18) return LONG_MIN;
    return (long)d;
}

/* Match keyword `kw` at the cursor only when it is NOT followed by another
 * identifier character (so `pi` matches but `pizza` does not). Advances cur and
 * returns 1 on a match. */
static int is_idchar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
static int match_kw(const char *kw) {
    int i = 0;
    while (kw[i] && cur[i] == kw[i]) i++;
    if (kw[i] != 0) return 0;             /* did not fully match */
    if (is_idchar(cur[i])) return 0;      /* longer identifier — not this keyword */
    cur += i;
    return 1;
}

/* Parse a parenthesised single argument: '(' bor ')'. Sets err on a missing
 * paren and returns the inner value (or 0 with err set). */
static double call_arg(void) {
    skipws();
    if (*cur != '(') { err = 1; return 0; }
    cur++;
    double v = bor();
    skipws();
    if (*cur == ')') cur++; else err = 1;
    return v;
}

/* Parse a parenthesised two-argument list: '(' bor ',' bor ')'. Fills *a and *b;
 * sets err on a missing paren/comma. Used by the 2-arg functions (min/max/hypot/atan2). */
static void call_arg2(double *a, double *b) {
    *a = *b = 0;
    skipws();
    if (*cur != '(') { err = 1; return; }
    cur++;
    *a = bor();
    skipws();
    if (*cur == ',') cur++; else { err = 1; return; }
    *b = bor();
    skipws();
    if (*cur == ')') cur++; else err = 1;
}

/* Parse a variadic argument list '(' bor (',' bor)* ')' into out[0..max); returns
 * the count (which may exceed max — extras are counted but not stored). Sets err on
 * a missing paren. Used by the list statistics functions (sum/mean/stdev/…, M1827). */
static int call_argn(double *out, int max) {
    skipws();
    if (*cur != '(') { err = 1; return 0; }
    cur++;
    skipws();
    if (*cur == ')') { cur++; return 0; }            /* empty () */
    int n = 0;
    for (;;) {
        double v = bor();
        if (n < max) out[n] = v;
        n++;
        skipws();
        if (*cur == ',') { cur++; continue; }
        if (*cur == ')') { cur++; break; }
        err = 1; break;
    }
    return n;
}

/* atan2(y,x): the polar angle of (x,y) in (-pi,pi], built from js_atan (dmath has
 * no atan2) with the standard quadrant fix-up. */
static double calc_atan2(double y, double x) {
    const double PI = 3.14159265358979;
    if (x > 0) return js_atan(y / x);
    if (x < 0) return y >= 0 ? js_atan(y / x) + PI : js_atan(y / x) - PI;
    if (y > 0) return PI / 2;
    if (y < 0) return -PI / 2;
    return 0;
}

/* --- scientific helpers (M1812) --- */
/* factorial n! for a non-negative integer n; NaN for negative or non-integer.
 * n > 170 overflows a double, so the running product naturally becomes +Inf. */
static double calc_fact(double x) {
    if (!js_isfinite(x) || x < 0) return JS_NAN;
    double n = js_round(x);
    if (js_fabs(n - x) > 1e-9) return JS_NAN;    /* only defined on integers here */
    double r = 1;
    for (double i = 2; i <= n; i += 1) r *= i;
    return r;
}
/* nPr = n!/(n-r)! as the falling product n*(n-1)*...*(n-r+1). */
static double calc_perm(double nd, double rd) {
    if (!js_isfinite(nd) || !js_isfinite(rd)) return JS_NAN;
    double n = js_round(nd), r = js_round(rd);
    if (js_fabs(n - nd) > 1e-9 || js_fabs(r - rd) > 1e-9) return JS_NAN;
    if (n < 0 || r < 0 || r > n) return JS_NAN;
    double res = 1;
    for (double i = 0; i < r; i += 1) res *= (n - i);
    return res;
}
/* nCr via the multiplicative form res*(n-r+i)/i, which keeps the running value
 * small so ncr(100,50) doesn't overflow the way computing n! alone would. */
static double calc_comb(double nd, double rd) {
    if (!js_isfinite(nd) || !js_isfinite(rd)) return JS_NAN;
    double n = js_round(nd), r = js_round(rd);
    if (js_fabs(n - nd) > 1e-9 || js_fabs(r - rd) > 1e-9) return JS_NAN;
    if (n < 0 || r < 0 || r > n) return JS_NAN;
    if (r > n - r) r = n - r;                    /* symmetry: fewer terms, less overflow */
    double res = 1;
    for (double i = 1; i <= r; i += 1) res = res * (n - r + i) / i;
    return js_round(res);                        /* an exact integer; round off fp drift */
}
/* gcd/lcm on the integer (truncated toward zero, like the bitwise ops) magnitudes. */
static double calc_gcd(double ad, double bd) {
    long a = to_long(ad), b = to_long(bd);
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { long t = a % b; a = b; b = t; }
    return (double)a;
}
static double calc_lcm(double ad, double bd) {
    long a = to_long(ad), b = to_long(bd);
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    if (a == 0 || b == 0) return 0;
    long g = a, t = b; while (t) { long r = g % t; g = t; t = r; }   /* gcd(a,b) */
    long l = (a / g) * b;   /* g divides a exactly, so a/g is exact integer arithmetic */
    return (double)l;
}

static double factor(void) {
    skipws();
    if (*cur == '(') {
        cur++;
        double v = bor();
        skipws();
        if (*cur == ')') cur++; else err = 1;
        return v;
    }
    /* unary -/~ moved to unary() (M1784) so ^ binds tighter: -2^2 == -(2^2) == -4 */

    /* identifiers: constants (pi, e) and functions (name '(' arg ')') */
    if ((*cur >= 'a' && *cur <= 'z') || (*cur >= 'A' && *cur <= 'Z') || *cur == '_') {
        if (match_kw("pi")) return 3.14159265358979;
        if (match_kw("e"))  return 2.71828182845905;
        if (match_kw("ans")) return calc_ans;             /* last result (settable by the caller) */
        if (match_kw("sqrt"))  return js_sqrt(call_arg());
        if (match_kw("sin"))   return js_sin(calc_a2r(call_arg()));
        if (match_kw("cos"))   return js_cos(calc_a2r(call_arg()));
        if (match_kw("tan"))   return js_tan(calc_a2r(call_arg()));
        if (match_kw("asin"))  return calc_r2a(js_asin(call_arg()));
        if (match_kw("acos"))  return calc_r2a(js_acos(call_arg()));
        if (match_kw("atan"))  return calc_r2a(js_atan(call_arg()));
        if (match_kw("ln"))    return js_ln(call_arg());
        if (match_kw("log"))   return js_ln(call_arg()) / js_ln(10.0);   /* base-10 */
        if (match_kw("exp"))   return js_exp(call_arg());
        if (match_kw("abs"))   return js_fabs(call_arg());
        if (match_kw("floor")) return js_floor(call_arg());
        if (match_kw("ceil"))  return js_ceil(call_arg());
        if (match_kw("round")) return js_round(call_arg());
        if (match_kw("log2"))  return js_ln(call_arg()) / js_ln(2.0);
        if (match_kw("log10")) return js_ln(call_arg()) / js_ln(10.0);
        if (match_kw("trunc")) return js_trunc(call_arg());               /* toward zero, unlike floor */
        if (match_kw("sign"))  { double a = call_arg(); return a > 0 ? 1.0 : a < 0 ? -1.0 : 0.0; }
        if (match_kw("min"))   { double a, b; call_arg2(&a, &b); return a < b ? a : b; }
        if (match_kw("max"))   { double a, b; call_arg2(&a, &b); return a > b ? a : b; }
        if (match_kw("hypot")) { double a, b; call_arg2(&a, &b); return js_sqrt(a * a + b * b); }
        if (match_kw("atan2")) { double a, b; call_arg2(&a, &b); return calc_r2a(calc_atan2(a, b)); }
        /* hyperbolic + cube root + combinatorics + gcd/lcm (M1812). Hyperbolics take
         * a plain real argument, NOT an angle, so they are never DEG-converted. */
        if (match_kw("sinh"))  { double x = call_arg(); return (js_exp(x) - js_exp(-x)) * 0.5; }
        if (match_kw("cosh"))  { double x = call_arg(); return (js_exp(x) + js_exp(-x)) * 0.5; }
        if (match_kw("tanh"))  { double x = call_arg(); double a = x < 0 ? -x : x; double e = js_exp(-2.0 * a); double t = (1.0 - e) / (1.0 + e); return x < 0 ? -t : t; }
        if (match_kw("cbrt"))  { double x = call_arg(); double r = js_pow(x < 0 ? -x : x, 1.0 / 3.0); return x < 0 ? -r : r; }
        if (match_kw("fact"))  return calc_fact(call_arg());
        if (match_kw("gcd"))   { double a, b; call_arg2(&a, &b); return calc_gcd(a, b); }
        if (match_kw("lcm"))   { double a, b; call_arg2(&a, &b); return calc_lcm(a, b); }
        if (match_kw("npr"))   { double a, b; call_arg2(&a, &b); return calc_perm(a, b); }
        if (match_kw("ncr"))   { double a, b; call_arg2(&a, &b); return calc_comb(a, b); }
        /* variadic list statistics (M1827): sum/mean(avg)/stdev(sample)/variance/median/count */
        if (match_kw("sum"))   { double a[64]; int n = call_argn(a, 64); if (n > 64) n = 64; double s = 0; for (int k = 0; k < n; k++) s += a[k]; return s; }
        if (match_kw("mean") || match_kw("avg")) { double a[64]; int n = call_argn(a, 64); if (n > 64) n = 64; if (!n) return JS_NAN; double s = 0; for (int k = 0; k < n; k++) s += a[k]; return s / n; }
        if (match_kw("stdev") || match_kw("stddev")) { double a[64]; int n = call_argn(a, 64); if (n > 64) n = 64; if (n < 2) return JS_NAN; double s = 0; for (int k = 0; k < n; k++) s += a[k]; double m = s / n, ss = 0; for (int k = 0; k < n; k++) { double d = a[k] - m; ss += d * d; } return js_sqrt(ss / (n - 1)); }
        if (match_kw("variance") || match_kw("var")) { double a[64]; int n = call_argn(a, 64); if (n > 64) n = 64; if (n < 2) return JS_NAN; double s = 0; for (int k = 0; k < n; k++) s += a[k]; double m = s / n, ss = 0; for (int k = 0; k < n; k++) { double d = a[k] - m; ss += d * d; } return ss / (n - 1); }
        if (match_kw("median")) { double a[64]; int n = call_argn(a, 64); if (n > 64) n = 64; if (!n) return JS_NAN; for (int i = 1; i < n; i++) { double k = a[i]; int j = i - 1; while (j >= 0 && a[j] > k) { a[j+1] = a[j]; j--; } a[j+1] = k; } return (n & 1) ? a[n/2] : (a[n/2 - 1] + a[n/2]) / 2.0; }
        if (match_kw("count"))  { double a[64]; return (double)call_argn(a, 64); }
        err = 1;                          /* unknown identifier/function */
        return JS_NAN;
    }

    /* numeric literals: 0x-hex (integer -> double) or decimal (3.14, .5, 1e3) */
    if (cur[0] == '0' && (cur[1] == 'x' || cur[1] == 'X')) {          /* hex literal: 0x... */
        cur += 2;
        unsigned long v = 0; int any = 0;
        for (;;) {
            char c = *cur; int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = v * 16 + d; cur++; any = 1;
        }
        if (!any) err = 1;
        return (double)v;
    }
    if (cur[0] == '0' && (cur[1] == 'b' || cur[1] == 'B')) {          /* binary literal: 0b1010 (M1812) */
        cur += 2;
        unsigned long v = 0; int any = 0;
        while (*cur == '0' || *cur == '1') { v = v * 2 + (unsigned)(*cur - '0'); cur++; any = 1; }
        if (!any) err = 1;
        return (double)v;
    }
    /* decimal: [digits] [ '.' digits ] [ ('e'|'E') ['+'|'-'] digits ] */
    {
        double v = 0; int any = 0;
        while (*cur >= '0' && *cur <= '9') { v = v * 10.0 + (*cur - '0'); cur++; any = 1; }
        if (*cur == '.') {
            cur++;
            double f = 0.1;
            while (*cur >= '0' && *cur <= '9') { v += (*cur - '0') * f; f *= 0.1; cur++; any = 1; }
        }
        if (any && (*cur == 'e' || *cur == 'E')) {
            const char *save = cur;
            cur++;
            int eneg = 0;
            if (*cur == '+') cur++; else if (*cur == '-') { eneg = 1; cur++; }
            int ex = 0, ed = 0;
            while (*cur >= '0' && *cur <= '9') { ex = ex * 10 + (*cur - '0'); cur++; ed = 1; }
            if (ed) v *= js_pow(10.0, eneg ? -(double)ex : (double)ex);
            else cur = save;              /* 'e' not followed by an exponent -> not part of the number */
        }
        if (!any) err = 1;
        return v;
    }
}

static double unary(void) {               /* unary -/~ : looser than ^ (so -2^2 == -(2^2) == -4), but the ^ exponent is itself unary (2^-2 stays 0.25) — M1784 */
    skipws();
    if (*cur == '-') { cur++; return -unary(); }
    if (*cur == '~') { cur++; return (double)(~to_long(unary())); }   /* bitwise NOT (integer) */
    return power();
}
static double power(void) {               /* right-associative ^, binds tighter than unary and * / % */
    double b = factor();
    skipws();
    while (*cur == '!') { cur++; b = calc_fact(b); skipws(); }   /* postfix factorial: 5! = 120, `3!^2` = (3!)^2 (M1812) */
    if (*cur == '^') {
        cur++;
        double e = unary();               /* M1784: exponent is a unary-expr, so 2^-2 == 0.25 */
        return js_pow(b, e);
    }
    return b;
}

static double term(void) {
    double v = unary();
    for (;;) {
        skipws();
        if (*cur == '*') { cur++; v *= unary(); }
        else if (*cur == '/') { cur++; v /= unary(); }            /* real division: /0 -> +-Inf, 0/0 -> NaN (no trap) */
        else if (*cur == '%') { cur++; v = js_fmod(v, unary()); } /* JS-style fmod: sign of dividend, x%0 -> NaN */
        else break;
    }
    return v;
}

static double expr(void) {
    double v = term();
    for (;;) {
        skipws();
        if (*cur == '+') { cur++; v += term(); }
        else if (*cur == '-') { cur++; v -= term(); }
        else break;
    }
    return v;
}

static double shift(void) {       /* << >> ; looser than + - ; integer ops */
    double v = expr();
    for (;;) {
        skipws();
        if (cur[0] == '<' && cur[1] == '<') { cur += 2; long s = to_long(expr()); long b = to_long(v); v = (double)((s >= 0 && s < 64) ? b << s : 0); }
        else if (cur[0] == '>' && cur[1] == '>') { cur += 2; long s = to_long(expr()); long b = to_long(v); v = (double)((s >= 0 && s < 64) ? b >> s : 0); }
        else break;
    }
    return v;
}
static double band(void) {        /* bitwise AND (integer) */
    double v = shift();
    for (;;) { skipws(); if (*cur == '&') { cur++; v = (double)(to_long(v) & to_long(shift())); } else break; }
    return v;
}
static double bor(void) {         /* bitwise OR (lowest precedence; integer) */
    double v = band();
    for (;;) { skipws(); if (*cur == '|') { cur++; v = (double)(to_long(v) | to_long(band())); } else break; }
    return v;
}

/* Evaluate the NUL-terminated expression `s`. Sets *out_err to 1 on a syntax
 * error (bad token, unknown name, unmatched ')', missing function arg, or
 * trailing junk), else 0. Undefined *math* (1/0, sqrt(-1), ln(0)) yields
 * Inf/NaN with no error — only structural problems set the flag. The returned
 * double is meaningful only when *out_err is 0. */
static double calc_eval(const char *s, int *out_err) {
    cur = s; err = 0;
    double r = bor();
    skipws();
    if (*cur) err = 1;          /* unconsumed trailing characters = syntax error */
    if (out_err) *out_err = err;
    return r;
}

#endif /* CALCEVAL_H */
