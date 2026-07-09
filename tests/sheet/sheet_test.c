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
/* check fmt_value_col(v, w, fmt) renders exactly `want` */
static void chk_fmtc(double v, int w, char fmt, const char *want) {
    checks++;
    const char *got = fmt_value_col(v, w, fmt);
    if (strcmp(got, want) != 0) FAILN("fmt_value_col(%.10g,%d,'%c') = \"%s\", want \"%s\"", v, w, fmt ? fmt : ' ', got, want);
}
/* check adjust_refs shifts a formula's relative refs by (dr,dc) into `want` */
static void chk_adj(const char *src, int dr, int dc, const char *want) {
    checks++;
    char out[RAWMAX];
    adjust_refs(src, dr, dc, out, RAWMAX);
    if (strcmp(out, want) != 0) FAILN("adjust_refs(\"%s\", %d, %d) = \"%s\", want \"%s\"", src, dr, dc, out, want);
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

    /* --- comparison operators (M1697): lowest precedence, yield 1.0/0.0 ----*/
    chk_expr("=1<2", 1);
    chk_expr("=2<1", 0);
    chk_expr("=3=3", 1);
    chk_expr("=3<>3", 0);
    chk_expr("=3<>4", 1);
    chk_expr("=5>=5", 1);
    chk_expr("=5>=6", 0);
    chk_expr("=4<=3", 0);
    chk_expr("=4<=4", 1);
    chk_expr("=7>2", 1);
    chk_expr("=2>7", 0);
    chk_expr("=2+3=5", 1);             /* arithmetic binds tighter than comparison */
    chk_expr("=2+3<10", 1);            /* 5 < 10 */
    chk_expr("=1<2=1", 1);             /* left-assoc: (1<2)=1 -> 1=1 -> 1 */
    chk_expr("=(2>1)*10", 10);         /* boolean usable as a number */

    /* --- IF / AND / OR / NOT (M1697) --------------------------------------*/
    chk_expr("=IF(1,10,20)", 10);
    chk_expr("=IF(0,10,20)", 20);
    chk_expr("=IF(2>1,100,200)", 100);
    chk_expr("=IF(2<1,100,200)", 200);
    chk_expr("=IF(1,7)", 7);           /* omitted else, condition true -> then */
    chk_expr("=IF(0,7)", 0);           /* omitted else, condition false -> 0 */
    chk_expr("=IF(1>0,IF(2>1,5,6),9)", 5);   /* nested IF */
    chk_expr("=AND(1,1,1)", 1);
    chk_expr("=AND(1,0,1)", 0);
    chk_expr("=OR(0,0,1)", 1);
    chk_expr("=OR(0,0,0)", 0);
    chk_expr("=NOT(0)", 1);
    chk_expr("=NOT(5)", 0);
    chk_expr("=AND(2>1, 3>2)", 1);
    chk_expr("=OR(1>2, 2>3)", 0);
    chk_expr("=IF(AND(1,1), 42, 0)", 42);

    clear_all();                        /* IF's untaken branch must not fail the formula */
    set_raw(0, 0, "=SUM(");             /* A1 is a syntax error */
    set_raw(0, 1, "=IF(1>0, 5, A1)");   /* takes the then-branch; A1's error must not leak */
    recompute();
    chk_num(0, 1, 5, "IF untaken-branch error rollback");
    clear_all();
    set_raw(0, 0, "10"); set_raw(0, 1, "5");        /* A1=10 B1=5 */
    set_raw(0, 2, "=IF(A1>B1, A1-B1, B1-A1)");      /* refs in condition + branches */
    recompute();
    chk_num(0, 2, 5, "IF over cell refs");

    /* --- math functions (M1697); values matched to calc's proven-accurate set */
    chk_expr("=LN(1)", 0);
    chk_expr("=EXP(0)", 1);
    chk_expr("=EXP(LN(5))", 5);        /* inverse pair */
    chk_expr("=LOG(100)", 2);          /* default base 10 (like calc) */
    chk_expr("=LOG(1000)", 3);
    chk_expr("=LOG(8,2)", 3);          /* explicit base */
    chk_expr("=LOG10(1000)", 3);
    chk_expr("=LOG2(8)", 3);
    chk_expr("=SIN(0)", 0);
    chk_expr("=COS(0)", 1);
    chk_expr("=TAN(0)", 0);
    chk_expr("=ATAN(1)", 0.7853981633974483);   /* pi/4 */
    chk_expr("=SIGN(-3.5)", -1);
    chk_expr("=SIGN(0)", 0);
    chk_expr("=SIGN(9)", 1);
    chk_expr("=TRUNC(3.7)", 3);
    chk_expr("=TRUNC(-3.7)", -3);      /* toward zero, unlike FLOOR */
    chk_expr("=TRUNC(3.14159, 2)", 3.14);

    /* --- statistics (M1697): classic {2,4,4,4,5,5,7,9}, mean 5, ss=32 ------*/
    chk_expr("=VARP(2,4,4,4,5,5,7,9)", 4);        /* population variance = 32/8 */
    chk_expr("=STDEVP(2,4,4,4,5,5,7,9)", 2);      /* population stdev = sqrt(4) */
    chk_expr("=VAR(2,4,4,4,5,5,7,9)", 32.0 / 7.0);          /* sample variance = 32/(n-1) */
    chk_expr("=STDEV(2,4,4,4,5,5,7,9)", js_sqrt(32.0 / 7.0));
    clear_all();                        /* same, over a range */
    set_raw(0, 0, "2"); set_raw(1, 0, "4"); set_raw(2, 0, "4"); set_raw(3, 0, "4");
    set_raw(4, 0, "5"); set_raw(5, 0, "5"); set_raw(6, 0, "7"); set_raw(7, 0, "9");
    set_raw(0, 2, "=VARP(A1:A8)");   recompute(); chk_num(0, 2, 4, "VARP range");
    set_raw(0, 2, "=STDEVP(A1:A8)"); recompute(); chk_num(0, 2, 2, "STDEVP range");
    set_raw(0, 2, "=AVERAGE(A1:A8)"); recompute(); chk_num(0, 2, 5, "mean of the sample");

    /* --- range parsing for :chart (M1699) ---------------------------------*/
    {
        int r1, c1, r2, c2;
        checks++; if (!parse_range("B2:B5", &r1, &c1, &r2, &c2) || r1 != 1 || c1 != 1 || r2 != 4 || c2 != 1) FAILN("parse_range B2:B5");
        checks++; if (!parse_range("A1:C3", &r1, &c1, &r2, &c2) || r1 != 0 || c1 != 0 || r2 != 2 || c2 != 2) FAILN("parse_range A1:C3");
        checks++; if (!parse_range("D5", &r1, &c1, &r2, &c2) || r1 != 4 || c1 != 3 || r2 != 4 || c2 != 3) FAILN("parse_range single D5");
        checks++; if (!parse_range("b5:b2", &r1, &c1, &r2, &c2) || r1 != 1 || r2 != 4) FAILN("parse_range normalises reversed ends");
        checks++; if (parse_range("B2:", &r1, &c1, &r2, &c2)) FAILN("parse_range must reject \"B2:\"");
        checks++; if (parse_range("2B", &r1, &c1, &r2, &c2)) FAILN("parse_range must reject \"2B\"");
        checks++; if (parse_range("B2 C3", &r1, &c1, &r2, &c2)) FAILN("parse_range must reject junk after ref");
        checks++; if (parse_range("A200:A201", &r1, &c1, &r2, &c2)) FAILN("parse_range must reject out-of-range row");
    }

    /* --- CSV export / import (M1698) --------------------------------------*/
    {
        char csv[512];
        clear_all();
        set_raw(0, 0, "Name");   set_raw(0, 1, "Qty"); set_raw(0, 2, "Total");
        set_raw(1, 0, "Apples"); set_raw(1, 1, "3");   set_raw(1, 2, "=B2*10");
        set_raw(2, 0, "Pears");  set_raw(2, 1, "5");   set_raw(2, 2, "=B3*10");
        recompute();
        sheet_to_csv(csv, sizeof csv);   /* formulas export as their computed VALUES */
        checks++;
        if (strcmp(csv, "Name,Qty,Total\nApples,3,30\nPears,5,50\n") != 0)
            FAILN("CSV export: got \"%s\"", csv);

        clear_all();                     /* RFC-4180 quoting of comma / quote fields */
        set_raw(0, 0, "a,b"); set_raw(0, 1, "he said \"hi\""); set_raw(0, 2, "plain");
        recompute();
        sheet_to_csv(csv, sizeof csv);
        checks++;
        if (strcmp(csv, "\"a,b\",\"he said \"\"hi\"\"\",plain\n") != 0)
            FAILN("CSV quoting: got \"%s\"", csv);

        sheet_from_csv("x,y,z\n1,2,3\n10,20,30\n");   /* import: numbers vs text */
        recompute();
        chk_num(1, 0, 1, "CSV import A2"); chk_num(1, 2, 3, "CSV import C2");
        chk_num(2, 1, 20, "CSV import B3");
        checks++; if (CELL(0, 0)->kind != K_TEXT) FAILN("CSV import: \"x\" should be text");
        checks++; if (CELL(1, 0)->kind != K_NUM)  FAILN("CSV import: \"1\" should be number");

        /* import quoted fields: embedded comma + escaped ("") quote */
        sheet_from_csv("\"a,b\",\"say \"\"hi\"\"\"\nplain,42\n");
        recompute();
        checks++; if (strcmp(CELL(0, 0)->raw, "a,b") != 0) FAILN("CSV in quoted comma: \"%s\"", CELL(0, 0)->raw);
        checks++; if (strcmp(CELL(0, 1)->raw, "say \"hi\"") != 0) FAILN("CSV in escaped quote: \"%s\"", CELL(0, 1)->raw);
        chk_num(1, 1, 42, "CSV import row2 B");

        sheet_from_csv("5,=A1*3\n");     /* an '='-prefixed field imports as a live formula */
        recompute();
        chk_num(0, 1, 15, "CSV import formula field");

        clear_all();                     /* export -> import -> export is stable */
        set_raw(0, 0, "10"); set_raw(0, 1, "=A1+5"); set_raw(1, 0, "hello");
        recompute();
        char csv1[512], csv2[512];
        sheet_to_csv(csv1, sizeof csv1);
        sheet_from_csv(csv1); recompute();
        sheet_to_csv(csv2, sizeof csv2);
        checks++; if (strcmp(csv1, csv2) != 0) FAILN("CSV round-trip unstable: \"%s\" vs \"%s\"", csv1, csv2);
    }

    /* --- conditional aggregates (M1711): A1:A6 = 1,5,3,8,5,2 --------------*/
    clear_all();
    set_raw(0, 0, "1"); set_raw(1, 0, "5"); set_raw(2, 0, "3"); set_raw(3, 0, "8"); set_raw(4, 0, "5"); set_raw(5, 0, "2");
    set_raw(0, 2, "=SUMIF(A1:A6,>3)");    recompute(); chk_num(0, 2, 18, "SUMIF >3");        /* 5+8+5 */
    set_raw(0, 2, "=SUMIF(A1:A6,5)");     recompute(); chk_num(0, 2, 10, "SUMIF =5 (bare)"); /* 5+5 */
    set_raw(0, 2, "=SUMIF(A1:A6,<=3)");   recompute(); chk_num(0, 2, 6,  "SUMIF <=3");        /* 1+3+2 */
    set_raw(0, 2, "=SUMIF(A1:A6,<>5)");   recompute(); chk_num(0, 2, 14, "SUMIF <>5");        /* 1+3+8+2 */
    set_raw(0, 2, "=SUMIF(A1:A6,>100)");  recompute(); chk_num(0, 2, 0,  "SUMIF none matches");
    set_raw(0, 2, "=COUNTIF(A1:A6,>3)");  recompute(); chk_num(0, 2, 3,  "COUNTIF >3");
    set_raw(0, 2, "=COUNTIF(A1:A6,5)");   recompute(); chk_num(0, 2, 2,  "COUNTIF =5");
    set_raw(0, 2, "=COUNTIF(A1:A6,<>5)"); recompute(); chk_num(0, 2, 4,  "COUNTIF <>5");
    set_raw(0, 2, "=AVERAGEIF(A1:A6,>3)"); recompute(); chk_num(0, 2, 6, "AVERAGEIF >3");     /* 18/3 */
    set_raw(0, 2, "=SUMIF(A1:A6,>3)+COUNTIF(A1:A6,>3)"); recompute(); chk_num(0, 2, 21, "conditional aggs compose"); /* 18+3 */

    /* --- relative-reference adjustment for copy/paste + fill (M1712) -------
     * The pure engine behind :y/:p and :fd/:fr — shift every genuine cell ref
     * by (dr,dc), clamped to the grid, while leaving function names, constants,
     * text and number literals (incl. exponents) untouched. */
    chk_adj("=A1+B1", 1, 0, "=A2+B2");            /* fill one row down */
    chk_adj("=A1+B1", 2, 0, "=A3+B3");            /* two rows down */
    chk_adj("=A1+B1", 0, 1, "=B1+C1");            /* one column right */
    chk_adj("=A1", 2, 3, "=D3");                  /* both axes: A1 -> D3 */
    chk_adj("=A1*2", 1, 0, "=A2*2");              /* a literal 2 is not a ref */
    chk_adj("=SUM(A1:A5)", 1, 0, "=SUM(A2:A6)");  /* range endpoints both shift; SUM stays */
    chk_adj("=A1+SUM(B1:B2)", 2, 0, "=A3+SUM(B3:B4)");
    chk_adj("=IF(A1>0,B1,C1)", 1, 0, "=IF(A2>0,B2,C2)");   /* IF name kept, all three refs shift */
    chk_adj("=SUMIF(A1:A6,>3)", 1, 0, "=SUMIF(A2:A7,>3)"); /* criterion's 3 is a literal, not a ref */
    chk_adj("=A1*PI", 0, 1, "=B1*PI");            /* PI is a constant, not a ref */
    chk_adj("=A1+E", 1, 0, "=A2+E");              /* bare E (constant) is left alone */
    chk_adj("=E5", 1, 0, "=E6");                  /* ...but E5 is a genuine ref (col E, row 5) */
    chk_adj("=A1*1e5", 1, 0, "=A2*1e5");          /* an exponent's e5 must NOT be read as a ref */
    chk_adj("=1.5e-2+A1", 2, 0, "=1.5e-2+A3");    /* signed exponent preserved */
    chk_adj("=a1+b1", 1, 0, "=A2+B2");            /* lowercase refs normalise to uppercase */
    chk_adj("=A1", -5, 0, "=A1");                 /* clamp at the top edge (row can't go < 1) */
    chk_adj("=A1", 0, -3, "=A1");                 /* clamp at the left edge (col can't go < A) */
    chk_adj("=Z100", 5, 5, "=Z100");              /* clamp at the bottom-right corner */
    chk_adj("hello world", 1, 1, "hello world");  /* plain text: nothing to adjust */
    chk_adj("=A10+A2", 1, 0, "=A11+A3");          /* multi-digit row shifts correctly */

    /* --- row sort (M1713): whole rows reorder by a key column, and moved
     * formulas ref-adjust so they still reference their own (new) row --------*/
    clear_all();                        /* col A label, B value, C = a formula =B{row}*2 */
    set_raw(0, 0, "d"); set_raw(0, 1, "30"); set_raw(0, 2, "=B1*2");   /* 30 -> 60 */
    set_raw(1, 0, "b"); set_raw(1, 1, "10"); set_raw(1, 2, "=B2*2");   /* 10 -> 20 */
    set_raw(2, 0, "c"); set_raw(2, 1, "20"); set_raw(2, 2, "=B3*2");   /* 20 -> 40 */
    set_raw(3, 0, "a"); set_raw(3, 1, "40"); set_raw(3, 2, "=B4*2");   /* 40 -> 80 */
    recompute();
    sort_rows(0, 3, 1, 0);              /* sort rows 0..3 by column B, ascending */
    recompute();
    chk_num(0, 1, 10, "sort asc B row0"); chk_num(1, 1, 20, "sort asc B row1");
    chk_num(2, 1, 30, "sort asc B row2"); chk_num(3, 1, 40, "sort asc B row3");
    chk_num(0, 2, 20, "sort asc C follows row0");   /* the =B*2 formula moved WITH its row */
    chk_num(1, 2, 40, "sort asc C follows row1");
    chk_num(2, 2, 60, "sort asc C follows row2");
    chk_num(3, 2, 80, "sort asc C follows row3");
    checks++; if (strcmp(CELL(0, 0)->raw, "b") != 0) FAILN("sort asc: label row0 = \"%s\", want b", CELL(0, 0)->raw);
    checks++; if (strcmp(CELL(3, 0)->raw, "a") != 0) FAILN("sort asc: label row3 = \"%s\", want a", CELL(3, 0)->raw);
    checks++; if (strcmp(CELL(0, 2)->raw, "=B1*2") != 0) FAILN("sort asc: C row0 raw = \"%s\", want =B1*2", CELL(0, 2)->raw);
    checks++; if (strcmp(CELL(3, 2)->raw, "=B4*2") != 0) FAILN("sort asc: C row3 raw = \"%s\", want =B4*2", CELL(3, 2)->raw);

    clear_all();                        /* descending */
    set_raw(0, 1, "30"); set_raw(1, 1, "10"); set_raw(2, 1, "20"); set_raw(3, 1, "40");
    recompute();
    sort_rows(0, 3, 1, 1);
    recompute();
    chk_num(0, 1, 40, "sort desc row0"); chk_num(1, 1, 30, "sort desc row1");
    chk_num(2, 1, 20, "sort desc row2"); chk_num(3, 1, 10, "sort desc row3");

    clear_all();                        /* text keys ascending; an empty key cell sorts last */
    set_raw(0, 0, "banana"); set_raw(1, 0, "apple"); set_raw(2, 0, ""); set_raw(3, 0, "cherry");
    recompute();
    sort_rows(0, 3, 0, 0);
    checks++; if (strcmp(CELL(0, 0)->raw, "apple")  != 0) FAILN("sort text row0 = \"%s\"", CELL(0, 0)->raw);
    checks++; if (strcmp(CELL(1, 0)->raw, "banana") != 0) FAILN("sort text row1 = \"%s\"", CELL(1, 0)->raw);
    checks++; if (strcmp(CELL(2, 0)->raw, "cherry") != 0) FAILN("sort text row2 = \"%s\"", CELL(2, 0)->raw);
    checks++; if (CELL(3, 0)->raw[0] != 0) FAILN("sort: empty key should sort last, got \"%s\"", CELL(3, 0)->raw);

    clear_all();                        /* a number key sorts before a text key */
    set_raw(0, 0, "xyz"); set_raw(1, 0, "5"); set_raw(2, 0, "abc"); set_raw(3, 0, "2");
    recompute();
    sort_rows(0, 3, 0, 0);
    checks++; if (strcmp(CELL(0, 0)->raw, "2")   != 0) FAILN("num<text row0 = \"%s\", want 2", CELL(0, 0)->raw);
    checks++; if (strcmp(CELL(1, 0)->raw, "5")   != 0) FAILN("num<text row1 = \"%s\", want 5", CELL(1, 0)->raw);
    checks++; if (strcmp(CELL(2, 0)->raw, "abc") != 0) FAILN("num<text row2 = \"%s\", want abc", CELL(2, 0)->raw);
    checks++; if (strcmp(CELL(3, 0)->raw, "xyz") != 0) FAILN("num<text row3 = \"%s\", want xyz", CELL(3, 0)->raw);

    clear_all();                        /* stability: equal keys keep their original order */
    set_raw(0, 0, "5"); set_raw(0, 1, "first");
    set_raw(1, 0, "5"); set_raw(1, 1, "second");
    set_raw(2, 0, "5"); set_raw(2, 1, "third");
    recompute();
    sort_rows(0, 2, 0, 0);
    checks++; if (strcmp(CELL(0, 1)->raw, "first") != 0 || strcmp(CELL(1, 1)->raw, "second") != 0 ||
                   strcmp(CELL(2, 1)->raw, "third") != 0) FAILN("sort not stable for equal keys");

    /* --- per-column number formats (M1726): fmt_value_col + fmt_fixed dec=0 round */
    chk_fmtc(117.5, 8, 'G', "117.5");           /* general = fmt_value */
    chk_fmtc(470, 8, 0, "470");                  /* 0 code = general */
    chk_fmtc(3.14159, 8, '2', "3.14");           /* fixed 2 dp */
    chk_fmtc(2, 8, '3', "2.000");
    chk_fmtc(3.7, 8, '0', "4");                  /* 0 dp ROUNDS (the fmt_fixed dec=0 fix) */
    chk_fmtc(3.4, 8, '0', "3");
    chk_fmtc(0.5, 8, '%', "50.0%");              /* percent = value*100 + 1dp + '%' */
    chk_fmtc(0.125, 8, '%', "12.5%");
    chk_fmtc(1.0, 8, '%', "100.0%");
    chk_fmtc(-0.25, 8, '%', "-25.0%");
    chk_fmtc(270, 10, '$', "$270.00");           /* currency = '$' + 2dp */
    chk_fmtc(1005, 10, '$', "$1005.00");
    chk_fmtc(0, 8, '$', "$0.00");
    chk_fmtc(-12.5, 10, '$', "-$12.50");         /* sign ahead of the '$' */
    chk_fmtc(123456, 4, '2', "####");            /* formatted value too wide -> '#' fill */
    chk_fmtc(1234, 4, '%', "####");

    /* --- cell find (M1727): case-insensitive, raw text + computed value, wraps */
    clear_all();
    set_raw(0, 0, "North"); set_raw(0, 1, "120");
    set_raw(1, 0, "South"); set_raw(1, 1, "90");
    set_raw(2, 0, "total"); set_raw(2, 1, "=B1+B2");   /* value 210 */
    recompute();
    {
        int r, c;
        checks++; if (!sheet_find("South", 0, 0, &r, &c) || r != 1 || c != 0) FAILN("find South");
        checks++; if (!sheet_find("north", 2, 1, &r, &c) || r != 0 || c != 0) FAILN("find 'north' (case-insensitive, wraps)");
        checks++; if (!sheet_find("210", 0, 0, &r, &c) || r != 2 || c != 1) FAILN("find computed value 210");
        checks++; if (!sheet_find("North", 1, 0, &r, &c) || r != 0 || c != 0) FAILN("find wraps past the end");
        checks++; if (sheet_find("zzz", 0, 0, &r, &c)) FAILN("find non-existent must fail");
        checks++; if (sheet_find("", 0, 0, &r, &c)) FAILN("find empty query must fail");
        /* 'o' is in North(0,0)/South(1,0)/total(2,0); from (1,0) the NEXT is (2,0) */
        checks++; if (!sheet_find("o", 1, 0, &r, &c) || r != 2 || c != 0) FAILN("find-next cycles matches");
    }

    if (failures == 0) printf("PASS: %d checks, spreadsheet engine correct\n", checks);
    else printf("FAIL: %d/%d checks failed\n", failures, checks);
    return failures ? 1 : 0;
}
