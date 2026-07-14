/* shtest_test.c — host-side regression for the shell's `test` / `[ ]` conditional
 * evaluator (user/shtest.h). Built with ASan+UBSan. The evaluator is pure apart
 * from the sh_test_file() file-probe hook (mocked here) and shmath's sh_var
 * (stubbed). Exit 0 = pass. Keep in sync with user/shtest.h + shell.c's usage. */
#include <stdio.h>
#include <string.h>
#include "shtest.h"

/* shmath.h's variable hook — unused by the test evaluator, stub to 0. */
static long sh_var(const char *name, int len) { (void)name; (void)len; return 0; }

/* Mock filesystem for the file-test primaries: "F1" a non-empty regular file,
 * "EMPTY" an empty regular file, "D1" a directory, everything else missing. */
static int sh_test_file(char op, const char *path, const char *cwd) {
    (void)cwd;
    int isdir = !strcmp(path, "D1");
    int isfile = !strcmp(path, "F1") || !strcmp(path, "EMPTY");
    int nonempty = !strcmp(path, "F1");
    int exists = isdir || isfile;
    switch (op) {
        case 'd': return isdir;
        case 'e': return exists;
        case 'f': return isfile;
        case 's': return nonempty;
        case 'r': case 'w': case 'x': return exists;
    }
    return 0;
}

static int fails = 0, checks = 0;
/* Split a space-separated string into argv and evaluate (no empty/quoted tokens). */
static int ev(const char *expr) {
    static char buf[256]; char *av[32]; int ac = 0, n = 0;
    for (const char *s = expr; *s && n < 255; s++) buf[n++] = *s;
    buf[n] = 0;
    char *p = buf;
    while (*p && ac < 32) {
        while (*p == ' ') p++;
        if (!*p) break;
        av[ac++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    return sh_test_eval(av, ac, "/");
}
#define T(expr, want) do { checks++; int g = ev(expr); \
    if (g != (want)) { printf("FAIL: [ %-30s ] = %d (want %d)\n", expr, g, want); fails++; } } while (0)
#define TA(want, ...) do { checks++; char *av[] = { __VA_ARGS__ }; \
    int g = sh_test_eval(av, (int)(sizeof(av)/sizeof(av[0])), "/"); \
    if (g != (want)) { printf("FAIL: manual argv = %d (want %d)\n", g, want); fails++; } } while (0)

int main(void) {
    /* string equality / inequality (incl. the == bash alias) */
    T("abc = abc", 1); T("abc = abd", 0); T("abc == abc", 1); T("abc == abd", 0);
    T("abc != abd", 1); T("abc != abc", 0);
    T("= = =", 1);                          /* "=" equals "=" */

    /* numeric comparisons */
    T("5 -eq 5", 1); T("5 -eq 6", 0); T("5 -ne 6", 1); T("5 -ne 5", 0);
    T("5 -lt 9", 1); T("9 -lt 5", 0); T("10 -gt 2", 1); T("2 -gt 10", 0);
    T("3 -le 3", 1); T("4 -le 3", 0); T("3 -ge 4", 0); T("4 -ge 4", 1);
    T("-3 -lt -2", 1);                      /* signed operands */
    T("0x10 -eq 16", 1);                    /* hex operand via sh_str2long */

    /* string presence: bare STR, -z (empty), -n (non-empty) */
    T("abc", 1);
    TA(0, "");                              /* [ "" ] -> false */
    TA(1, "-z", "");                        /* -z "" -> true */
    TA(0, "-z", "x");                       /* -z "x" -> false */
    TA(1, "-n", "x");                       /* -n "x" -> true */
    TA(0, "-n", "");                        /* -n "" -> false */

    /* negation */
    T("! abc = abc", 0); T("! abc = abd", 1); T("! 5 -eq 6", 1); T("! ! abc", 1);

    /* file tests (against the mock filesystem) */
    T("-e F1", 1); T("-e D1", 1); T("-e NOPE", 0);
    T("-f F1", 1); T("-f D1", 0); T("-f NOPE", 0);
    T("-d D1", 1); T("-d F1", 0);
    T("-s F1", 1); T("-s EMPTY", 0);        /* -s: non-empty regular file */
    T("-r F1", 1); T("-w D1", 1); T("-x NOPE", 0);

    /* -a (AND, binds tighter) / -o (OR) combiners + precedence */
    T("1 -eq 1 -a 2 -eq 2", 1);
    T("1 -eq 1 -a 2 -eq 3", 0);
    T("1 -eq 2 -o 3 -eq 3", 1);
    T("1 -eq 2 -o 3 -eq 4", 0);
    T("1 -eq 1 -o 2 -eq 2 -a 3 -eq 9", 1);  /* -a tighter: T OR (T AND F) = T */
    T("1 -eq 2 -a 3 -eq 3 -o 4 -eq 4", 1);  /* (F AND T) OR T = T */
    T("1 -eq 2 -a 3 -eq 3 -o 4 -eq 9", 0);  /* (F AND T) OR F = F */
    T("-f F1 -a -d D1", 1);
    T("-f F1 -a -d F1", 0);
    T("-f NOPE -o -d D1", 1);
    T("! 1 -eq 1 -a 2 -eq 2", 0);           /* (!T) AND T = F */

    if (fails) { printf("shtest: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("shtest: all %d test/[ ] checks passed\n", checks);
    return 0;
}
