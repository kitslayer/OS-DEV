/* normpath.h — resolve a shell path: turn `arg` (absolute, or relative to `base`)
 * into a clean absolute path in `out` (<=128 bytes), collapsing '.', '..' and '//'.
 * Used by the shell's `cd` so the displayed cwd matches what the kernel resolved
 * (e.g. `cd ../..` from /aa/bb -> /). Pure + self-contained (no syscalls, no libc),
 * so it's host-unit-tested by tests/normpath; user/shell.c #includes it.
 *
 * Component stack: seed from `base` for a relative arg, then walk `arg` dropping
 * '.' and popping on '..'. Bounded: up to 16 components of <=31 chars each (deeper
 * paths / longer names truncate rather than overflow). */
#ifndef NORMPATH_H
#define NORMPATH_H

static void normpath(const char *base, const char *arg, char *out) {
    char comps[16][32]; int nc = 0;
    const char *srcs[2]; int ns = 0;
    if (arg[0] != '/') srcs[ns++] = base;     /* relative: seed from base first */
    srcs[ns++] = arg;
    for (int s = 0; s < ns; s++) {
        const char *p = srcs[s];
        while (*p) {
            while (*p == '/') p++;
            if (!*p) break;
            char c[32]; int l = 0;
            while (*p && *p != '/' && l < 31) c[l++] = *p++;
            c[l] = 0;
            if (c[0] == '.' && c[1] == 0) continue;                                      /* "."  : current dir, skip */
            if (c[0] == '.' && c[1] == '.' && c[2] == 0) { if (nc > 0) nc--; continue; }  /* ".." : pop one (floor at root) */
            if (nc < 16) { int k = 0; while (c[k] && k < 31) { comps[nc][k] = c[k]; k++; } comps[nc][k] = 0; nc++; }
        }
    }
    int o = 0; out[o++] = '/';
    for (int i = 0; i < nc; i++) {
        if (i > 0 && o < 126) out[o++] = '/';
        for (int k = 0; comps[i][k] && o < 126; k++) out[o++] = comps[i][k];
    }
    out[o] = 0;
}

#endif /* NORMPATH_H */
