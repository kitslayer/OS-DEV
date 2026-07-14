/* shsort_test.c — host-side regression for the shell's `sort` key helpers
 * (user/shsort.h): numeric-key parsing, fold equality, field extraction. Pure
 * (only gr_lc from shgrep.h), built for the host under ASan+UBSan. Exit 0 = pass.
 * Keep in sync with user/shsort.h + shell.c's `sort` builtin. */
#include <stdio.h>
#include <string.h>
#include "shsort.h"

static int fails = 0, checks = 0;
#define EQL(got, want) do { checks++; long g = (long)(got); if (g != (long)(want)) { printf("FAIL: got %ld want %ld\n", g, (long)(want)); fails++; } } while (0)
#define EQS(got, want) do { checks++; const char *g = (got); if (strcmp(g, (want)) != 0) { printf("FAIL: got \"%s\" want \"%s\"\n", g, (want)); fails++; } } while (0)
#define TRUE(c, m) do { checks++; if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main(void) {
    /* --- sort_numval: fixed-point x1e6, sign + up to 6 decimals (M1838) --- */
    EQL(sort_numval("5"), 5000000);
    EQL(sort_numval("10"), 10000000);
    EQL(sort_numval("3.14"), 3140000);
    EQL(sort_numval("3.2"), 3200000);
    EQL(sort_numval("-1.5"), -1500000);
    EQL(sort_numval("+5"), 5000000);
    EQL(sort_numval("  42"), 42000000);              /* leading whitespace skipped */
    EQL(sort_numval("3.14159265"), 3141592);         /* clamped to 6 decimal places */
    EQL(sort_numval("abc"), 0);                      /* no digits -> 0 */
    /* the ordering property `sort -n` relies on */
    TRUE(sort_numval("3.14") < sort_numval("3.2"), "3.14 < 3.2");
    TRUE(sort_numval("3.2") < sort_numval("10"),   "3.2 < 10");
    TRUE(sort_numval("-1.5") < sort_numval("0"),   "-1.5 < 0");
    TRUE(sort_numval("2.9") > sort_numval("2.1"),  "2.9 > 2.1 (decimals distinguish)");

    /* --- sort_foldeq: case-insensitive equality (sort -uf) --- */
    EQL(sort_foldeq("abc", "abc"), 1);
    EQL(sort_foldeq("ABC", "abc"), 1);
    EQL(sort_foldeq("abc", "abd"), 0);
    EQL(sort_foldeq("", ""), 1);
    EQL(sort_foldeq("abc", "ab"), 0);

    /* --- sort_field: field k, whitespace or a custom delimiter (sort -kN [-td]) --- */
    EQS(sort_field("a b c", 1, 0), "a b c");
    EQS(sort_field("a b c", 2, 0), "b c");
    EQS(sort_field("a b c", 3, 0), "c");
    EQS(sort_field("a b c", 5, 0), "");              /* past the last field */
    EQS(sort_field("  a  b", 2, 0), "b");            /* leading blanks of field k skipped */
    EQS(sort_field("a:b:c", 1, ':'), "a:b:c");
    EQS(sort_field("a:b:c", 2, ':'), "b:c");
    EQS(sort_field("a:b:c", 3, ':'), "c");

    if (fails) { printf("shsort: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("shsort: all %d sort-helper checks passed\n", checks);
    return 0;
}
