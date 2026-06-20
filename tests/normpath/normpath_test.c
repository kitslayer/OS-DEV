/* normpath_test.c — host-side regression + fuzz for the shell's cd path resolver
 * (user/normpath.h: normpath). Built with ASan+UBSan. The resolver is pure and
 * self-contained, so it's unit-testable off-target like shgrep/shmath/shsplit.
 * Exit 0 = pass. Keep the expected results in sync with user/normpath.h. */
#include <stdio.h>
#include <string.h>
#include "normpath.h"

static int fails = 0;
#define CHECK(base, arg, want) do { char got[128]; normpath(base, arg, got); \
    if (strcmp(got, want)) { printf("FAIL: normpath(%-10s, %-12s) = %-12s (want %s)\n", base, arg, got, want); fails++; } } while (0)

int main(void) {
    /* --- absolute args ignore the base --- */
    CHECK("/aa/bb", "/x/y", "/x/y");
    CHECK("/aa/bb", "/",    "/");
    CHECK("/zzz",   "/a/../b", "/b");

    /* --- relative args resolve against the base --- */
    CHECK("/aa/bb", "c",   "/aa/bb/c");
    CHECK("/",      "a/b/c", "/a/b/c");
    CHECK("/aa",    "",    "/aa");            /* empty arg keeps the base */

    /* --- '.' (skip) and '..' (pop, floor at root) --- */
    CHECK("/aa/bb", "..",     "/aa");
    CHECK("/aa/bb", "../..",  "/");
    CHECK("/aa/bb", "../../..", "/");         /* pop past root stays at root */
    CHECK("/",      "..",     "/");
    CHECK("/aa",    ".",      "/aa");
    CHECK("/aa",    "./x",    "/aa/x");
    CHECK("/aa/bb", "../cc",  "/aa/cc");      /* up then into a sibling */

    /* --- '//' collapses; trailing slash dropped --- */
    CHECK("/",      "a//b",   "/a/b");
    CHECK("/",      "a/",     "/a");
    CHECK("/",      "//x//",  "/x");

    /* --- a mix of everything --- */
    CHECK("/aa/bb", "../../x/./y/../z", "/x/z");
    CHECK("/a/b/c", "../../..",         "/");
    CHECK("/a/b/c", "../../../../d",    "/d");

    if (fails) { printf("normpath: %d check(s) FAILED\n", fails); return 1; }

    /* --- fuzz: random base+arg from a path-heavy charset must never crash
     * (ASan/UBSan), and the result must always be a NUL-terminated absolute path
     * (out[0]=='/', length < 128). normpath consumes bounded strings and always
     * terminates, so a violation here is a real bug. --- */
    const char *cs = "ab./";
    unsigned seed = 0x5EED1234u;
    for (int it = 0; it < 400000; it++) {
        char base[24], arg[24], out[128];
        seed = seed * 1103515245u + 12345u; int nb = (seed >> 24) % 23;
        for (int i = 0; i < nb; i++) { seed = seed * 1103515245u + 12345u; base[i] = cs[(seed >> 12) % 4u]; }
        base[nb] = 0;
        seed = seed * 1103515245u + 12345u; int na = (seed >> 24) % 23;
        for (int i = 0; i < na; i++) { seed = seed * 1103515245u + 12345u; arg[i] = cs[(seed >> 12) % 4u]; }
        arg[na] = 0;
        normpath(base, arg, out);
        if (out[0] != '/') { printf("FUZZ FAIL: result not absolute for base=[%s] arg=[%s] -> [%s]\n", base, arg, out); return 1; }
        if (strlen(out) >= 128) { printf("FUZZ FAIL: result overruns 128 for base=[%s] arg=[%s]\n", base, arg); return 1; }
    }

    printf("normpath: all path checks passed + 400k fuzz iterations clean\n");
    return 0;
}
