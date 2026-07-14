/* shtest.h — the shell's `test` / `[ ... ]` conditional-expression evaluator.
 * Primaries: STR (true iff non-empty), -z/-n STR, -e/-f/-d/-s/-r/-w/-x FILE,
 * and A OP B where OP is = == != (string) or -eq -ne -lt -gt -le -ge (numeric);
 * a leading `!` negates a primary. Primaries are joined by -a (logical AND, binds
 * tighter) and -o (logical OR). Pure apart from sh_test_file() (the file
 * existence/type probe), which the includer provides — so it is host-unit-tested
 * by tests/shtest, and user/shell.c #includes it for the `test`/`[ ]` builtin.
 *
 * NOTE: keep in sync with its host test (tests/shtest/shtest_test.c). (M1816) */
#ifndef SHTEST_H
#define SHTEST_H
#include "shmath.h"   /* sh_str2long() (and shmath's own sh_var hook, stubbed by the host test) */

/* Provided by the includer: 1 if the file-test `op` (one of e/f/d/s/r/w/x) holds
 * for `path` (resolved relative to cwd). */
static int sh_test_file(char op, const char *path, const char *cwd);

static int sht_eq(const char *a, const char *b) { int i = 0; while (a[i] && a[i] == b[i]) i++; return a[i] == 0 && b[i] == 0; }
static int sh_test_isunop(const char *s) {
    return s[0] == '-' && s[1] && s[2] == 0 &&
           (s[1]=='z'||s[1]=='n'||s[1]=='d'||s[1]=='e'||s[1]=='f'||s[1]=='s'||s[1]=='r'||s[1]=='w'||s[1]=='x');
}
static int sh_test_isbinop(const char *s) {
    return sht_eq(s,"=")||sht_eq(s,"==")||sht_eq(s,"!=")||sht_eq(s,"-eq")||sht_eq(s,"-ne")||
           sht_eq(s,"-lt")||sht_eq(s,"-gt")||sht_eq(s,"-le")||sht_eq(s,"-ge");
}
/* Evaluate one primary starting at av[*ip], advancing *ip past the tokens it consumes. */
static int sh_test_prim(char **av, int ac, int *ip, const char *cwd) {
    int neg = 0;
    while (*ip < ac && sht_eq(av[*ip], "!")) { neg = !neg; (*ip)++; }
    int rem = ac - *ip, res = 0;
    if (rem >= 3 && sh_test_isbinop(av[*ip+1])) {                 /* A OP B */
        const char *a = av[*ip], *op = av[*ip+1], *b = av[*ip+2]; *ip += 3;
        if (sht_eq(op,"=") || sht_eq(op,"==")) res = sht_eq(a, b);
        else if (sht_eq(op,"!=")) res = !sht_eq(a, b);
        else { long x = sh_str2long(a), y = sh_str2long(b);
            if (sht_eq(op,"-eq")) res = (x==y); else if (sht_eq(op,"-ne")) res = (x!=y);
            else if (sht_eq(op,"-lt")) res = (x<y); else if (sht_eq(op,"-gt")) res = (x>y);
            else if (sht_eq(op,"-le")) res = (x<=y); else res = (x>=y); }
    } else if (rem >= 2 && sh_test_isunop(av[*ip])) {            /* -OP OPERAND */
        char op = av[*ip][1]; const char *a = av[*ip+1]; *ip += 2;
        if (op == 'z') res = (a[0] == 0);
        else if (op == 'n') res = (a[0] != 0);
        else res = sh_test_file(op, a, cwd);
    } else if (rem >= 1) {                                       /* STR: true iff non-empty */
        res = (av[*ip][0] != 0); (*ip)++;
    }
    return neg ? !res : res;
}
/* The whole expression: primaries joined by -a (AND, binds tighter) / -o (OR). */
static int sh_test_eval(char **av, int ac, const char *cwd) {
    int i = 0, orv = 0;
    for (;;) {
        int andv = sh_test_prim(av, ac, &i, cwd);
        while (i < ac && sht_eq(av[i], "-a")) { i++; int r = sh_test_prim(av, ac, &i, cwd); andv = andv && r; }
        orv = orv || andv;
        if (i < ac && sht_eq(av[i], "-o")) { i++; continue; }
        break;
    }
    return orv;
}
#endif /* SHTEST_H */
