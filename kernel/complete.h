/* complete.h — the pure core of the terminal's Tab completion.
 *
 * Factored out of the kernel line editor (kernel/app.c) so it can be unit
 * tested in isolation (tests/complete): it has no kernel dependencies and
 * works over a plain array of name strings rather than the VFS, mirroring the
 * shgrep.h / shmath.h pattern.
 *
 *   complete_match(name, word, plen)
 *       Does `name` begin with the first `plen` characters of `word`, compared
 *       case-insensitively (ASCII)?  Used to pick the candidates.
 *
 *   complete_scan(names, count, word, plen, &nm, &fmi)
 *       Scan `count` names for the ones that complete_match() `word`; return the
 *       length of their longest common prefix (a directory's trailing '/' is
 *       excluded) and report the match count (*out_nm) and the index of the
 *       first match (*out_fmi, -1 if none).  A unique match yields its whole
 *       name length, so the caller fills the name in; several matches yield the
 *       point where they first disagree, i.e. bash's "extend to common prefix".
 */
#pragma once

static inline char complete_lc(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static inline int complete_match(const char *name, const char *word, int plen) {
    for (int k = 0; k < plen; k++)
        if (complete_lc(word[k]) != complete_lc(name[k])) return 0;
    return 1;
}

static inline int complete_scan(const char *const *names, int count,
                                const char *word, int plen,
                                int *out_nm, int *out_fmi) {
    int fmi = -1, nm = 0, cpl = 0;
    for (int i = 0; i < count; i++) {
        if (!complete_match(names[i], word, plen)) continue;
        nm++;
        if (fmi < 0) {                  /* first match seeds the prefix with its whole name */
            fmi = i;
            while (names[i][cpl] && names[i][cpl] != '/') cpl++;
        } else {                        /* later matches shrink it to where they still agree */
            int k = 0;
            while (k < cpl && names[i][k] && complete_lc(names[fmi][k]) == complete_lc(names[i][k]))
                k++;
            cpl = k;
        }
    }
    *out_nm = nm;
    *out_fmi = fmi;
    return cpl;
}
