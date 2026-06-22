/* shbrace.h — the shell's brace expansion: a{1,2}b -> "a1b a2b", {1..4} ->
 * "1 2 3 4", {a..d} -> "a b c d", and the cartesian product of adjacent groups
 * ({a,b}{1,2} -> a1 a2 b1 b2). A bash-style preprocessing pass run after
 * variable/alias expansion and before glob. Safe + additive: a `{...}` is only
 * expanded when it has no spaces and either a top-level comma or a valid range,
 * and is not preceded by `$` -- so ${VAR}, $(...), function bodies `f() { ...; }`
 * and command groups `{ cmd; }` are left untouched. Pure (no syscalls), so it's
 * host-unit-tested by tests/shbrace like shgrep/shmath/shsplit; user/shell.c
 * #includes it and run_line applies expand_braces() to the command line. */
#ifndef SHBRACE_H
#define SHBRACE_H

static int br_itoa(long v, char *o) {
    char t[24]; int n = 0, neg = v < 0; unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (!u) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + u % 10); u /= 10; }
    int j = 0; if (neg) o[j++] = '-';
    while (n) o[j++] = t[--n];
    return j;
}
static int br_int(const char *s, int n, int *pi, long *pv) {   /* parse a signed int from s[*pi..n) */
    int i = *pi, sign = 1, got = 0; long v = 0;
    if (i < n && s[i] == '-') { sign = -1; i++; }
    while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; got = 1; }
    if (!got) return 0;
    *pi = i; *pv = sign * v; return 1;
}
/* {N..M[..S]} numeric or {a..z} single-char range; fills lo/hi/step/ischar. */
static int br_range(const char *c, int clen, long *lo, long *hi, long *step, int *ischar) {
    if (clen == 4 && c[1] == '.' && c[2] == '.' &&
        (((c[0]|32) >= 'a' && (c[0]|32) <= 'z')) && (((c[3]|32) >= 'a' && (c[3]|32) <= 'z'))) {
        *lo = c[0]; *hi = c[3]; *step = 1; *ischar = 1; return 1;
    }
    int i = 0; long a, b, s = 1;
    if (!br_int(c, clen, &i, &a)) return 0;
    if (i + 1 >= clen || c[i] != '.' || c[i + 1] != '.') return 0;
    i += 2;
    if (!br_int(c, clen, &i, &b)) return 0;
    if (i < clen) {                                  /* optional ..step */
        if (i + 1 < clen && c[i] == '.' && c[i + 1] == '.') { i += 2; if (!br_int(c, clen, &i, &s)) return 0; }
        if (i != clen) return 0;                     /* trailing junk -> not a clean range */
    }
    if (s < 0) s = -s;
    if (s == 0) s = 1;
    *lo = a; *hi = b; *step = s; *ischar = 0; return 1;
}
static int brace_eligible(const char *s, int lb, int *prb) {
    if (lb > 0 && s[lb - 1] == '$') return 0;        /* ${...} parameter expansion */
    int depth = 0, hascomma = 0;
    for (int i = lb; s[i]; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t') return 0;         /* spaces -> function body / command group, not expansion */
        if (c == '{') depth++;
        else if (c == '}') {
            if (--depth == 0) {
                *prb = i;
                if (i <= lb + 1) return 0;            /* empty {} */
                if (hascomma) return 1;
                long lo, hi, st; int ic;
                return br_range(s + lb + 1, i - lb - 1, &lo, &hi, &st, &ic);
            }
        } else if (depth == 1 && c == ',') hascomma = 1;
    }
    return 0;                                        /* unbalanced */
}
static int find_first_group(const char *s, int *plb, int *prb) {
    for (int i = 0; s[i]; i++)
        if (s[i] == '{' && brace_eligible(s, i, prb)) { *plb = i; return 1; }
    return 0;
}
/* Expand the single group s[lb..rb] into out; return new length or -1 on overflow. */
static int brace_expand_one(const char *s, int lb, int rb, char *out, int cap) {
    int ws = lb; while (ws > 0 && s[ws - 1] != ' ' && s[ws - 1] != '\t') ws--;
    int we = rb + 1; while (s[we] && s[we] != ' ' && s[we] != '\t') we++;
    const char *content = s + lb + 1; int clen = rb - lb - 1;
    int o = 0, first = 1;
    #define EMIT(ch)  do { if (o >= cap - 1) return -1; out[o++] = (char)(ch); } while (0)
    #define EMIT_ITEM(IP, IL) do { \
        if (!first) EMIT(' '); \
        first = 0; \
        for (int _k = ws; _k < lb; _k++) EMIT(s[_k]); \
        for (int _k = 0; _k < (IL); _k++) EMIT((IP)[_k]); \
        for (int _k = rb + 1; _k < we; _k++) EMIT(s[_k]); \
    } while (0)
    for (int i = 0; i < ws; i++) EMIT(s[i]);          /* text before the word */
    int depth = 0, hascomma = 0;
    for (int i = 0; i < clen; i++) { char c = content[i]; if (c == '{') depth++; else if (c == '}') depth--; else if (depth == 0 && c == ',') hascomma = 1; }
    if (hascomma) {                                   /* comma list: split on top-level commas */
        depth = 0; int istart = 0;
        for (int i = 0; i <= clen; i++) {
            if (i == clen) { EMIT_ITEM(content + istart, i - istart); }
            else { char c = content[i]; if (c == '{') depth++; else if (c == '}') depth--; else if (depth == 0 && c == ',') { EMIT_ITEM(content + istart, i - istart); istart = i + 1; } }
        }
    } else {                                          /* range (guaranteed valid by eligibility) */
        long lo, hi, st; int ic; br_range(content, clen, &lo, &hi, &st, &ic);
        int dir = (lo <= hi) ? 1 : -1;
        for (long v = lo; (dir > 0) ? (v <= hi) : (v >= hi); v += dir * st) {
            if (ic) { char ch = (char)v; EMIT_ITEM(&ch, 1); }
            else { char num[24]; int nl = br_itoa(v, num); EMIT_ITEM(num, nl); }
        }
    }
    for (int i = we; s[i]; i++) EMIT(s[i]);            /* text after the word */
    out[o] = 0; return o;
    #undef EMIT
    #undef EMIT_ITEM
}
static int expand_braces(const char *src, char *dst, int cap) {
    int o = 0; for (int i = 0; src[i] && o < cap - 1; i++) dst[o++] = src[i]; dst[o] = 0;
    static char brtmp[1024];
    int changed = 0, guard = 0;
    for (;;) {
        int lb, rb;
        if (!find_first_group(dst, &lb, &rb)) break;
        int nl = brace_expand_one(dst, lb, rb, brtmp, (int)sizeof brtmp);
        if (nl < 0) break;                            /* overflow: keep what we have */
        for (int i = 0; i <= nl; i++) dst[i] = brtmp[i];
        changed = 1;
        if (++guard > 2000) break;                    /* safety against pathological growth */
    }
    return changed;
}

#endif /* SHBRACE_H */
