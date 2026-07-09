/*
 * sheet_test.c — host unit tests for the spreadsheet's formula engine.
 *
 * user/sheeteval.h is pure (no syscalls), so — exactly like tests/calc drives
 * user/calceval.h — we build it for the host under ASan/UBSan and check the cell
 * model, the recursive-descent evaluator (arithmetic, precedence, cell refs,
 * ranges, functions), the recalc pass (forward refs, circular-ref detection,
 * error propagation) and the number formatter. Exit 0 = all pass.
 */
#include <stdio.h>
#include <string.h>
#include "sheeteval.h"      /* -Iuser on the compile line; pulls in dmath.h too */

static int failures;
static int checks;

static void clear_all(void) { memset(cells, 0, sizeof cells); }

static int approx(double a, double b) {
    double d = a - b; if (d < 0) d = -d;
    double m = b < 0 ? -b : b;
    return d <= 1e-9 * (1.0 + m);
}

#define FAILN(...) do { failures++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } while (0)

/* check cell (r,c) holds numeric value `want` with no error */
static void chk_num(int r, int c, double want, const char *label) {
    checks++;
    cell_t *cell = CELL(r, c);
    if (cell->err) { FAILN("%s: %c%d unexpected error %d", label, 'A' + c, r + 1, cell->err); return; }
    if (!cell->is_num) { FAILN("%s: %c%d is not numeric", label, 'A' + c, r + 1); return; }
    if (!approx(cell->val, want)) FAILN("%s: %c%d = %.10g, want %.10g", label, 'A' + c, r + 1, cell->val, want);
}
/* check the given formula (evaluated in a fresh sheet at A1) yields `want` */
static void chk_expr(const char *formula, double want) {
    clear_all(); set_raw(0, 0, formula); recompute();
    chk_num(0, 0, want, formula);
}
static void chk_err(const char *formula, int wanterr) {
    clear_all(); set_raw(0, 0, formula); recompute();
    checks++;
    cell_t *cell = CELL(0, 0);
    if (cell->err != wanterr) FAILN("%s: err=%d, want %d (val=%.10g num=%d)", formula, cell->err, wanterr, cell->val, cell->is_num);
}
static void chk_fmt(double v, int w, const char *want) {
    checks++;
    const char *got = fmt_value(v, w);
    if (strcmp(got, want) != 0) FAILN("fmt_value(%.10g,%d) = \"%s\", want \"%s\"", v, w, got, want);
}

int main(void) {
    printf("spreadsheet formula-engine tests\n");

    /* --- literals & arithmetic --------------------------------------------*/
    chk_expr("=42", 42);
    chk_expr("=3.14", 3.14);
    chk_expr("=.5", 0.5);
    chk_expr("=1e3", 1000);
    chk_expr("=1.5e-2", 0.015);
    chk_expr("=1+2*3", 7);            /* precedence */
    chk_expr("=(1+2)*3", 9);
    chk_expr("=10-4-3", 3);           /* left-assoc */
    chk_expr("=2^10", 1024);
    chk_expr("=2^3^2", 512);          /* right-assoc: 2^(3^2)=2^9 */
    chk_expr("=-2^2", 4);             /* unary binds tighter than ^ (Excel-style) */
    chk_expr("=7/2", 3.5);
    chk_expr("=10%3", 1);
    chk_expr("=-(3+4)", -7);
    chk_expr("=2*-3", -6);            /* unary after operator */
    chk_expr("=+5", 5);

    /* --- constants & functions --------------------------------------------*/
    chk_expr("=SQRT(16)", 4);
    chk_expr("=ABS(-5)", 5);
    chk_expr("=INT(3.9)", 3);
    chk_expr("=FLOOR(3.9)", 3);
    chk_expr("=CEIL(3.1)", 4);
    chk_expr("=CEILING(3.1)", 4);
    chk_expr("=ROUND(3.14159, 2)", 3.14);
    chk_expr("=ROUND(2.5)", 3);
    chk_expr("=MOD(10, 3)", 1);
    chk_expr("=POW(2, 10)", 1024);
    chk_expr("=POWER(3, 3)", 27);
    chk_expr("=PRODUCT(2, 3, 4)", 24);
    chk_expr("=SUM(1, 2, 3, 4)", 10);
    chk_expr("=MAX(3, 9, 2)", 9);
    chk_expr("=MIN(3, 9, 2)", 2);
    chk_expr("=AVERAGE(2, 4, 6)", 4);
    chk_expr("=AVG(10, 20)", 15);
    chk_expr("=COUNT(1, 2, 3)", 3);
    chk_expr("=sum(1,2)+sqrt(4)", 5);  /* case-insensitive names */

    /* --- cell references ---------------------------------------------------*/
    clear_all();
    set_raw(0, 0, "10"); set_raw(0, 1, "=A1*2"); recompute();
    chk_num(0, 1, 20, "B1=A1*2");

    clear_all();                        /* forward reference (A1 depends on A2, evaluated first) */
    set_raw(0, 0, "=A2+1"); set_raw(1, 0, "5"); recompute();
    chk_num(0, 0, 6, "A1=A2+1 forward");

    clear_all();                        /* dependency chain */
    set_raw(0, 0, "5"); set_raw(1, 0, "=A1+1"); set_raw(2, 0, "=A2+1"); set_raw(3, 0, "=A2*10");
    recompute();
    chk_num(2, 0, 7, "A3 chain");
    chk_num(3, 0, 60, "A4=A2*10");     /* A2=6, so 6*10=60 */

    clear_all();                        /* empty/text refs read as 0 in arithmetic */
    set_raw(0, 1, "=A1+5"); recompute();
    chk_num(0, 1, 5, "empty ref = 0");
    clear_all();
    set_raw(0, 0, "hello"); set_raw(0, 1, "=A1+5"); recompute();
    chk_num(0, 1, 5, "text ref = 0");

    /* --- ranges ------------------------------------------------------------*/
    clear_all();
    set_raw(1, 1, "120"); set_raw(2, 1, "90"); set_raw(3, 1, "60"); set_raw(4, 1, "200");  /* B2:B5 */
    set_raw(0, 0, "=SUM(B2:B5)");   recompute(); chk_num(0, 0, 470, "SUM range");
    set_raw(0, 0, "=AVERAGE(B2:B5)"); recompute(); chk_num(0, 0, 117.5, "AVG range");
    set_raw(0, 0, "=MIN(B2:B5)"); recompute(); chk_num(0, 0, 60, "MIN range");
    set_raw(0, 0, "=MAX(B2:B5)"); recompute(); chk_num(0, 0, 200, "MAX range");
    set_raw(0, 0, "=COUNT(B2:B5)"); recompute(); chk_num(0, 0, 4, "COUNT range");
    set_raw(0, 0, "=SUM(B2:B5, 30)"); recompute(); chk_num(0, 0, 500, "SUM range + scalar");
    set_raw(0, 0, "=SUM(B2:B5)/COUNT(B2:B5)"); recompute(); chk_num(0, 0, 117.5, "nested fns");

    clear_all();                        /* range with a text cell in it: COUNT vs COUNTA */
    set_raw(0, 0, "1"); set_raw(1, 0, "2"); set_raw(2, 0, "hi"); set_raw(3, 0, "4");   /* A1:A4 */
    set_raw(0, 2, "=COUNT(A1:A4)");  recompute(); chk_num(0, 2, 3, "COUNT skips text");
    set_raw(0, 2, "=COUNTA(A1:A4)"); recompute(); chk_num(0, 2, 4, "COUNTA counts text");
    set_raw(0, 2, "=SUM(A1:A4)");    recompute(); chk_num(0, 2, 7, "SUM skips text");

    clear_all();                        /* 2-D range */
    set_raw(0, 0, "1"); set_raw(0, 1, "2"); set_raw(1, 0, "3"); set_raw(1, 1, "4");
    set_raw(3, 3, "=SUM(A1:B2)"); recompute(); chk_num(3, 3, 10, "2-D range sum");

    /* --- circular references ----------------------------------------------*/
    clear_all(); set_raw(0, 0, "=A1"); recompute();
    checks++; if (CELL(0, 0)->err != ERR_CIRC) FAILN("self-ref A1=A1 should be #CIRC (got %d)", CELL(0, 0)->err);

    clear_all();
    set_raw(0, 0, "=B1"); set_raw(0, 1, "=A1"); recompute();
    checks++; if (CELL(0, 0)->err != ERR_CIRC || CELL(0, 1)->err != ERR_CIRC) FAILN("A1<->B1 cycle should both be #CIRC (%d,%d)", CELL(0, 0)->err, CELL(0, 1)->err);

    clear_all();                        /* error propagates to a cell that references the cycle */
    set_raw(0, 0, "=B1"); set_raw(0, 1, "=A1"); set_raw(0, 2, "=A1+1"); recompute();
    checks++; if (CELL(0, 2)->err == 0) FAILN("C1 referencing a cyclic cell should be an error");

    /* --- syntax / value errors --------------------------------------------*/
    chk_err("=1+", ERR_SYNTAX);
    chk_err("=(1+2", ERR_SYNTAX);       /* unmatched paren */
    chk_err("=1+2)", ERR_SYNTAX);       /* trailing junk */
    chk_err("=SUM(", ERR_SYNTAX);
    chk_err("=foobar", ERR_SYNTAX);     /* unknown name (no paren, not a ref/const) */
    chk_err("=A1 A2", ERR_SYNTAX);      /* garbage after a ref */
    chk_err("=3.14", 0);                /* well-formed: no error */

    /* --- number formatting -------------------------------------------------*/
    chk_fmt(117.5, 8, "117.5");
    chk_fmt(470, 8, "470");
    chk_fmt(-42, 8, "-42");
    chk_fmt(0, 8, "0");
    chk_fmt(3.14159265, 6, "3.1416");   /* trimmed to fit width 6 */
    chk_fmt(1000000, 8, "1000000");
    chk_fmt(123456789, 8, "########");  /* too wide even as an integer */

    /* --- non-formula cell kinds -------------------------------------------*/
    clear_all();
    set_raw(0, 0, "42"); set_raw(0, 1, "hello"); set_raw(0, 2, "-3.5"); set_raw(0, 3, "3 apples");
    recompute();
    checks++; if (CELL(0, 0)->kind != K_NUM)  FAILN("\"42\" should be K_NUM");
    checks++; if (CELL(0, 1)->kind != K_TEXT) FAILN("\"hello\" should be K_TEXT");
    checks++; if (CELL(0, 2)->kind != K_NUM || !approx(CELL(0, 2)->val, -3.5)) FAILN("\"-3.5\" should be K_NUM -3.5");
    checks++; if (CELL(0, 3)->kind != K_TEXT) FAILN("\"3 apples\" should be K_TEXT (trailing junk)");

    /* --- deep dependency chain exercises real recursion depth --------------
     * Reverse order (A1=A2+1, ..., A89=A90+1, A90=1) so recompute starting at
     * A1 recurses all the way down before anything is memoised — a forward
     * chain would resolve in order at depth 1 and prove nothing. Length 90 sits
     * under EVAL_MAX_DEPTH (100), so it must compute correctly, not crash. */
    clear_all();
    set_raw(89, 0, "1");                /* A90 = 1 */
    for (int r = 0; r < 89; r++) {      /* A(r+1) = A(r+2) + 1 */
        char f[16]; snprintf(f, sizeof f, "=A%d+1", r + 2);
        set_raw(r, 0, f);
    }
    recompute();
    chk_num(0, 0, 90, "deep reverse chain A1");
    chk_num(89, 0, 1, "deep reverse chain A90 base");

    if (failures == 0) printf("PASS: %d checks, spreadsheet engine correct\n", checks);
    else printf("FAIL: %d/%d checks failed\n", failures, checks);
    return failures ? 1 : 0;
}
