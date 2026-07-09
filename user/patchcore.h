/* patchcore.h — apply a unified-diff patch to a text (M1731).
 *
 * The inverse of diffcore.h's diff_to_patch(): given a source text and a unified
 * diff (---/+++ headers + @@ hunks), produce the patched text. Pure and
 * memcmp-free (drops into the freestanding ring-3 apps), so it is host-unit-
 * tested by tests/diff via a diff -> patch -> apply round-trip. Kept separate
 * from diffcore.h so an app that only generates diffs (gdiff) or only applies
 * them (the shell's `patch`) pulls in just what it uses.
 */
#ifndef PATCHCORE_H
#define PATCHCORE_H

#define PC_MAXLINES 1024

static int pa_eq(const char *a, int al, const char *b, int bl) {   /* memcmp-free line compare */
    if (al != bl) return 0;
    for (int i = 0; i < al; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Apply a unified-diff `patch` to `src`, writing the (\n-terminated) result into
 * out (<= max-1 + NUL). Each @@ header positions its hunk; context (' ') and
 * removed ('-') lines are verified against src there — a mismatch means the patch
 * doesn't apply, so -1 is returned. Lines before the first @@ (the ---/+++
 * headers) are ignored. Returns the output byte length on success. */
static int patch_apply(const char *src, const char *patch, char *out, int max) {
    static int so[PC_MAXLINES], sl[PC_MAXLINES];
    int ns = 0, i = 0;
    while (src[i] && ns < PC_MAXLINES) {
        int st = i; while (src[i] && src[i] != '\n') i++;
        so[ns] = st; sl[ns] = i - st; ns++;
        if (src[i] == '\n') i++;
    }

    int si = 0, po = 0, in_hunk = 0;
    const char *p = patch;
    #define PC_EMIT(ptr, len) do { for (int _k = 0; _k < (len) && po < max - 1; _k++) out[po++] = (ptr)[_k]; if (po < max - 1) out[po++] = '\n'; } while (0)
    while (*p) {
        const char *ls = p; while (*p && *p != '\n') p++;
        int llen = (int)(p - ls);
        if (*p == '\n') p++;
        char c0 = llen ? ls[0] : 0;
        if (c0 == '@' && llen >= 2 && ls[1] == '@') {          /* @@ -as,ac +bs,bc @@ */
            in_hunk = 1;
            const char *q = ls + 2; while (*q == ' ') q++;
            if (*q == '-') q++;
            int as = 0; while (*q >= '0' && *q <= '9') { as = as * 10 + (*q - '0'); q++; }
            int gap = as >= 1 ? as - 1 : 0;                    /* 0-based src index the hunk starts at */
            while (si < gap && si < ns) { PC_EMIT(src + so[si], sl[si]); si++; }
        } else if (!in_hunk) {
            continue;                                          /* ---/+++ or preamble: ignore */
        } else if (c0 == ' ') {                                /* context: must match src */
            if (si >= ns || !pa_eq(src + so[si], sl[si], ls + 1, llen - 1)) return -1;
            PC_EMIT(src + so[si], sl[si]); si++;
        } else if (c0 == '-') {                                /* removed: must match src, drop it */
            if (si >= ns || !pa_eq(src + so[si], sl[si], ls + 1, llen - 1)) return -1;
            si++;
        } else if (c0 == '+') {                                /* added: emit the patch text */
            PC_EMIT(ls + 1, llen - 1);
        } else {
            in_hunk = 0;                                       /* a stray line ends the hunk */
        }
    }
    while (si < ns) { PC_EMIT(src + so[si], sl[si]); si++; }    /* trailing unchanged lines */
    #undef PC_EMIT
    out[po] = 0;
    return po;
}

#endif /* PATCHCORE_H */
