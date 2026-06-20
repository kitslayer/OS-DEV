/* calc_test.c — host-side regression + fuzz for the calculator app's expression
 * evaluator (user/calceval.h: calc_eval). Built with ASan+UBSan AND -fwrapv (the
 * OS-authored userspace is built with -fwrapv too, so the signed add/sub/mul
 * accumulation wraps — defined — instead of tripping UBSan). The evaluator is
 * pure, so it's unit-testable off-target like shgrep/shmath. Exit 0 = pass.
 *
 * Keep the expected results in sync with user/calceval.h. */
#include <stdio.h>
#include <string.h>
#include "calceval.h"

static long ev(const char *s) { int e; return calc_eval(s, &e); }
static int  bad(const char *s) { int e; calc_eval(s, &e); return e; }   /* 1 if syntax error */

static int fails = 0;
#define CHECK(s, want) do { long got = ev(s); \
    if (got != (long)(want)) { printf("FAIL: %-16s = %ld (want %ld)\n", s, got, (long)(want)); fails++; } } while (0)
#define CHECK_ERR(s) do { if (!bad(s)) { printf("FAIL: %-16s should be a syntax error\n", s); fails++; } } while (0)
#define CHECK_OK(s)  do { if (bad(s))  { printf("FAIL: %-16s should NOT be an error\n", s); fails++; } } while (0)

int main(void) {
    /* --- arithmetic, precedence, parens, literals --- */
    CHECK("2+3", 5);
    CHECK("10*4-2", 38);            /* * before - */
    CHECK("2+3*4", 14);             /* * before + */
    CHECK("(2+3)*4", 20);           /* parens override */
    CHECK("100/7", 14);             /* integer division truncates */
    CHECK("17%5", 2);
    CHECK("-5+2", -3);              /* unary minus */
    CHECK("- -5", 5);               /* double unary */
    CHECK("2*(3+(4-1))", 12);
    CHECK("((1+2)*(3+4))", 21);
    CHECK("100-10-10", 80);         /* left-associative subtraction */
    CHECK("64/4/2", 8);             /* left-associative division */
    CHECK("  3 + 4 ", 7);           /* surrounding / interior whitespace */

    /* --- ^ power (calc uses ^ for power, NOT xor; right-associative, tight) --- */
    CHECK("2^8", 256);
    CHECK("2^0", 1);
    CHECK("2^3^2", 512);            /* right-associative: 2^(3^2)=2^9 */
    CHECK("3*2^3", 24);             /* ^ binds tighter than * : 3*(2^3) */
    CHECK("2^-1", 0);               /* integer: x^(negative) -> 0 */

    /* --- hex literals + bitwise (& | << >> ~) with C-like precedence --- */
    CHECK("0xff", 255);
    CHECK("0x10+1", 17);
    CHECK("0xFF", 255);             /* uppercase hex digits */
    CHECK("~0", -1);
    CHECK("~5", -6);
    CHECK("1<<4", 16);
    CHECK("256>>2", 64);
    CHECK("0xff & 0x0f", 15);
    CHECK("12 | 3", 15);
    CHECK("(1|2)&6", 2);            /* parens override : (3)&6 = 2 */
    CHECK("1+2<<3", 24);            /* + before << : (1+2)<<3 = 24 */
    CHECK("1<<2|1", 5);             /* << before | : (1<<2)|1 = 5 */

    /* --- error cases: bad token, unmatched paren, /0, %0, trailing junk --- */
    CHECK_ERR("");                  /* empty -> no number */
    CHECK_ERR("2+");                /* dangling operator */
    CHECK_ERR("(2+3");              /* unmatched ( */
    CHECK_ERR("2)");                /* trailing ) */
    CHECK_ERR("5/0");               /* divide by zero */
    CHECK_ERR("5%0");               /* mod by zero */
    CHECK_ERR("2 3");               /* trailing junk after a complete expr */
    CHECK_ERR("@");                 /* not a token */
    CHECK_ERR("0x");                /* 0x with no hex digits */
    CHECK_OK("0");                  /* a bare zero is fine */
    CHECK_OK("(0)");

    if (fails) { printf("calc: %d check(s) FAILED\n", fails); return 1; }

    /* --- fuzz: random short expressions must never crash (ASan), trap (UBSan,
     * modulo -fwrapv's defined signed wrap), or hang. calc_eval consumes a
     * bounded string and always terminates, so a violation here is a real bug. --- */
    const char *cs = "0123456789abcdefx+-*/%()^&|<>~ ";
    unsigned seed = 0xC0FFEEu;
    for (int it = 0; it < 400000; it++) {
        char buf[33];
        seed = seed * 1103515245u + 12345u; int n = (seed >> 24) % 32;
        for (int i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; buf[i] = cs[(seed >> 12) % 31u]; }
        buf[n] = 0;
        int e; volatile long r = calc_eval(buf, &e); (void)r; (void)e;
    }

    printf("calc: all evaluator checks passed + 400k fuzz iterations clean\n");
    return 0;
}
