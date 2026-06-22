/* calc_test.c — host-side regression + fuzz for the calculator app's
 * floating-point SCIENTIFIC expression evaluator (user/calceval.h: calc_eval).
 * Built with ASan+UBSan AND -fwrapv (the OS-authored userspace is built with
 * -fwrapv too, so the long bitwise accumulation wraps — defined — instead of
 * tripping UBSan). The evaluator is pure, so it's unit-testable off-target like
 * shgrep/shmath. Exit 0 = pass.
 *
 * Results are doubles now, so float checks use a tolerance (CHECK_F: pass if
 * |got-want| < 1e-9, or within a relative epsilon for large magnitudes).
 * Keep the expected results in sync with user/calceval.h. */
#include <stdio.h>
#include <string.h>
#include "calceval.h"

static double ev(const char *s) { int e; return calc_eval(s, &e); }
static int    bad(const char *s) { int e; calc_eval(s, &e); return e; }   /* 1 if syntax error */

static int fails = 0;
static double dabs(double x){ return x < 0 ? -x : x; }
/* float-tolerant equality: absolute 1e-9, or relative 1e-9 for big values */
static int near(double got, double want) {
    double d = dabs(got - want);
    if (d < 1e-9) return 1;
    double scale = dabs(want); if (scale < 1.0) scale = 1.0;
    return d < 1e-9 * scale;
}
#define CHECK_F(s, want) do { double got = ev(s); \
    if (!near(got, (double)(want))) { printf("FAIL: %-18s = %.17g (want %.17g)\n", s, got, (double)(want)); fails++; } } while (0)
#define CHECK_ERR(s) do { if (!bad(s)) { printf("FAIL: %-18s should be a syntax error\n", s); fails++; } } while (0)
#define CHECK_OK(s)  do { if (bad(s))  { printf("FAIL: %-18s should NOT be an error\n", s); fails++; } } while (0)
#define CHECK_NAN(s) do { double got = ev(s); if (!(got != got)) { printf("FAIL: %-18s = %.17g (want NaN)\n", s, got); fails++; } } while (0)
#define CHECK_INF(s, sign) do { double got = ev(s); double w = (sign) * (1.0/0.0); \
    if (!(got == w)) { printf("FAIL: %-18s = %.17g (want %sInf)\n", s, got, (sign)<0?"-":"+"); fails++; } } while (0)

int main(void) {
    /* --- arithmetic, precedence, parens, literals (now floating point) --- */
    CHECK_F("2+3", 5);
    CHECK_F("10*4-2", 38);            /* * before - */
    CHECK_F("2+3*4", 14);             /* * before + */
    CHECK_F("(2+3)*4", 20);           /* parens override */
    CHECK_F("7/2", 3.5);              /* REAL division now */
    CHECK_F("100/7", 100.0/7.0);      /* 14.2857... */
    CHECK_F("10%3", 1);               /* fmod */
    CHECK_F("17%5", 2);
    CHECK_F("-5+2", -3);              /* unary minus */
    CHECK_F("- -5", 5);               /* double unary */
    CHECK_F("2*(3+(4-1))", 12);
    CHECK_F("((1+2)*(3+4))", 21);
    CHECK_F("100-10-10", 80);         /* left-associative subtraction */
    CHECK_F("64/4/2", 8);             /* left-associative division */
    CHECK_F("  3 + 4 ", 7);           /* surrounding / interior whitespace */

    /* --- decimal literals: fraction, leading dot, exponent --- */
    CHECK_F("3.14", 3.14);
    CHECK_F(".5", 0.5);
    CHECK_F("0.25*4", 1);
    CHECK_F("1e3", 1000);
    CHECK_F("1.5e-2", 0.015);
    CHECK_F("2.5e2", 250);
    CHECK_F("1.5+.5", 2);

    /* --- ^ power (calc uses ^ for power, NOT xor; right-associative, tight) --- */
    CHECK_F("2^10", 1024);
    CHECK_F("2^8", 256);
    CHECK_F("2^0", 1);
    CHECK_F("2^3^2", 512);            /* right-associative: 2^(3^2)=2^9 */
    CHECK_F("3*2^3", 24);             /* ^ binds tighter than * : 3*(2^3) */
    CHECK_F("2^-1", 0.5);             /* float: x^(negative) is the reciprocal now */
    CHECK_F("9^0.5", 3);              /* fractional exponent -> sqrt */

    /* --- functions (dmath) --- */
    CHECK_F("sqrt(2)", 1.4142135623730951);
    CHECK_F("sqrt(16)", 4);
    CHECK_F("sin(0)", 0);
    CHECK_F("cos(0)", 1);
    CHECK_F("tan(0)", 0);
    CHECK_F("sin(pi/2)", 1);
    CHECK_F("cos(pi)", -1);
    CHECK_F("asin(1)", 1.5707963267948966);   /* pi/2 */
    CHECK_F("acos(1)", 0);
    CHECK_F("atan(1)", 0.7853981633974483);    /* pi/4 */
    CHECK_F("exp(0)", 1);
    CHECK_F("exp(1)", 2.718281828459045);
    CHECK_F("ln(e)", 1);                        /* natural log */
    CHECK_F("log(1000)", 3);                    /* log is base-10 */
    CHECK_F("log(100)", 2);
    CHECK_F("abs(-3)", 3);
    CHECK_F("abs(3)", 3);
    CHECK_F("floor(2.7)", 2);
    CHECK_F("floor(-2.1)", -3);
    CHECK_F("ceil(2.1)", 3);
    CHECK_F("round(2.5)", 3);
    CHECK_F("round(2.4)", 2);
    CHECK_F("sqrt(2)^2", 2);                    /* round-trip */
    CHECK_F("ln(exp(3))", 3);

    /* --- constants --- */
    CHECK_F("pi", 3.14159265358979);
    CHECK_F("e", 2.71828182845905);
    CHECK_F("2*pi", 6.28318530717958);

    /* --- hex literals + bitwise (& | << >> ~) : integer ops, result as double --- */
    CHECK_F("0xff", 255);
    CHECK_F("0x10+1", 17);
    CHECK_F("0xFF", 255);             /* uppercase hex digits */
    CHECK_F("5&3", 1);
    CHECK_F("~0", -1);
    CHECK_F("~5", -6);
    CHECK_F("1<<4", 16);
    CHECK_F("256>>2", 64);
    CHECK_F("0xff & 0x0f", 15);
    CHECK_F("12 | 3", 15);
    CHECK_F("(1|2)&6", 2);            /* parens override : (3)&6 = 2 */
    CHECK_F("1+2<<3", 24);            /* + before << : (1+2)<<3 = 24 */
    CHECK_F("1<<2|1", 5);             /* << before | : (1<<2)|1 = 5 */
    CHECK_F("7.9 & 3", 3);            /* bitwise truncates operands toward zero: (7)&3 = 3 */

    /* --- undefined math yields Inf/NaN with NO error (float, not a trap) --- */
    CHECK_INF("5/0", +1);            /* +Infinity */
    CHECK_INF("-5/0", -1);           /* -Infinity */
    CHECK_NAN("0/0");                /* NaN */
    CHECK_NAN("sqrt(-1)");           /* NaN */
    CHECK_NAN("ln(-1)");             /* NaN */
    CHECK_NAN("5%0");                /* fmod by zero -> NaN */
    CHECK_NAN("asin(2)");            /* out of domain -> NaN */
    CHECK_OK("5/0");                 /* ...and these are NOT syntax errors */
    CHECK_OK("0/0");
    CHECK_OK("sqrt(-1)");

    /* --- error cases: bad token, unmatched paren, missing arg, trailing junk --- */
    CHECK_ERR("");                  /* empty -> no number */
    CHECK_ERR("2+");                /* dangling operator */
    CHECK_ERR("(2+3");              /* unmatched ( */
    CHECK_ERR("2)");                /* trailing ) */
    CHECK_ERR("2 3");               /* trailing junk after a complete expr */
    CHECK_ERR("@");                 /* not a token */
    CHECK_ERR("0x");                /* 0x with no hex digits */
    CHECK_ERR("sqrt");              /* function name with no ( arg ) */
    CHECK_ERR("sqrt 4");            /* function without parens */
    CHECK_ERR("sqrt(4");            /* unmatched function paren */
    CHECK_ERR("bogus(3)");          /* unknown function */
    CHECK_ERR("xyzzy");             /* unknown identifier */
    CHECK_OK("0");                  /* a bare zero is fine */
    CHECK_OK("(0)");
    CHECK_OK("sqrt(4)");
    CHECK_OK(".5");
    CHECK_OK("pi");

    if (fails) { printf("calc: %d check(s) FAILED\n", fails); return 1; }

    /* --- fuzz: random short expressions must never crash (ASan), trap (UBSan,
     * modulo -fwrapv's defined signed wrap), or hang. calc_eval consumes a
     * bounded string and always terminates, so a violation here is a real bug.
     * Alphabet includes letters + '.' so function/constant identifiers and
     * decimal literals are exercised too. --- */
    const char *cs = "0123456789abcdefxXpinrsqolge.+-*/%()^&|<>~ ";
    unsigned cn = (unsigned)strlen(cs);
    unsigned seed = 0xC0FFEEu;
    for (int it = 0; it < 400000; it++) {
        char buf[33];
        seed = seed * 1103515245u + 12345u; int n = (seed >> 24) % 32;
        for (int i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; buf[i] = cs[(seed >> 12) % cn]; }
        buf[n] = 0;
        int e; volatile double r = calc_eval(buf, &e); (void)r; (void)e;
    }

    printf("calc: all evaluator checks passed + 400k fuzz iterations clean\n");
    return 0;
}
