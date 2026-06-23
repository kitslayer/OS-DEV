/* shsed.h — the `sed s/RE/REPL/` substitution engine, factored out of shell.c
 * so it can be host-unit-tested (tests/shsed). Pure (no syscalls): applies one
 * substitution to a single line into a caller buffer. The RE uses the grep
 * regex matcher in shgrep.h (^ $ . * [..] \), so sed and grep share one engine.
 *
 * REPL: `&` = the whole match, `\&`/`\\`/`\<c>` = a literal. `g` = every
 * (non-overlapping) match, else just the first. `ci` folds case. Output is
 * strictly bounds-checked: a pathological line truncates cleanly at `cap`
 * rather than overflowing `out`. */
#ifndef SHSED_H
#define SHSED_H
#include "shgrep.h"   /* gr_match_span(): leftmost regex match span [ms, me) */

static void sed_sub(const char *line, const char *re, const char *repl,
                    int g, int ci, char *out, long cap) {
    long o = 0;
    const char *p = line;
    const char *prev_end = (const char *)0;   /* end of the previous emitted match */
    /* NB: SED_PUT must not be given a side-effecting argument — its body is
     * skipped once the buffer is full, so a `SED_PUT(*p++)` would stop advancing
     * `p` and spin forever on a truncated line. Always step pointers separately. */
#define SED_PUT(ch) do { if (o < cap - 1) out[o++] = (char)(ch); } while (0)
    for (;;) {
        const char *ms, *me;
        if (!gr_match_span(re, p, ci, &ms, &me)) break;       /* no more matches */
        if (ms == me && ms == prev_end) {                     /* empty match right after the last match: skip it (GNU rule) */
            if (!*p) break;
            SED_PUT(*p); p++;
            continue;
        }
        while (p < ms) { SED_PUT(*p); p++; }                  /* copy text before the match */
        for (const char *r = repl; *r; ) {                    /* expand the replacement */
            if (*r == '&') { for (const char *m = ms; m < me; m++) SED_PUT(*m); r++; }
            else if (*r == '\\' && r[1]) { SED_PUT(r[1]); r += 2; }
            else { SED_PUT(*r); r++; }
        }
        prev_end = me;
        if (me == ms) {                                       /* empty match: emit one char so we make progress */
            if (!*me) { p = me; break; }
            SED_PUT(*me); p = me + 1;
        } else p = me;
        if (!g) break;                                        /* single substitution */
    }
    while (*p) { SED_PUT(*p); p++; }                          /* copy the tail after the last match */
    out[o] = 0;
#undef SED_PUT
}

#endif
