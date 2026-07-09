/*
 * gjson.c — a JSON viewer / pretty-printer, a userspace program (M1703).
 *
 * The OS has a complete JSON implementation inside the JS engine, but no tool a
 * user could point at a file. This is that tool: it validates and re-indents
 * JSON and shows it with syntax colouring in a scrollable text window — handy
 * for reading a config or an API response fetched with `httpget`. The whole
 * validator/pretty-printer lives in user/jsoncore.h (pure, no syscalls, no
 * floating point), host-unit-tested by tests/json exactly like calc/sheet/plot.
 * This file is just the terminal UI, the file load, and the scroll loop.
 *
 * Launch: `gjson [file]` from the shell, or the Apps menu (a built-in demo).
 * Keys: up/down scroll a line, left/right page, Esc/q quit.
 */
#include "ulib.h"
#include "jsoncore.h"        /* the pure validator + pretty-printer (host-tested by tests/json) */

#define IOMAX    16384
#define FMTMAX   49152
#define VIEWROWS 21
#define MAXLINES 8192

static char iobuf[IOMAX];
static char fmtbuf[FMTMAX];
static char fname[64];
static int  err;                          /* json_format result: -1 ok, >=0 error offset, -2 no file */
static int  nlines, top;
static int  linestart[MAXLINES];

static const char *DEMO =
    "{\"name\":\"OS-DEV\",\"kind\":\"from-scratch x86_64 OS\",\"milestone\":1703,"
    "\"features\":[\"kernel\",\"TLS 1.3\",\"JS engine\",\"browser\",\"spreadsheet\",\"graphing calc\",\"paint\"],"
    "\"stats\":{\"apps\":93,\"testSuites\":73,\"selfHosted\":true},"
    "\"note\":\"validated + pretty-printed by our own JSON engine\",\"nested\":[[1,2],[3,[4,5]]]}";

static int  slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scopy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }
static void print_int(int v) {
    char b[12]; int n = 0;
    unsigned u = v < 0 ? (print("-"), (unsigned)(-v)) : (unsigned)v;
    if (u == 0) b[n++] = '0';
    while (u) { b[n++] = (char)('0' + u % 10); u /= 10; }
    char c[2] = { 0, 0 };
    while (n) { c[0] = b[--n]; print(c); }
}

static void index_lines(void) {
    nlines = 0; linestart[nlines++] = 0;
    for (int i = 0; fmtbuf[i] && nlines < MAXLINES; i++)
        if (fmtbuf[i] == '\n') linestart[nlines++] = i + 1;
}

static void load(void) {
    const char *src;
    if (fname[0]) {
        long n = sys_readfile(fname, iobuf, IOMAX - 1);
        if (n < 0) { err = -2; return; }
        iobuf[n] = 0; src = iobuf;
    } else src = DEMO;
    err = json_format(src, fmtbuf, FMTMAX);
    index_lines();
}

/* print one formatted line with JSON syntax colouring */
static void print_line(int li) {
    int s = linestart[li];
    int e = (li + 1 < nlines) ? linestart[li + 1] - 1 : slen(fmtbuf);   /* exclude the '\n' */
    if (e < s) e = s;
    int instr = 0; char b[2] = { 0, 0 };
    for (int i = s; i < e; i++) {
        char c = fmtbuf[i];
        if (instr) {
            sys_setcolor(10); b[0] = c; print(b);            /* string body: teal */
            if (c == '\\' && i + 1 < e) { b[0] = fmtbuf[++i]; print(b); continue; }
            if (c == '"') instr = 0;
            continue;
        }
        if (c == '"') { instr = 1; sys_setcolor(10); }
        else if (c == '{' || c == '}' || c == '[' || c == ']' || c == ':' || c == ',') sys_setcolor(8);  /* punctuation grey */
        else if ((c >= '0' && c <= '9') || c == '-') sys_setcolor(4);   /* number cyan */
        else if (c >= 'a' && c <= 'z') sys_setcolor(3);                  /* true/false/null yellow */
        else sys_setcolor(1);
        b[0] = c; print(b);
    }
    sys_setcolor(0);
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print(" json "); sys_setcolor(1);
    print(fname[0] ? fname : "(demo)");
    if (err == -2)      { sys_setcolor(2); print("  file not found"); }
    else if (err >= 0)  { sys_setcolor(2); print("  INVALID  syntax error @ offset "); print_int(err); }
    else                { sys_setcolor(10); print("  valid"); sys_setcolor(8); print("  ("); print_int(nlines); print(" lines)"); }
    sys_setcolor(0); print("\n");

    if (err == -2) { print("\n  could not read "); print(fname); print("\n"); sys_setcolor(0); return; }
    for (int r = 0; r < VIEWROWS; r++) {
        int li = top + r;
        if (li >= nlines) { print("\n"); continue; }
        print_line(li); print("\n");
    }
    sys_setcolor(8);
    if (err >= 0) print(" (showing the valid prefix)  ");
    print(" up/dn scroll  left/right page  Esc quit");
    sys_setcolor(0);
}

int main(void) {
    char arg[64];
    if (sys_getarg(arg, sizeof arg) > 0 && arg[0] && !streq(arg, "demo")) scopy(fname, arg, sizeof fname);
    load();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(15); continue; }
        if (k == 27 || k == 'q') break;
        else if (k == 0x11) { if (top > 0) top--; }                                  /* up */
        else if (k == 0x12) { if (top < nlines - 1) top++; }                         /* down */
        else if (k == 0x13) { top -= VIEWROWS; if (top < 0) top = 0; }               /* left: page up */
        else if (k == 0x14) { top += VIEWROWS; if (top > nlines - 1) top = nlines - 1; if (top < 0) top = 0; }  /* right: page down */
        else continue;
        render();
    }
    sys_clear();
    return 0;
}
