/* shgrep.h — the shell's tiny grep regex matcher: ^ $ . * [..] character
 * classes, \ escapes, and the -i case fold (Kernighan/Pike style). A pattern
 * with no metacharacters behaves exactly like a literal substring search, so
 * existing greps are unaffected. Pure (no syscalls), so it's host-unit-tested by
 * tests/shgrep; user/shell.c #includes it for the `grep` command. */
#ifndef SHGREP_H
#define SHGREP_H

static char gr_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int gr_matchhere(const char *re, const char *t, int ci, const char **endp);  /* endp (or NULL): where the match ended, for grep -o */

/* length of a [..] class starting at re[0]=='[' (a leading ] is a literal member) */
static int gr_classlen(const char *re) {
    int i = 1; if (re[i] == '^') i++; if (re[i] == ']') i++;
    while (re[i] && re[i] != ']') i++;
    return re[i] == ']' ? i + 1 : i;
}
/* is `ch` a member of the [..] class at `re`? handles ranges a-z, negation [^..], case-fold */
static int gr_inclass(const char *re, int ch, int ci) {
    int i = 1, neg = 0, ok = 0;
    if (re[i] == '^') { neg = 1; i++; }
    for (; re[i] && re[i] != ']'; i++) {
        if (re[i+1] == '-' && re[i+2] && re[i+2] != ']') {
            int lo = (unsigned char)re[i], hi = (unsigned char)re[i+2]; i += 2;
            int c = ci ? gr_lc((char)ch) : ch;
            if ((ci ? gr_lc((char)lo) : lo) <= c && c <= (ci ? gr_lc((char)hi) : hi)) ok = 1;
        } else if (ci ? gr_lc((char)ch) == gr_lc(re[i]) : ch == (unsigned char)re[i]) ok = 1;
    }
    return neg ? !ok : ok;
}
/* `c*`: greedy — consume every c, then backtrack so the rest matches at the
 * longest point first (so grep -o reports the longest match). Boolean result is
 * identical to a lazy quantifier — a match exists or it doesn't either way. */
static int gr_matchstar(int c, const char *re, const char *t, int ci, const char **endp) {
    const char *start = t;
    while (*t && (c == '.' || (ci ? gr_lc(*t) == gr_lc((char)c) : (unsigned char)*t == (unsigned char)c))) t++;
    for (;;) {
        if (gr_matchhere(re, t, ci, endp)) return 1;
        if (t == start) break;
        t--;
    }
    return 0;
}
static int gr_matchhere(const char *re, const char *t, int ci, const char **endp) {
    if (re[0] == '\0') { if (endp) *endp = t; return 1; }
    if (re[0] == '[') {                                            /* [..] character class, incl. [..]* */
        int cl = gr_classlen(re);
        if (re[cl] == '*') { const char *rest = re + cl + 1, *start = t;
            while (*t && gr_inclass(re, (unsigned char)*t, ci)) t++;   /* greedy */
            for (;;) { if (gr_matchhere(rest, t, ci, endp)) return 1; if (t == start) break; t--; }
            return 0; }
        if (*t && gr_inclass(re, (unsigned char)*t, ci)) return gr_matchhere(re + cl, t + 1, ci, endp);
        return 0;
    }
    if (re[0] == '\\' && re[1]) {                                  /* escaped literal: \. \* \^ … */
        if (re[2] == '*') return gr_matchstar((unsigned char)re[1], re + 3, t, ci, endp);
        if (*t && (ci ? gr_lc(*t) == gr_lc(re[1]) : *t == re[1])) return gr_matchhere(re + 2, t + 1, ci, endp);
        return 0;
    }
    if (re[1] == '*') return gr_matchstar((unsigned char)re[0], re + 2, t, ci, endp);
    if (re[0] == '$' && re[1] == '\0') { if (*t == '\0') { if (endp) *endp = t; return 1; } return 0; }
    if (*t && (re[0] == '.' || (ci ? gr_lc(*t) == gr_lc(re[0]) : (unsigned char)*t == (unsigned char)re[0])))
        return gr_matchhere(re + 1, t + 1, ci, endp);
    return 0;
}
static int gr_match(const char *re, const char *t, int ci) {     /* match anywhere (^ anchors to start) */
    const char *e;
    if (re[0] == '^') return gr_matchhere(re + 1, t, ci, &e);
    do { if (gr_matchhere(re, t, ci, &e)) return 1; } while (*t++);
    return 0;
}
static int gr_isword(int c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'; }
/* grep -w: match only where the matched span is bounded by a non-word char (or an
 * end of the text) on both sides, so `grep -w cat` hits "a cat" but not "category". */
static int gr_match_word(const char *re, const char *t, int ci) {
    const char *base = t, *e;
    if (re[0] == '^') {                                          /* ^ anchors the start; the line start is a boundary */
        if (gr_matchhere(re + 1, t, ci, &e) && !gr_isword((unsigned char)*e)) return 1;
        return 0;
    }
    for (const char *p = t; ; p++) {
        if (gr_matchhere(re, p, ci, &e)) {
            int lb = (p == base) || !gr_isword((unsigned char)p[-1]);
            if (lb && !gr_isword((unsigned char)*e)) return 1;
        }
        if (!*p) break;
    }
    return 0;
}
/* grep -o: leftmost match's span [*ms, *me). With the greedy matcher this is the
 * leftmost-longest match at that start. Returns 1 if a match was found. */
static int gr_match_span(const char *re, const char *t, int ci, const char **ms, const char **me) {
    if (re[0] == '^') { const char *e; if (gr_matchhere(re + 1, t, ci, &e)) { *ms = t; *me = e; return 1; } return 0; }
    for (;; t++) { const char *e; if (gr_matchhere(re, t, ci, &e)) { *ms = t; *me = e; return 1; } if (!*t) return 0; }
}

/* Filename globbing for `ls *.txt` etc.: '*' (any run, greedy with backtrack),
 * '?' (exactly one char), case-insensitive literals. Iterative (no recursion):
 * `star`/`mark` record the last '*' so a failed tail re-stretches it. */
static int glob_match(const char *pat, const char *s) {
    const char *star = 0, *mark = 0;
    while (*s) {
        if (*pat == '*') { star = pat++; mark = s; }                 /* try matching empty first */
        else if (*pat == '?' || gr_lc(*pat) == gr_lc(*s)) { pat++; s++; }
        else if (star) { pat = star + 1; s = ++mark; }               /* backtrack: '*' eats one more */
        else return 0;
    }
    while (*pat == '*') pat++;                                        /* trailing '*'s match empty */
    return *pat == 0;
}

#endif
