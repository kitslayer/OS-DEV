/*
 * calc.c — a third userspace program: an interactive calculator.
 *
 * A small recursive-descent expression evaluator (the same shape a real
 * compiler's parser has): expr -> term (+|-) term, term -> factor (*|/) factor,
 * factor -> number | ( expr ) | -factor. It reads a line, parses + evaluates
 * it, and prints the result — a genuinely distinct interactive program from the
 * shell and the clock, all running as isolated windowed processes.
 */
#include "ulib.h"
#include "calceval.h"   /* the recursive-descent evaluator (calc_eval), host-tested by tests/calc */

static void itoa_l(long v, char *out) {
    char t[24]; int i = 0, neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (u == 0) t[i++] = '0';
    while (u) { t[i++] = (char)('0' + u % 10); u /= 10; }
    int j = 0; if (neg) out[j++] = '-';
    while (i) out[j++] = t[--i];
    out[j] = 0;
}
static void itoa_hex(long v, char *out) {        /* unsigned 64-bit hex (so negatives show two's-complement) */
    unsigned long u = (unsigned long)v;
    char t[20]; int i = 0;
    if (u == 0) t[i++] = '0';
    while (u) { int d = (int)(u & 15); t[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u >>= 4; }
    int j = 0; out[j++] = '0'; out[j++] = 'x';
    while (i) out[j++] = t[--i];
    out[j] = 0;
}

int main(void) {
    sys_setcolor(4); print("\n  OS-DEV calc: + - * / % ^ & | << >> ~ ( ) 0x\n");   /* title: cyan */
    sys_setcolor(8); print("  e.g. (2+3)*4 ; 'q' to quit\n\n"); sys_setcolor(0);
    char line[128];
    for (;;) {
        print("calc> ");
        readline(line, sizeof(line));
        if (line[0] == '\0') continue;
        if (streq(line, "q") || streq(line, "quit") || streq(line, "exit")) break;
        int e; long r = calc_eval(line, &e);
        if (e) { sys_setcolor(2); print("  ? syntax error\n"); sys_setcolor(0); }   /* error: red */
        else { char b[24]; itoa_l(r, b); print("  = "); sys_setcolor(3); print(b); sys_setcolor(0);  /* decimal: yellow */
               char h[24]; itoa_hex(r, h); sys_setcolor(8); print("  "); print(h); sys_setcolor(0); print("\n"); }  /* hex: grey */
    }
    print("bye!\n");
    return 0;
}
