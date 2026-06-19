/* shmath_test.c — host-side regression + fuzz for the shell's $((expr)) integer
 * evaluator (user/shmath.h). Built with ASan+UBSan. The evaluator is pure apart
 * from the sh_var() hook, which we stub here. Exit 0 = pass.
 *
 * Keep the expected results in sync with user/shmath.h (and shell.c's usage). */
#include <stdio.h>
#include <string.h>
#include "shmath.h"

/* stubbed variable table for the test: a=2, b=3, ten=10, anything else = 0 */
static long sh_var(const char *name, int len) {
    if (len == 1 && name[0] == 'a') return 2;
    if (len == 1 && name[0] == 'b') return 3;
    if (len == 3 && !strncmp(name, "ten", 3)) return 10;
    return 0;
}

static long ev(const char *s) { const char *p = s; return sh_eval(&p); }

static int fails = 0;
#define CHECK(s, want) do { long got = ev(s); \
    if (got != (long)(want)) { printf("FAIL: %-16s = %ld (want %ld)\n", s, got, (long)(want)); fails++; } } while (0)

int main(void) {
    /* --- regression: operators, precedence, parens, literals, variables --- */
    CHECK("2+3", 5);
    CHECK("10*4-2", 38);            /* * before - */
    CHECK("2+3*4", 14);             /* * before + */
    CHECK("(2+3)*4", 20);           /* parens override */
    CHECK("100/7", 14);             /* integer division truncates */
    CHECK("17%5", 2);
    CHECK("-5+2", -3);              /* unary minus */
    CHECK("- -5", 5);               /* double unary */
    CHECK("+7", 7);                 /* unary plus */
    CHECK("2+3*4-1", 13);
    CHECK("((1+2)*(3+4))", 21);
    CHECK("2*(3+(4-1))", 12);
    CHECK("0xff", 255);             /* hex literal */
    CHECK("0x10+1", 17);
    CHECK("5/0", 0);                /* divide by zero -> 0, no trap */
    CHECK("5%0", 0);
    CHECK("a+b", 5);                /* bare variable names */
    CHECK("a*b+1", 7);
    CHECK("$a+$b", 5);              /* $-prefixed names */
    CHECK("ten*ten", 100);
    CHECK("nosuch+1", 1);           /* unknown variable -> 0 */
    CHECK("  3  +  4 ", 7);         /* surrounding whitespace */
    CHECK("", 0);                   /* empty expression */
    CHECK("()", 0);
    CHECK("100-10-10", 80);         /* left-associative subtraction */
    CHECK("64/4/2", 8);             /* left-associative division */

    /* --- bitwise / shift / power (bash $(()) operators + precedence) --- */
    CHECK("1<<4", 16);              /* shift left */
    CHECK("256>>2", 64);            /* shift right */
    CHECK("0xff & 0x0f", 15);       /* bitwise AND */
    CHECK("12 | 3", 15);            /* bitwise OR */
    CHECK("5 ^ 3", 6);              /* bitwise XOR (NOT power) */
    CHECK("~0", -1);                /* bitwise NOT */
    CHECK("~5", -6);
    CHECK("2**8", 256);             /* power */
    CHECK("2**3**2", 512);          /* power is right-associative: 2**(3**2)=2**9 */
    CHECK("3*2**3", 24);            /* ** binds tighter than * : 3*(2**3) */
    CHECK("1+2<<3", 24);            /* + before << : (1+2)<<3 = 24 */
    CHECK("1<<2|1", 5);             /* << before | : (1<<2)|1 = 5 */
    CHECK("0xf0 | 0x0f & 0x33", 0xf3); /* & before | : 0xf0 | (0x0f & 0x33) = 0xf0|0x03 */
    CHECK("(1|2)&6", 2);            /* parens override : (3)&6 = 2 */
    CHECK("2**0", 1);
    CHECK("10 % 3", 1);

    /* --- fuzz: random short expressions must never crash, trap (ASan/UBSan),
     * or hang. sh_eval consumes a bounded string left-to-right and always
     * terminates, so a violation here is a real bug. --- */
    const char *cs = "0123456789+-*/%()xab $\t<>&^|~";
    unsigned seed = 0x9e3779b9u;
    for (int it = 0; it < 300000; it++) {
        char buf[33];
        seed = seed * 1103515245u + 12345u; int n = (seed >> 24) % 32;
        for (int i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; buf[i] = cs[(seed >> 16) % (unsigned)(strlen(cs))]; }
        buf[n] = 0;
        const char *p = buf; volatile long r = sh_eval(&p); (void)r;
    }

    if (fails) { printf("shmath: %d check(s) FAILED\n", fails); return 1; }
    printf("shmath: all arithmetic checks passed + 300k fuzz iterations clean\n");
    return 0;
}
