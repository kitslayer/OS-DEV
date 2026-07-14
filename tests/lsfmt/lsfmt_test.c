/* lsfmt_test.c — host-side regression for the `ls -l` formatting helpers
 * (user/lsfmt.h): the mode string and the mtime date column. Pure, so built for
 * the host under ASan+UBSan. Exit 0 = pass. Keep in sync with user/lsfmt.h. */
#include <stdio.h>
#include <string.h>
#include "lsfmt.h"

static int fails = 0, checks = 0;
#define EQ(got, want) do { checks++; if (strcmp((got), (want)) != 0) { \
    printf("FAIL: got \"%s\" want \"%s\"\n", (got), (want)); fails++; } } while (0)

int main(void) {
    char b[24];
    /* --- mode string --- */
    ls_mode_str(S_IFDIR | 0755, b); EQ(b, "drwxr-xr-x");
    ls_mode_str(S_IFREG | 0644, b); EQ(b, "-rw-r--r--");
    ls_mode_str(S_IFREG | 0777, b); EQ(b, "-rwxrwxrwx");
    ls_mode_str(S_IFREG | 0000, b); EQ(b, "----------");
    ls_mode_str(S_IFLNK | 0777, b); EQ(b, "lrwxrwxrwx");
    ls_mode_str(S_IFDIR | 0700, b); EQ(b, "drwx------");
    ls_mode_str(S_IFREG | 0751, b); EQ(b, "-rwxr-x--x");

    /* --- mtime column: known UTC epochs --- */
    ls_fmt_time(0UL, b);          EQ(b, "1970-01-01 00:00");
    ls_fmt_time(86400UL, b);      EQ(b, "1970-01-02 00:00");
    ls_fmt_time(3661UL, b);       EQ(b, "1970-01-01 01:01");
    ls_fmt_time(1700000000UL, b); EQ(b, "2023-11-14 22:13");
    ls_fmt_time(1577836800UL, b); EQ(b, "2020-01-01 00:00");   /* leap-year boundary */
    ls_fmt_time(1582934400UL, b); EQ(b, "2020-02-29 00:00");   /* the leap day */
    ls_fmt_time(1609459199UL, b); EQ(b, "2020-12-31 23:59");   /* last minute of 2020 */

    if (fails) { printf("lsfmt: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("lsfmt: all %d ls -l formatting checks passed\n", checks);
    return 0;
}
