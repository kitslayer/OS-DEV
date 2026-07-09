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
static int napprox(double a, double b) {            /* looser tolerance for numeric methods */
    double d = a - b; if (d < 0) d = -d;
    double m = b < 0 ? -b : b;
    return d <= 1e-4 * (1.0 + m);
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

    /* two-argument functions (M1729) */
    CK("min(x, 3)", 5, 3);   CK("min(x, 3)", 1, 1);        /* min */
    CK("max(x, 0)", -2, 0);  CK("max(x, 0)", 4, 4);        /* max / ReLU */
    CK("min(5, x*x)", 3, 5); CK("min(5, x*x)", 1, 1);      /* clamp a parabola; args are expressions */
    CK("hypot(3, 4)", 0, 5); CK("hypot(x, 4)", 3, 5);      /* hypot */
    CK("max(min(x, 3), -3)", 5, 3);                        /* nested clamp to [-3,3] */
    CK("atan2(1, 1)", 0, 0.7853981633974483);              /* atan2: pi/4 in quadrant I */
    CK("atan2(1, 0)", 0, 1.5707963267948966);              /* +pi/2 up the +y axis */
    CK("atan2(0, -1)", 0, 3.141592653589793);              /* +pi along the -x axis (y>=0) */
    CK("atan2(-1, 0)", 0, -1.5707963267948966);            /* -pi/2 down the -y axis */
    CK("atan2(-1, -1)", 0, -2.356194490192345);            /* -3pi/4 in quadrant III */
    CK("atan2(x, 1)", 1, 0.7853981633974483);              /* the y-arg can be the variable */
    CKERR("atan2(1)");                                     /* atan2 needs two args */
    CKERR("min(x)");   CKERR("sin(x, 2)");                 /* wrong arg count is an error */

    /* a realistic plotted formula sampled at a few points */
    CK("5*sin(x)", 0, 0);  CK("5*sin(x)", 3.14159265358979 / 2, 5);

    /* errors: incomplete, unmatched, unknown name, trailing junk */
    CKERR("x+");  CKERR("(x");  CKERR("x)");  CKERR("sin");  CKERR("foo(x)");  CKERR("x x");  CKERR("");

    /* --- definite integral (Simpson) + numeric derivative (M1714) ----------*/
    {
        int e; double g;
        g = plot_integral("x*x", 0, 3, 2000, &e);       checks++; if (e || !napprox(g, 9.0))        { printf("  FAIL integral x*x [0,3] = %.10g, want 9\n", g); fails++; }
        g = plot_integral("x*x", -10, 10, 2000, &e);    checks++; if (e || !napprox(g, 2000.0/3.0))  { printf("  FAIL integral x*x [-10,10] = %.10g, want 666.67\n", g); fails++; }
        g = plot_integral("2*x", 0, 5, 2000, &e);       checks++; if (e || !napprox(g, 25.0))       { printf("  FAIL integral 2*x [0,5] = %.10g, want 25\n", g); fails++; }
        g = plot_integral("x", -7, 7, 2000, &e);        checks++; if (e || !napprox(g, 0.0))        { printf("  FAIL integral x [-7,7] = %.10g, want 0\n", g); fails++; }
        g = plot_integral("sin(x)", 0, 3.14159265358979, 2000, &e); checks++; if (e || !napprox(g, 2.0)) { printf("  FAIL integral sin [0,pi] = %.10g, want 2\n", g); fails++; }
        g = plot_integral("5", 0, 4, 2000, &e);         checks++; if (e || !napprox(g, 20.0))       { printf("  FAIL integral 5 [0,4] = %.10g, want 20\n", g); fails++; }
        g = plot_integral("exp(x)", 0, 1, 2000, &e);    checks++; if (e || !napprox(g, 2.71828182845905 - 1.0)) { printf("  FAIL integral exp [0,1] = %.10g, want e-1\n", g); fails++; }
        plot_integral("1/x", -1, 1, 2000, &e);          checks++; if (!e) { printf("  FAIL integral 1/x [-1,1] should error (singularity at 0)\n"); fails++; }

        g = plot_derivative("x*x", 3, 1e-5, &e);        checks++; if (e || !napprox(g, 6.0))  { printf("  FAIL d/dx x*x @3 = %.10g, want 6\n", g); fails++; }
        g = plot_derivative("sin(x)", 0, 1e-5, &e);     checks++; if (e || !napprox(g, 1.0))  { printf("  FAIL d/dx sin @0 = %.10g, want 1\n", g); fails++; }
        g = plot_derivative("5*x", 2, 1e-5, &e);        checks++; if (e || !napprox(g, 5.0))  { printf("  FAIL d/dx 5*x @2 = %.10g, want 5\n", g); fails++; }
        g = plot_derivative("x^3", 2, 1e-4, &e);        checks++; if (e || !napprox(g, 12.0)) { printf("  FAIL d/dx x^3 @2 = %.10g, want 12\n", g); fails++; }
        g = plot_derivative("exp(x)", 0, 1e-5, &e);     checks++; if (e || !napprox(g, 1.0))  { printf("  FAIL d/dx exp @0 = %.10g, want 1\n", g); fails++; }
        g = plot_derivative("cos(x)", 3.14159265358979/2.0, 1e-5, &e); checks++; if (e || !napprox(g, -1.0)) { printf("  FAIL d/dx cos @pi/2 = %.10g, want -1\n", g); fails++; }
    }

    /* --- root finding (M1716): sign-change scan + bisection ----------------*/
    {
        double r[16]; int nr;
        nr = plot_find_roots("x*x-4", -10, 10, 1000, r, 16);
        checks++; if (nr != 2 || !napprox(r[0], -2) || !napprox(r[1], 2)) { printf("  FAIL roots x*x-4: n=%d\n", nr); fails++; }
        nr = plot_find_roots("x-3", -10, 10, 1000, r, 16);
        checks++; if (nr != 1 || !napprox(r[0], 3)) { printf("  FAIL roots x-3: n=%d\n", nr); fails++; }
        nr = plot_find_roots("x", -5, 5, 1000, r, 16);
        checks++; if (nr != 1 || !napprox(r[0], 0)) { printf("  FAIL roots x: n=%d\n", nr); fails++; }
        nr = plot_find_roots("x*x+1", -10, 10, 1000, r, 16);
        checks++; if (nr != 0) { printf("  FAIL roots x*x+1 (none expected): n=%d\n", nr); fails++; }
        nr = plot_find_roots("x*x-2", -10, 10, 1000, r, 16);
        checks++; if (nr != 2 || !napprox(r[0], -1.4142135623730951) || !napprox(r[1], 1.4142135623730951)) { printf("  FAIL roots x*x-2: n=%d\n", nr); fails++; }
        nr = plot_find_roots("(x-1)*(x-2)*(x-3)", 0, 4, 1000, r, 16);
        checks++; if (nr != 3 || !napprox(r[0], 1) || !napprox(r[1], 2) || !napprox(r[2], 3)) { printf("  FAIL roots cubic: n=%d\n", nr); fails++; }
        nr = plot_find_roots("sin(x)", -3.5, 3.5, 1000, r, 16);
        checks++; if (nr != 3 || !napprox(r[0], -3.14159265358979) || !napprox(r[1], 0) || !napprox(r[2], 3.14159265358979)) { printf("  FAIL roots sin: n=%d\n", nr); fails++; }
    }

    if (!fails) printf("PASS: %d checks, plot evaluator correct\n", checks);
    else printf("FAIL: %d/%d checks failed\n", fails, checks);
    return fails ? 1 : 0;
}
