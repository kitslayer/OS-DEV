/* shgrep.h — the shell's tiny grep regex matcher: ^ $ . * [..] character
 * classes, \ escapes, and the -i case fold (Kernighan/Pike style). A pattern
 * with no metacharacters behaves exactly like a literal substring search, so
 * existing greps are unaffected. Pure (no syscalls), so it's host-unit-tested by
 * tests/shgrep; user/shell.c #includes it for the `grep` command. */
#ifndef SHGREP_H
#define SHGREP_H

static char gr_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int gr_matchhere(const char *re, const char *t, int ci);

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
static int gr_matchstar(int c, const char *re, const char *t, int ci) {
    do { if (gr_matchhere(re, t, ci)) return 1; }
    while (*t && (c == '.' || (ci ? gr_lc(*t) == gr_lc((char)c) : (unsigned char)*t == (unsigned char)c)) && (t++, 1));
    return 0;
}
static int gr_matchhere(const char *re, const char *t, int ci) {
    if (re[0] == '\0') return 1;
    if (re[0] == '[') {                                            /* [..] character class, incl. [..]* */
        int cl = gr_classlen(re);
        if (re[cl] == '*') { const char *rest = re + cl + 1;
            do { if (gr_matchhere(rest, t, ci)) return 1; } while (*t && gr_inclass(re, (unsigned char)*t, ci) && (t++, 1));
            return 0; }
        if (*t && gr_inclass(re, (unsigned char)*t, ci)) return gr_matchhere(re + cl, t + 1, ci);
        return 0;
    }
    if (re[0] == '\\' && re[1]) {                                  /* escaped literal: \. \* \^ … */
        if (re[2] == '*') return gr_matchstar((unsigned char)re[1], re + 3, t, ci);
        if (*t && (ci ? gr_lc(*t) == gr_lc(re[1]) : *t == re[1])) return gr_matchhere(re + 2, t + 1, ci);
        return 0;
    }
    if (re[1] == '*') return gr_matchstar((unsigned char)re[0], re + 2, t, ci);
    if (re[0] == '$' && re[1] == '\0') return *t == '\0';
    if (*t && (re[0] == '.' || (ci ? gr_lc(*t) == gr_lc(re[0]) : (unsigned char)*t == (unsigned char)re[0])))
        return gr_matchhere(re + 1, t + 1, ci);
    return 0;
}
static int gr_match(const char *re, const char *t, int ci) {     /* match anywhere (^ anchors to start) */
    if (re[0] == '^') return gr_matchhere(re + 1, t, ci);
    do { if (gr_matchhere(re, t, ci)) return 1; } while (*t++);
    return 0;
}

#endif
