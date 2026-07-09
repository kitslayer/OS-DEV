/*
 * plot_test.c — host unit tests for the graphing calculator's expression
 * evaluator (user/ploteval.h). Pure (no syscalls), so — exactly like tests/calc
 * and tests/sheet — we build it for the host under ASan/UBSan and check the
 * recursive-descent evaluator (arithmetic, precedence, the variable x, the math
 * functions, and error handling). Exit 0 = all pass.
 */
#include <stdio.h>
#include "ploteval.h"       /* -Iuser on the compile line; pulls in dmath.h too */

static int fails, checks;

static int approx(double a, double b) {
    double d = a - b; if (d < 0) d = -d;
    double m = b < 0 ? -b : b;
    return d <= 1e-9 * (1.0 + m);
}
#define CK(expr, xval, want) do { checks++; int e; double g = plot_eval(expr, xval, &e); \
    if (e) { printf("  FAIL %s @x=%g: unexpected error\n", expr, (double)(xval)); fails++; } \
    else if (!approx(g, want)) { printf("  FAIL %s @x=%g = %.10g, want %.10g\n", expr, (double)(xval), g, (double)(want)); fails++; } } while (0)
#define CKERR(expr) do { checks++; int e; plot_eval(expr, 0, &e); \
    if (!e) { printf("  FAIL %s: should have errored\n", expr); fails++; } } while (0)

int main(void) {
    printf("graphing-calculator evaluator tests\n");

    /* the variable x */
    CK("x", 3, 3);  CK("x", -4, -4);  CK("x*x", 3, 9);  CK("2*x+1", 5, 11);
    CK("x^2", 4, 16);  CK("-x", 5, -5);  CK("(x+1)*(x-1)", 3, 8);
    CK("1/x", 4, 0.25);  CK("x^2+2*x+1", 3, 16);

    /* arithmetic / precedence / associativity (constant in x) */
    CK("1+2*3", 0, 7);  CK("(1+2)*3", 0, 9);  CK("2^3^2", 0, 512);  CK("10-4-3", 0, 3);
    CK("7/2", 0, 3.5);  CK("10%3", 0, 1);  CK("2*-3", 0, -6);

    /* functions (values matched to calc's proven-accurate dmath assertions) */
    CK("sin(x)", 0, 0);  CK("cos(x)", 0, 1);  CK("tan(x)", 0, 0);
    CK("sin(x)", 3.14159265358979 / 2, 1);  CK("cos(x)", 3.14159265358979, -1);
    CK("atan(x)", 1, 0.7853981633974483);
    CK("exp(x)", 0, 1);  CK("ln(x)", 1, 0);  CK("exp(ln(x))", 5, 5);
    CK("log(x)", 1000, 3);  CK("log2(x)", 8, 3);  CK("sqrt(x)", 16, 4);
    CK("abs(x)", -7, 7);  CK("floor(x)", 3.9, 3);  CK("ceil(x)", 3.1, 4);
    CK("round(x)", 2.5, 3);  CK("sign(x)", -2, -1);  CK("sign(x)", 0, 0);

    /* constants */
    CK("pi", 0, 3.14159265358979);  CK("2*pi", 0, 6.28318530717958);  CK("e", 0, 2.71828182845905);

    /* a realistic plotted formula sampled at a few points */
    CK("5*sin(x)", 0, 0);  CK("5*sin(x)", 3.14159265358979 / 2, 5);

    /* errors: incomplete, unmatched, unknown name, trailing junk */
    CKERR("x+");  CKERR("(x");  CKERR("x)");  CKERR("sin");  CKERR("foo(x)");  CKERR("x x");  CKERR("");

    if (!fails) printf("PASS: %d checks, plot evaluator correct\n", checks);
    else printf("FAIL: %d/%d checks failed\n", fails, checks);
    return fails ? 1 : 0;
}
