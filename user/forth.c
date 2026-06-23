/*
 * forth.c — a real Forth interpreter, in OS-DEV userspace (M1082).
 *
 * "The OS can program itself": a stack-based language with arithmetic, the
 * classic stack words, comparison/logic, output, colon definitions (: name … ;),
 * IF/ELSE/THEN, BEGIN/UNTIL, DO/LOOP + I, variables/constants with ! @ ?, and
 * ( … ) / \ comments. It's a self-contained ring-3 app (just readline/print +
 * the existing syscalls), so zero kernel risk — and a genuine in-guest toolchain.
 *
 * Implementation: an *immediate* interpreter. Each input line (and each colon
 * word's body, re-interpreted on call) is tokenised on whitespace into an array;
 * a program counter walks it, and the control words steer the PC (forward-scan
 * for IF/ELSE/THEN with nesting, a small loop stack for BEGIN/DO). Numbers parse
 * as signed decimal or 0x-hex. Slower than a threaded Forth, but small and clear.
 */
#include "ulib.h"

#define DS_N 256
static long ds[DS_N];
static int  dp;
static int  err;                       /* set on stack underflow / bad word */

static void push(long v) { if (dp < DS_N) ds[dp++] = v; else err = 1; }
static long pop(void)    { if (dp > 0) return ds[--dp]; err = 1; return 0; }

#define VARS_N 64
static long vars[VARS_N];
static int  nvars;

#define WORDS_N 96
struct word { char name[32]; int type; char body[200]; long val; };  /* type 0=colon 1=var 2=const */
static struct word words[WORDS_N];
static int nwords;

/* loop stack for DO/LOOP + I (shared; words balance their own do/loop) */
static long loop_idx[32], loop_lim[32];
static int  loop_sp;

static int interp(const char *src, int depth);

/* lower-case a single char (Forth is case-insensitive for our built-ins) */
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int  ieq(const char *a, const char *b) {       /* case-insensitive streq */
    while (*a && *b) { if (lc(*a) != lc(*b)) return 0; a++; b++; }
    return *a == *b;
}

/* parse `s` as a signed decimal or 0x-hex integer; 1 + *out if fully numeric */
static int parse_num(const char *s, long *out) {
    if (!*s) return 0;
    int neg = 0; const char *p = s;
    if (*p == '-') { if (!p[1]) return 0; neg = 1; p++; }
    long v = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && p[2]) {
        p += 2;
        for (; *p; p++) {
            char c = lc(*p); int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else return 0;
            v = v * 16 + d;
        }
    } else {
        for (; *p; p++) { if (*p < '0' || *p > '9') return 0; v = v * 10 + (*p - '0'); }
    }
    *out = neg ? -v : v;
    return 1;
}

/* split `src` into NUL-terminated tokens inside `buf`; returns the token count */
static int tokenize(const char *src, char *buf, int buflen, char **toks, int maxtoks) {
    int n = 0, bi = 0, i = 0;
    while (src[i] && bi < buflen - 1 && n < maxtoks) {
        while (src[i] == ' ' || src[i] == '\t') i++;
        if (!src[i]) break;
        toks[n++] = &buf[bi];
        while (src[i] && src[i] != ' ' && src[i] != '\t' && bi < buflen - 1) buf[bi++] = src[i++];
        buf[bi++] = 0;
    }
    return n;
}

static struct word *find_word(const char *name) {
    for (int i = nwords - 1; i >= 0; i--) if (ieq(words[i].name, name)) return &words[i];
    return 0;
}

/* from just-after an IF, find the matching ELSE/THEN at depth 0 (is_else set) */
static int scan_if(char **t, int n, int pc, int *is_else) {
    int depth = 0;
    for (int j = pc; j < n; j++) {
        if (ieq(t[j], "if")) depth++;
        else if (ieq(t[j], "else") && depth == 0) { *is_else = 1; return j; }
        else if (ieq(t[j], "then")) { if (depth == 0) { *is_else = 0; return j; } depth--; }
    }
    return n;
}
static int scan_then(char **t, int n, int pc) {       /* after ELSE: matching THEN */
    int depth = 0;
    for (int j = pc; j < n; j++) {
        if (ieq(t[j], "if")) depth++;
        else if (ieq(t[j], "then")) { if (depth == 0) return j; depth--; }
    }
    return n;
}

static void print_long(long v) {
    char b[24]; int n = 0, neg = v < 0;
    unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    if (!u) b[n++] = '0';
    while (u) { b[n++] = (char)('0' + u % 10); u /= 10; }
    if (neg) b[n++] = '-';
    char o[24]; int m = 0;
    while (n) o[m++] = b[--n];
    o[m] = 0; print(o);
}

/* interpret one tokenised program (a line, or a colon word's body). depth guards
 * runaway recursion through user words. */
static int interp(const char *src, int depth) {
    if (depth > 64) { print("forth: recursion too deep\n"); err = 1; return -1; }
    char buf[1024]; char *t[256];
    int n = tokenize(src, buf, sizeof buf, t, 256);

    int begin_stk[32], begin_sp = 0;         /* BEGIN addresses for BEGIN/UNTIL */
    int do_pc[32], do_sp = 0;                /* token index just after each DO */

    for (int pc = 0; pc < n && !err; pc++) {
        const char *w = t[pc];

        long num;
        if (parse_num(w, &num)) { push(num); continue; }

        /* --- comments --- */
        if (ieq(w, "(")) { while (pc < n && !ieq(t[pc], ")")) pc++; continue; }
        if (ieq(w, "\\")) break;             /* line comment: rest of the line */

        /* --- colon definition --- */
        if (ieq(w, ":")) {
            if (pc + 1 >= n) { err = 1; break; }
            struct word *nw = (nwords < WORDS_N) ? &words[nwords] : 0;
            if (!nw) { print("forth: dictionary full\n"); err = 1; break; }
            int j = 0; const char *nm = t[++pc];
            while (nm[j] && j < 31) { nw->name[j] = nm[j]; j++; } nw->name[j] = 0;
            nw->type = 0;
            int bp = 0; pc++;
            while (pc < n && !ieq(t[pc], ";")) {                 /* copy body text */
                const char *tok = t[pc];
                for (int k = 0; tok[k] && bp < 198; k++) nw->body[bp++] = tok[k];
                if (bp < 198) nw->body[bp++] = ' ';
                pc++;
            }
            nw->body[bp] = 0;
            nwords++;
            continue;
        }
        if (ieq(w, ";")) continue;           /* stray ; */

        /* --- defining words --- */
        if (ieq(w, "variable")) {
            if (pc + 1 >= n || nwords >= WORDS_N || nvars >= VARS_N) { err = 1; break; }
            struct word *nw = &words[nwords++]; int j = 0; const char *nm = t[++pc];
            while (nm[j] && j < 31) { nw->name[j] = nm[j]; j++; } nw->name[j] = 0;
            nw->type = 1; nw->val = nvars; vars[nvars++] = 0;
            continue;
        }
        if (ieq(w, "constant")) {            /* ( n -- )  constant NAME */
            if (pc + 1 >= n || nwords >= WORDS_N) { err = 1; break; }
            struct word *nw = &words[nwords++]; int j = 0; const char *nm = t[++pc];
            while (nm[j] && j < 31) { nw->name[j] = nm[j]; j++; } nw->name[j] = 0;
            nw->type = 2; nw->val = pop();
            continue;
        }

        /* --- string print:  ." text"  --- */
        if (ieq(w, ".\"")) {
            pc++;
            while (pc < n) {
                const char *tok = t[pc]; int len = 0; while (tok[len]) len++;
                if (len && tok[len - 1] == '"') { char tmp[64]; int m = 0; for (int k = 0; k < len - 1 && m < 63; k++) tmp[m++] = tok[k]; tmp[m] = 0; print(tmp); break; }
                print(tok); print(" "); pc++;
            }
            continue;
        }

        /* --- control flow --- */
        if (ieq(w, "if")) {
            long c = pop();
            if (!c) { int is_else; pc = scan_if(t, n, pc + 1, &is_else); }  /* false: jump to ELSE (pc++ enters its body) or THEN (pc++ steps past) */
            continue;                                                       /* true: fall through into the body */
        }
        if (ieq(w, "else")) { pc = scan_then(t, n, pc + 1); continue; }   /* true branch ran: skip to THEN */
        if (ieq(w, "then")) continue;
        if (ieq(w, "begin")) { if (begin_sp < 32) begin_stk[begin_sp++] = pc; continue; }
        if (ieq(w, "until")) { long c = pop(); if (!c && begin_sp > 0) pc = begin_stk[begin_sp - 1]; else if (begin_sp > 0) begin_sp--; continue; }
        if (ieq(w, "again")) { if (begin_sp > 0) pc = begin_stk[begin_sp - 1]; continue; }   /* infinite (use with a guarded exit) */
        if (ieq(w, "do")) {                  /* ( limit start -- ) */
            long start = pop(), limit = pop();
            if (loop_sp < 32) { loop_idx[loop_sp] = start; loop_lim[loop_sp] = limit; loop_sp++; }
            if (do_sp < 32) do_pc[do_sp++] = pc;       /* remember the DO token */
            continue;
        }
        if (ieq(w, "loop")) {
            if (loop_sp > 0) {
                loop_idx[loop_sp - 1]++;
                if (loop_idx[loop_sp - 1] < loop_lim[loop_sp - 1]) { pc = do_pc[do_sp - 1]; }   /* back to DO */
                else { loop_sp--; do_sp--; }
            }
            continue;
        }
        if (ieq(w, "i")) { push(loop_sp > 0 ? loop_idx[loop_sp - 1] : 0); continue; }

        /* --- a user-defined word: re-interpret its body --- */
        struct word *uw = find_word(w);
        if (uw) {
            if (uw->type == 1) push(uw->val);            /* variable -> its cell index (address) */
            else if (uw->type == 2) push(uw->val);       /* constant -> its value */
            else interp(uw->body, depth + 1);            /* colon word */
            continue;
        }

        /* --- built-in words --- */
        if (ieq(w, "+")) { long b = pop(), a = pop(); push(a + b); }
        else if (ieq(w, "-")) { long b = pop(), a = pop(); push(a - b); }
        else if (ieq(w, "*")) { long b = pop(), a = pop(); push(a * b); }
        else if (ieq(w, "/")) { long b = pop(), a = pop(); push(b ? a / b : 0); }
        else if (ieq(w, "mod")) { long b = pop(), a = pop(); push(b ? a % b : 0); }
        else if (ieq(w, "negate")) { push(-pop()); }
        else if (ieq(w, "abs")) { long a = pop(); push(a < 0 ? -a : a); }
        else if (ieq(w, "1+")) { push(pop() + 1); }
        else if (ieq(w, "1-")) { push(pop() - 1); }
        else if (ieq(w, "dup")) { long a = pop(); push(a); push(a); }
        else if (ieq(w, "drop")) { pop(); }
        else if (ieq(w, "swap")) { long b = pop(), a = pop(); push(b); push(a); }
        else if (ieq(w, "over")) { long b = pop(), a = pop(); push(a); push(b); push(a); }
        else if (ieq(w, "rot")) { long c = pop(), b = pop(), a = pop(); push(b); push(c); push(a); }
        else if (ieq(w, "nip")) { long b = pop(); pop(); push(b); }
        else if (ieq(w, "tuck")) { long b = pop(), a = pop(); push(b); push(a); push(b); }
        else if (ieq(w, "2dup")) { long b = pop(), a = pop(); push(a); push(b); push(a); push(b); }
        else if (ieq(w, "=")) { long b = pop(), a = pop(); push(a == b ? -1 : 0); }
        else if (ieq(w, "<>")) { long b = pop(), a = pop(); push(a != b ? -1 : 0); }
        else if (ieq(w, "<")) { long b = pop(), a = pop(); push(a < b ? -1 : 0); }
        else if (ieq(w, ">")) { long b = pop(), a = pop(); push(a > b ? -1 : 0); }
        else if (ieq(w, "<=")) { long b = pop(), a = pop(); push(a <= b ? -1 : 0); }
        else if (ieq(w, ">=")) { long b = pop(), a = pop(); push(a >= b ? -1 : 0); }
        else if (ieq(w, "0=")) { push(pop() == 0 ? -1 : 0); }
        else if (ieq(w, "0<")) { push(pop() < 0 ? -1 : 0); }
        else if (ieq(w, "and")) { long b = pop(), a = pop(); push(a & b); }
        else if (ieq(w, "or")) { long b = pop(), a = pop(); push(a | b); }
        else if (ieq(w, "xor")) { long b = pop(), a = pop(); push(a ^ b); }
        else if (ieq(w, "invert")) { push(~pop()); }
        else if (ieq(w, "!")) { long addr = pop(), v = pop(); if (addr >= 0 && addr < VARS_N) vars[addr] = v; }
        else if (ieq(w, "@")) { long addr = pop(); push((addr >= 0 && addr < VARS_N) ? vars[addr] : 0); }
        else if (ieq(w, "?")) { long addr = pop(); print_long((addr >= 0 && addr < VARS_N) ? vars[addr] : 0); print(" "); }
        else if (ieq(w, "+!")) { long addr = pop(), v = pop(); if (addr >= 0 && addr < VARS_N) vars[addr] += v; }
        else if (ieq(w, ".")) { print_long(pop()); print(" "); }
        else if (ieq(w, ".s")) { print("<"); print_long(dp); print("> "); for (int k = 0; k < dp; k++) { print_long(ds[k]); print(" "); } }
        else if (ieq(w, "emit")) { char c = (char)pop(); char s[2] = { c, 0 }; print(s); }
        else if (ieq(w, "cr")) { print("\n"); }
        else if (ieq(w, "space")) { print(" "); }
        else if (ieq(w, "spaces")) { long k = pop(); while (k-- > 0) print(" "); }
        else if (ieq(w, "words")) { for (int k = 0; k < nwords; k++) { print(words[k].name); print(" "); } print("\n"); }
        else if (ieq(w, "depth")) { push(dp); }
        else if (ieq(w, "clear") || ieq(w, "clearstack")) { dp = 0; }
        else { print("forth: ?  "); print(w); print("\n"); err = 1; }
    }
    return err ? -1 : 0;
}

static char script[65536];             /* a loaded .fth file (BSS, not the stack) */

int main(void) {
    /* `forth FILE.FTH` runs a script then exits (a real save-and-run toolchain).
     * Interpreted line by line, so keep each colon definition on one line. */
    char arg[128];
    int ran_script = 0;
    if (sys_getarg(arg, sizeof arg) > 0) {
        long n = sys_readfile(arg, script, sizeof script - 1);
        if (n < 0) { print("forth: cannot read "); print(arg); print("\n"); }
        else {
            print("running "); print(arg); print(":\n");
            script[n] = 0;
            int s = 0;
            for (int i = 0;; i++) {
                if (script[i] == '\n' || script[i] == 0) {
                    char saved = script[i]; script[i] = 0;
                    if (i > s) { err = 0; interp(&script[s], 0); }
                    script[i] = saved; s = i + 1;
                    if (saved == 0) break;
                }
            }
            print("\n");
        }
        ran_script = 1;          /* then drop into the REPL so its output stays visible */
    }
    if (ran_script) goto repl;

    print("OS-DEV Forth -- a stack language you can program in.\n");
    print("  e.g.  : square dup * ;  5 square .      (prints 25)\n");
    print("  words: + - * / mod dup drop swap over rot . .s emit cr\n");
    print("         = < > if/else/then  begin/until  do/loop i  variable ! @\n");
    print("  'words' lists definitions, 'bye' exits.\n\n");

repl:;
    char line[1024];
    for (;;) {
        print("ok> ");
        int n = readline(line, sizeof line);
        if (n <= 0) continue;
        if (ieq(line, "bye") || ieq(line, "quit") || ieq(line, "exit")) break;
        err = 0;
        interp(line, 0);
        if (!err) print(" ok\n");
        else { dp = 0; print("\n"); }      /* on error, clear the stack for a clean prompt */
    }
    return 0;
}
