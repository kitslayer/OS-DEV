/*
 * calc.c — a third userspace program: an interactive scientific calculator.
 *
 * A floating-point recursive-descent expression evaluator (the same shape a
 * real compiler's parser has): expr -> term (+|-) term, term -> power (*|/|%)
 * power, ... factor -> number | const | fn( expr ) | ( expr ) | -factor. It
 * reads a line, parses + evaluates it as IEEE-754 doubles, and prints the
 * result — a genuinely distinct interactive program from the shell and the
 * clock, all running as isolated windowed processes. (Built with SSE so it can
 * use float; the math lives in dmath.h, copied from the JS engine.)
 */
#include "ulib.h"
#include "calceval.h"   /* the recursive-descent evaluator (calc_eval), host-tested by tests/calc */

/* Format a non-negative integer as "0x" + uppercase hex (for the dev-handy hex
 * echo of an integer result — masks/addresses/flags). buf needs >= 19 bytes. */
static void to_hex(unsigned long long v, char *buf) {
    char t[16]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { int d = (int)(v & 0xF); t[i++] = (char)(d < 10 ? '0' + d : 'A' + (d - 10)); v >>= 4; }
    int p = 0; buf[p++] = '0'; buf[p++] = 'x';
    while (i) buf[p++] = t[--i];
    buf[p] = 0;
}

int main(void) {
    sys_setcolor(4); print("\n  OS-DEV calc: + - * / % ^(pow) & | << >> ~ ( ) 0x\n");   /* title: cyan */
    sys_setcolor(8); print("  fns: sqrt sin cos tan asin acos atan ln log exp abs floor ceil round\n");
    print("  consts: pi e ; e.g. sqrt(2)  sin(pi/2)  2^10 ; 'q' to quit\n\n"); sys_setcolor(0);
    char line[128];
    for (;;) {
        print("calc> ");
        readline(line, sizeof(line));
        if (line[0] == '\0') continue;
        if (streq(line, "q") || streq(line, "quit") || streq(line, "exit")) break;
        int e; double r = calc_eval(line, &e);
        if (e) { sys_setcolor(2); print("  ? syntax error\n"); sys_setcolor(0); }   /* error: red */
        else {                                          /* result: yellow, with a hex echo for integers */
            print("  = "); sys_setcolor(3); print(dnum_to_str(r)); sys_setcolor(0);
            if (r >= 0 && r < 9007199254740992.0 && r == (double)(unsigned long long)r) {   /* exact non-negative integer (< 2^53): also show hex */
                char hx[20]; to_hex((unsigned long long)r, hx);
                sys_setcolor(8); print("  "); print(hx); sys_setcolor(0);
            }
            print("\n");
        }
    }
    print("bye!\n");
    return 0;
}
