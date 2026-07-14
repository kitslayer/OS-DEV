/* shtxt.h — small pure text-processing helpers for the shell's `tr` and `cut`
 * builtins: tr SET-token expansion (chars + a-z ranges) and cut range-membership.
 * Pure (no syscalls/deps), so host-unit-tested by tests/shtxt while user/shell.c
 * #includes it. NOTE: keep in sync with its host test (tests/shtxt/shtxt_test.c). */
#ifndef SHTXT_H
#define SHTXT_H

/* Expand a `tr` SET token (literal chars + a-z ranges) into out[]; advance *pp past it. */
static int tr_expand(const char **pp, char *out, int max) {
    const char *s = *pp; int o = 0;
    while (*s && *s != ' ' && o < max) {
        if (s[1] == '-' && s[2] && s[2] != ' ' && (unsigned char)s[2] >= (unsigned char)s[0]) {
            for (int c = (unsigned char)s[0]; c <= (unsigned char)s[2] && o < max; c++) out[o++] = (char)c;
            s += 3;
        } else out[o++] = *s++;
    }
    *pp = s;
    return o;
}
/* `cut` field/char selection: is position n in any of the nr ranges? (oe[i] = open-ended) */
static int cut_sel(int n, const int *rf, const int *rt, const int *oe, int nr) {
    for (int i = 0; i < nr; i++) if (n >= rf[i] && (oe[i] || n <= rt[i])) return 1;
    return 0;
}
#endif /* SHTXT_H */
