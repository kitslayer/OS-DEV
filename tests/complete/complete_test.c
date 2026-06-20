/* complete_test.c — host-side regression for the terminal's Tab-completion core
 * (kernel/complete.h). The logic is pure (plain char arrays, no kernel deps),
 * so it unit-tests off-target like the shell's shgrep/shmath extractions and
 * the browser's cssprop/url/htmlentity ones. Exit 0 = pass.
 *
 * Keep the expectations in sync with kernel/complete.h and its use in
 * kernel/app.c (the kernel line editor). */
#include <stdio.h>
#include "complete.h"

static int fails = 0;

/* Assert complete_scan over the in-scope `names[]` for `word`/`plen` yields the
 * given common-prefix length, match count, and first-match index. */
#define SCAN(word, plen, want_cpl, want_nm, want_fmi) do {                      \
        int nm = -7, fmi = -7;                                                  \
        int cpl = complete_scan(names, (int)(sizeof names / sizeof names[0]),   \
                                word, plen, &nm, &fmi);                         \
        if (cpl != (want_cpl) || nm != (want_nm) || fmi != (want_fmi)) {        \
            printf("FAIL: scan(\"%s\",%d) -> cpl=%d nm=%d fmi=%d "              \
                   "(want cpl=%d nm=%d fmi=%d)\n",                              \
                   word, plen, cpl, nm, fmi, (want_cpl), (want_nm), (want_fmi));\
            fails++;                                                            \
        }                                                                       \
    } while (0)

#define MATCH(name, word, plen, want) do {                                     \
        int got = complete_match(name, word, plen);                            \
        if (got != (want)) {                                                   \
            printf("FAIL: match(\"%s\",\"%s\",%d) = %d (want %d)\n",           \
                   name, word, plen, got, (want)); fails++;                    \
        }                                                                       \
    } while (0)

int main(void) {
    /* --- complete_match: case-insensitive prefix --- */
    MATCH("README.TXT", "READ", 4, 1);
    MATCH("readme.txt", "READ", 4, 1);     /* lowercase name, uppercase word */
    MATCH("README.TXT", "read", 4, 1);     /* uppercase name, lowercase word */
    MATCH("HELLO.TXT",  "READ", 4, 0);     /* mismatch */
    MATCH("README.TXT", "README.TXT", 10, 1);
    MATCH("HE",         "HELLO", 5, 0);    /* word longer than name: no OOB, no match */
    MATCH("anything",   "x", 0, 1);        /* empty prefix matches everything */

    /* --- complete_scan: unique match yields the whole name length --- */
    {
        const char *names[] = { "HELLO.TXT", "MOTD.TXT", "DEMO.SH" };
        SCAN("HEL", 3, 9, 1, 0);           /* HELLO.TXT unique -> cpl=9 */
        SCAN("MO",  2, 8, 1, 1);           /* MOTD.TXT unique  -> cpl=8, first match idx 1 */
        SCAN("ZZ",  2, 0, 0, -1);          /* no match */
    }

    /* --- two entries sharing a prefix -> extend to the common prefix --- */
    {
        const char *names[] = { "README.TXT", "README.MD" };
        SCAN("READ", 4, 7, 2, 0);          /* common prefix "README." (7 chars) */
        SCAN("README.", 7, 7, 2, 0);       /* already at the prefix: no further extension */
        SCAN("README.T", 8, 10, 1, 0);     /* now unique -> the whole "README.TXT" */
    }

    /* --- variable-length names: one name being a prefix of another --- */
    {
        const char *names[] = { "HELP", "HELPER" };
        SCAN("HE", 2, 4, 2, 0);            /* "HELP" is a prefix of "HELPER" -> 4 */
    }
    {
        const char *names[] = { "ABC", "ABD" };
        SCAN("A", 1, 2, 2, 0);             /* common "AB" (2) */
    }

    /* --- case-insensitive common prefix, emitted from the first match's bytes --- */
    {
        const char *names[] = { "Report1", "report2" };
        SCAN("re", 2, 6, 2, 0);            /* "Report"/"report" agree case-insensitively up to 6 */
    }

    /* --- a directory's trailing '/' is excluded from the completed prefix --- */
    {
        const char *names[] = { "DOCS/", "DATA.CSV" };
        SCAN("DO", 2, 4, 1, 0);            /* "DOCS" (4), slash dropped, unique */
    }
    {
        const char *names[] = { "DIR1/", "DIR2/" };
        SCAN("DI", 2, 3, 2, 0);            /* common "DIR" (3), slashes excluded */
    }

    /* --- empty word (the `cmd <Tab>` case): every entry matches --- */
    {
        const char *names[] = { "alpha", "albatross", "album" };
        SCAN("", 0, 2, 3, 0);              /* common prefix of all = "al" (2) */
    }
    {
        const char *names[] = { "xenon", "yellow" };
        SCAN("", 0, 0, 2, 0);              /* no common prefix -> 0 (caller then lists) */
    }

    if (fails) { printf("%d CHECK(s) FAILED\n", fails); return 1; }
    printf("all complete.h checks passed\n");
    return 0;
}
