/* shmath.h — the shell's integer arithmetic evaluator for $((expr)): recursive
 * descent with bash's $(()) operator set and precedence — unary - + ~ !, **
 * (power), * / %, + -, << >> (shift), < <= > >= (relational), == != (equality),
 * & (and), ^ (xor), | (or), && (logical and), || (logical or), ?: (ternary) —
 * plus parentheses, decimal/0x literals, and variable names (bare or $-prefixed).
 * Integer-only (no FPU), like the rest of the OS; division/modulo by zero and
 * out-of-range shifts yield 0, ** is loop-capped. Pure except for sh_var()
 * (variable lookup), which the includer provides — so it is host-unit-tested by
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
    int neg = 0; while (*s == ' ') s++; if (*s == '-') { neg = 1; s++; }
    unsigned long v = 0;                                 /* unsigned accumulate: overflow wraps (defined) */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        for (;;) {
            int d; char c = *s;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = v * 16 + (unsigned)d; s++;
        }
    } else while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned)(*s - '0'); s++; }
    return (long)(neg ? 0UL - v : v);
}
static void sh_askip(const char **p) { while (**p == ' ' || **p == '\t') (*p)++; }
static long sh_ternary(const char **p);                 /* lowest-precedence level (the entry) */
static long sh_factor(const char **p) {
    sh_askip(p);
    if (**p == '-') { (*p)++; return (long)(0UL - (unsigned long)sh_factor(p)); }   /* defined for LONG_MIN */
    if (**p == '+') { (*p)++; return  sh_factor(p); }
    if (**p == '~') { (*p)++; return ~sh_factor(p); }    /* bitwise NOT */
    if (**p == '!' && (*p)[1] != '=') { (*p)++; return !sh_factor(p); }   /* logical NOT -> 1/0 (not !=) */
    if (**p == '(') { (*p)++; long v = sh_ternary(p); sh_askip(p); if (**p == ')') (*p)++; return v; }
    if (**p == '$') { (*p)++;                            /* $-prefixed name -> variable, even $1..$9: a digit after $ is a positional param, not a literal */
        if (sh_vchar(**p)) { const char *s = *p; int nl = 0; while (sh_vchar(s[nl])) nl++;
            *p = s + nl; return sh_var(s, nl); }
        return 0; }                                      /* lone $ -> 0 */
    if (**p >= '0' && **p <= '9') { const char *s = *p; unsigned long v = 0;   /* unsigned: overflow wraps */
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
            for (;;) {
                int d; char c = *s;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else break;
                v = v * 16 + (unsigned)d; s++;
            }
        } else while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned)(*s - '0'); s++; }
        *p = s; return (long)v; }
    if (sh_vchar(**p)) { const char *s = *p; int nl = 0; while (sh_vchar(s[nl])) nl++;   /* identifier -> variable */
        *p = s + nl; return sh_var(s, nl); }
    return 0;
}
static long sh_pow(const char **p) {                    /* ** (right-associative, binds tightest) */
    long b = sh_factor(p); sh_askip(p);
    if ((*p)[0] == '*' && (*p)[1] == '*') { (*p) += 2; long e = sh_pow(p);
        if (e < 0) return 0;                            /* integer: x ** (negative) -> 0 */
        unsigned long r = 1, ub = (unsigned long)b;     /* unsigned: overflow wraps (defined) */
        for (long i = 0; i < e && i < 64; i++) r *= ub;  /* loop capped */
        return (long)r; }
    return b;
}
static long sh_term(const char **p) {
    long v = sh_pow(p);
    for (;;) { sh_askip(p); char c = **p;
        if (c == '*' && (*p)[1] != '*') { (*p)++; v = (long)((unsigned long)v * (unsigned long)sh_pow(p)); }
        else if (c == '/') { (*p)++; long d = sh_pow(p);                       /* signed; guard /0 and MIN/-1 */
            v = (d == 0) ? 0 : (d == -1) ? (long)(0UL - (unsigned long)v) : v / d; }
        else if (c == '%') { (*p)++; long d = sh_pow(p); v = (d == 0 || d == -1) ? 0 : v % d; }
        else break; }
    return v;
}
static long sh_addsub(const char **p) {
    long v = sh_term(p);
    for (;;) { sh_askip(p); char c = **p;
        if (c == '+') { (*p)++; v = (long)((unsigned long)v + (unsigned long)sh_term(p)); }
        else if (c == '-') { (*p)++; v = (long)((unsigned long)v - (unsigned long)sh_term(p)); }
        else break; }
    return v;
}
static long sh_shift(const char **p) {                  /* << >> */
    long v = sh_addsub(p);
    for (;;) { sh_askip(p);
        if ((*p)[0] == '<' && (*p)[1] == '<') { (*p) += 2; long s = sh_addsub(p); v = (s >= 0 && s < 64) ? (long)((unsigned long)v << s) : 0; }
        else if ((*p)[0] == '>' && (*p)[1] == '>') { (*p) += 2; long s = sh_addsub(p); v = (s >= 0 && s < 64) ? v >> s : 0; }
        else break; }
    return v;
}
static long sh_relational(const char **p) {             /* < <= > >= (sits just above the shift level) */
    long v = sh_shift(p);
    for (;;) { sh_askip(p);
        if      ((*p)[0] == '<' && (*p)[1] == '=') { (*p) += 2; v = (v <= sh_shift(p)); }
        else if ((*p)[0] == '>' && (*p)[1] == '=') { (*p) += 2; v = (v >= sh_shift(p)); }
        else if ((*p)[0] == '<' && (*p)[1] != '<') { (*p)++;    v = (v <  sh_shift(p)); }   /* '<' but not '<<' */
        else if ((*p)[0] == '>' && (*p)[1] != '>') { (*p)++;    v = (v >  sh_shift(p)); }   /* '>' but not '>>' */
        else break; }
    return v;
}
static long sh_equality(const char **p) {               /* == != */
    long v = sh_relational(p);
    for (;;) { sh_askip(p);
        if      ((*p)[0] == '=' && (*p)[1] == '=') { (*p) += 2; v = (v == sh_relational(p)); }
        else if ((*p)[0] == '!' && (*p)[1] == '=') { (*p) += 2; v = (v != sh_relational(p)); }
        else break; }
    return v;
}
static long sh_band(const char **p) {                   /* bitwise & (not &&) */
    long v = sh_equality(p);
    for (;;) { sh_askip(p); if (**p == '&' && (*p)[1] != '&') { (*p)++; v &= sh_equality(p); } else break; }
    return v;
}
static long sh_bxor(const char **p) {                   /* bitwise ^ */
    long v = sh_band(p);
    for (;;) { sh_askip(p); if (**p == '^') { (*p)++; v ^= sh_band(p); } else break; }
    return v;
}
static long sh_or(const char **p) {                     /* bitwise | (not ||) */
    long v = sh_bxor(p);
    for (;;) { sh_askip(p); if (**p == '|' && (*p)[1] != '|') { (*p)++; v |= sh_bxor(p); } else break; }
    return v;
}
static long sh_land(const char **p) {                   /* logical && -> 1/0 */
    long v = sh_or(p);
    for (;;) { sh_askip(p);
        if ((*p)[0] == '&' && (*p)[1] == '&') { (*p) += 2; long r = sh_or(p); v = (v != 0 && r != 0); }
        else break; }
    return v;
}
static long sh_lor(const char **p) {                    /* logical || -> 1/0 */
    long v = sh_land(p);
    for (;;) { sh_askip(p);
        if ((*p)[0] == '|' && (*p)[1] == '|') { (*p) += 2; long r = sh_land(p); v = (v != 0 || r != 0); }
        else break; }
    return v;
}
static long sh_ternary(const char **p) {                /* cond ? then : else (right-associative) */
    long c = sh_lor(p); sh_askip(p);
    if (**p == '?') { (*p)++;
        long a = sh_ternary(p); sh_askip(p);            /* then-branch (stops at ':') */
        if (**p == ':') (*p)++;
        long b = sh_ternary(p);                         /* else-branch */
        return c ? a : b; }
    return c;
}
/* Public entry: evaluate the expression at *p, advancing *p past what it ate.
 * Precedence (high->low): unary - + ~ !, ** , * / %, + -, << >>, < <= > >=,
 * == !=, &, ^, |, &&, ||, ?: — bash's $(()) order (note: ^ is XOR here, like
 * bash; the calc app uses ^ for power, via its own evaluator). */
static long sh_eval(const char **p) { return sh_ternary(p); }

#endif /* SHMATH_H */
