/* calceval.h — the calculator app's floating-point SCIENTIFIC expression
 * evaluator: recursive descent with C-like precedence — unary - ~, ^ (power,
 * right-assoc, binds tightest), * / %, + -, << >>, & , | (lowest) — plus
 * parentheses, decimal (3.14, .5, 1e3, 1.5e-2) and 0x-hex literals, the
 * constants pi/e, and the functions sqrt sin cos tan asin acos atan ln log
 * log2 log10 exp abs sign floor ceil round trunc, plus the two-argument
 * min(a,b) max(a,b) hypot(a,b). Results are IEEE-754 doubles (calc.o is built
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
static double bor(void);          /* the lowest-precedence level (bitwise OR) — the eval entry */

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
 * sets err on a missing paren/comma. Used by the 2-arg functions (min/max/hypot). */
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

static double factor(void) {
    skipws();
    if (*cur == '(') {
        cur++;
        double v = bor();
        skipws();
        if (*cur == ')') cur++; else err = 1;
        return v;
    }
    if (*cur == '-') { cur++; return -factor(); }
    if (*cur == '~') { cur++; return (double)(~to_long(factor())); }   /* bitwise NOT (integer) */

    /* identifiers: constants (pi, e) and functions (name '(' arg ')') */
    if ((*cur >= 'a' && *cur <= 'z') || (*cur >= 'A' && *cur <= 'Z') || *cur == '_') {
        if (match_kw("pi")) return 3.14159265358979;
        if (match_kw("e"))  return 2.71828182845905;
        if (match_kw("ans")) return calc_ans;             /* last result (settable by the caller) */
        if (match_kw("sqrt"))  return js_sqrt(call_arg());
        if (match_kw("sin"))   return js_sin(call_arg());
        if (match_kw("cos"))   return js_cos(call_arg());
        if (match_kw("tan"))   return js_tan(call_arg());
        if (match_kw("asin"))  return js_asin(call_arg());
        if (match_kw("acos"))  return js_acos(call_arg());
        if (match_kw("atan"))  return js_atan(call_arg());
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

static double power(void) {               /* right-associative ^, binds tighter than * / % */
    double b = factor();
    skipws();
    if (*cur == '^') {
        cur++;
        double e = power();
        return js_pow(b, e);
    }
    return b;
}

static double term(void) {
    double v = power();
    for (;;) {
        skipws();
        if (*cur == '*') { cur++; v *= power(); }
        else if (*cur == '/') { cur++; v /= power(); }            /* real division: /0 -> +-Inf, 0/0 -> NaN (no trap) */
        else if (*cur == '%') { cur++; v = js_fmod(v, power()); } /* JS-style fmod: sign of dividend, x%0 -> NaN */
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
