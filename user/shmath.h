/* shmath.h — the shell's integer arithmetic evaluator for $((expr)): recursive
 * descent over + - * / %, unary +/-, parentheses, decimal/0x literals, and
 * variable names (bare or $-prefixed). Integer-only (no FPU), like the rest of
 * the OS; division/modulo by zero yield 0. Pure except for sh_var() (variable
 * lookup), which the includer provides — so it is host-unit-tested by
 * tests/shmath, and user/shell.c #includes it for the `set` / $((...)) feature.
 *
 * NOTE: keep this in sync with its host test (tests/shmath/shmath_test.c). */
#ifndef SHMATH_H
#define SHMATH_H

/* Provided by the includer: the integer value of the variable name[0..len). */
static long sh_var(const char *name, int len);

static int sh_vchar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
/* Parse a leading (optionally signed) integer, decimal or 0x hex. */
static long sh_str2long(const char *s) {
    long sign = 1; while (*s == ' ') s++; if (*s == '-') { sign = -1; s++; }
    long v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; for (;;) { int d; char c = *s;
        if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else break; v = v * 16 + d; s++; } }
    else while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return sign * v;
}
static void sh_askip(const char **p) { while (**p == ' ' || **p == '\t') (*p)++; }
static long sh_expr(const char **p);
static long sh_factor(const char **p) {
    sh_askip(p);
    if (**p == '-') { (*p)++; return -sh_factor(p); }
    if (**p == '+') { (*p)++; return  sh_factor(p); }
    if (**p == '(') { (*p)++; long v = sh_expr(p); sh_askip(p); if (**p == ')') (*p)++; return v; }
    if (**p == '$') (*p)++;                              /* allow $X inside arithmetic */
    if (**p >= '0' && **p <= '9') { const char *s = *p; long v;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { v = 0; s += 2; for (;;) { int d; char c = *s;
            if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else break; v = v * 16 + d; s++; } }
        else { v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; } }
        *p = s; return v; }
    if (sh_vchar(**p)) { const char *s = *p; int nl = 0; while (sh_vchar(s[nl])) nl++;   /* identifier -> variable */
        *p = s + nl; return sh_var(s, nl); }
    return 0;
}
static long sh_term(const char **p) {
    long v = sh_factor(p);
    for (;;) { sh_askip(p); char c = **p;
        if (c == '*') { (*p)++; v *= sh_factor(p); }
        else if (c == '/') { (*p)++; long d = sh_factor(p); v = d ? v / d : 0; }
        else if (c == '%') { (*p)++; long d = sh_factor(p); v = d ? v % d : 0; }
        else break; }
    return v;
}
static long sh_expr(const char **p) {
    long v = sh_term(p);
    for (;;) { sh_askip(p); char c = **p;
        if (c == '+') { (*p)++; v += sh_term(p); }
        else if (c == '-') { (*p)++; v -= sh_term(p); }
        else break; }
    return v;
}
/* Public entry: evaluate the expression at *p, advancing *p past what it ate. */
static long sh_eval(const char **p) { return sh_expr(p); }

#endif /* SHMATH_H */
