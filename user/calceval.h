/* calceval.h — the calculator app's integer expression evaluator: recursive
 * descent with C-like precedence — unary - ~, ^ (power, right-assoc, binds
 * tightest), * / %, + -, << >>, & , | (lowest) — plus parentheses and
 * decimal/0x-hex literals. Note: calc uses ^ for POWER (not XOR) and has no
 * relational/logical operators, unlike the shell's $(()) evaluator (shmath.h).
 * Integer-only; division/modulo by zero and a stray ) set `err`; ^ is loop-capped.
 * Pure (no syscalls), so it's host-unit-tested by tests/calc; user/calc.c
 * #includes it for the interactive calculator.
 *
 * The OS-authored userspace is built with -fwrapv (M781), so the signed
 * add/sub/mul accumulation below wraps (defined); the host test is built the same way. */
#ifndef CALCEVAL_H
#define CALCEVAL_H

#ifndef LONG_MIN
#define LONG_MIN (-9223372036854775807L - 1)   /* freestanding: no limits.h */
#endif

static const char *cur;          /* the parse cursor */
static int err;
static long expr(void);
static long bor(void);          /* the lowest-precedence level (bitwise OR) — the eval entry */

static void skipws(void) { while (*cur == ' ' || *cur == '\t') cur++; }

static long factor(void) {
    skipws();
    if (*cur == '(') {
        cur++;
        long v = bor();
        skipws();
        if (*cur == ')') cur++; else err = 1;
        return v;
    }
    if (*cur == '-') { cur++; return -factor(); }
    if (*cur == '~') { cur++; return ~factor(); }   /* bitwise NOT */
    long v = 0; int any = 0;
    if (cur[0] == '0' && (cur[1] == 'x' || cur[1] == 'X')) {          /* hex literal: 0x... */
        cur += 2;
        for (;;) {
            char c = *cur; int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = v * 16 + d; cur++; any = 1;
        }
    } else {
        while (*cur >= '0' && *cur <= '9') { v = v * 10 + (*cur - '0'); cur++; any = 1; }
    }
    if (!any) err = 1;
    return v;
}

static long power(void) {               /* right-associative ^, binds tighter than * / % */
    long b = factor();
    skipws();
    if (*cur == '^') {
        cur++;
        long e = power();
        if (e < 0) return 0;            /* integer: x^(negative) rounds to 0 */
        long r = 1;
        for (long i = 0; i < e && i < 64; i++) r *= b;   /* capped to bound the loop */
        return r;
    }
    return b;
}

static long term(void) {
    long v = power();
    for (;;) {
        skipws();
        if (*cur == '*') { cur++; v *= power(); }
        else if (*cur == '/') { cur++; long d = power(); if (d == 0 || (d == -1 && v == LONG_MIN)) err = 1; else v /= d; }
        else if (*cur == '%') { cur++; long d = power(); if (d == 0 || (d == -1 && v == LONG_MIN)) err = 1; else v %= d; }
        else break;
    }
    return v;
}

static long expr(void) {
    long v = term();
    for (;;) {
        skipws();
        if (*cur == '+') { cur++; v += term(); }
        else if (*cur == '-') { cur++; v -= term(); }
        else break;
    }
    return v;
}

static long shift(void) {       /* << >> ; looser than + - */
    long v = expr();
    for (;;) {
        skipws();
        if (cur[0] == '<' && cur[1] == '<') { cur += 2; long s = expr(); v = (s >= 0 && s < 64) ? v << s : 0; }
        else if (cur[0] == '>' && cur[1] == '>') { cur += 2; long s = expr(); v = (s >= 0 && s < 64) ? v >> s : 0; }
        else break;
    }
    return v;
}
static long band(void) {        /* bitwise AND */
    long v = shift();
    for (;;) { skipws(); if (*cur == '&') { cur++; v &= shift(); } else break; }
    return v;
}
static long bor(void) {         /* bitwise OR (lowest precedence) */
    long v = band();
    for (;;) { skipws(); if (*cur == '|') { cur++; v |= band(); } else break; }
    return v;
}

/* Evaluate the NUL-terminated expression `s`. Sets *out_err to 1 on a syntax
 * error (bad token, unmatched ')', /0 or %0, or trailing junk), else 0. */
static long calc_eval(const char *s, int *out_err) {
    cur = s; err = 0;
    long r = bor();
    skipws();
    if (*cur) err = 1;          /* unconsumed trailing characters = syntax error */
    if (out_err) *out_err = err;
    return r;
}

#endif /* CALCEVAL_H */
