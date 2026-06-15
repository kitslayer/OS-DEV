/*
 * calc.c — a third userspace program: an interactive calculator.
 *
 * A small recursive-descent expression evaluator (the same shape a real
 * compiler's parser has): expr -> term (+|-) term, term -> factor (*|/) factor,
 * factor -> number | ( expr ) | -factor. It reads a line, parses + evaluates
 * it, and prints the result — a genuinely distinct interactive program from the
 * shell and the clock, all running as isolated windowed processes.
 */
#include "ulib.h"

static const char *cur;          /* the parse cursor */
static int err;
static long expr(void);

static void skipws(void) { while (*cur == ' ' || *cur == '\t') cur++; }

static long factor(void) {
    skipws();
    if (*cur == '(') {
        cur++;
        long v = expr();
        skipws();
        if (*cur == ')') cur++; else err = 1;
        return v;
    }
    if (*cur == '-') { cur++; return -factor(); }
    long v = 0; int any = 0;
    while (*cur >= '0' && *cur <= '9') { v = v * 10 + (*cur - '0'); cur++; any = 1; }
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
        else if (*cur == '/') { cur++; long d = power(); if (d == 0) err = 1; else v /= d; }
        else if (*cur == '%') { cur++; long d = power(); if (d == 0) err = 1; else v %= d; }
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

static void itoa_l(long v, char *out) {
    char t[24]; int i = 0, neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (u == 0) t[i++] = '0';
    while (u) { t[i++] = (char)('0' + u % 10); u /= 10; }
    int j = 0; if (neg) out[j++] = '-';
    while (i) out[j++] = t[--i];
    out[j] = 0;
}

int main(void) {
    sys_setcolor(4); print("\n  OS-DEV calc -- + - * / % ^ and ( )\n");   /* title: cyan */
    sys_setcolor(8); print("  e.g. (2+3)*4 ; 'q' to quit\n\n"); sys_setcolor(0);
    char line[128];
    for (;;) {
        print("calc> ");
        readline(line, sizeof(line));
        if (line[0] == '\0') continue;
        if (streq(line, "q") || streq(line, "quit") || streq(line, "exit")) break;
        cur = line; err = 0;
        long r = expr();
        skipws();
        if (err || *cur) { sys_setcolor(2); print("  ? syntax error\n"); sys_setcolor(0); }   /* error: red */
        else { char b[24]; itoa_l(r, b); print("  = "); sys_setcolor(3); print(b); sys_setcolor(0); print("\n"); }  /* result: yellow */
    }
    print("bye!\n");
    return 0;
}
