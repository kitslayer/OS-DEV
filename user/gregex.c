/*
 * gregex.c — an interactive regular-expression tester (M1704).
 *
 * The OS has a genuinely serious regular-expression engine — the one inside its
 * from-scratch JavaScript engine (kernel/js.c: alternation, groups incl. named,
 * backreferences, lookaround, character classes, the usual quantifiers). Until
 * now the only way to reach it was to write JavaScript. This surfaces it: type a
 * pattern, flags and a test string, and see the matches highlighted live with
 * their capture groups.
 *
 * Rather than reimplement a weaker matcher, this REUSES that engine: it builds a
 * tiny JS program (`new RegExp(pat,flags)` + an `exec` loop that prints each
 * match's offset/length/groups) and runs it through js_run() — the same ring-3
 * JS path as `jsrun` — then parses the printed result. So the matching is
 * exactly the browser's / JS engine's, not a second implementation.
 *
 * It also does substitution: fill in the `replace` field and the result of
 * subject.replace(re, replacement) is shown live (the engine's real String.replace,
 * so $1/$&/$` capture references work). Keys: type to edit the active field ·
 *   Tab cycles pattern -> flags -> subject -> replace · Backspace deletes · Esc
 *   quits.  Launch: `gregex` or the Apps menu.
 */
#include "ulib.h"
#include "js.h"          /* js_run(src, out, outmax) — runs JS, captures print() output */
#include "rtc.h"
#include <stddef.h>

/* String/RTC helpers kernel/js.c needs that ulib doesn't provide — identical to
 * jsrun.c's (that app links the same engine the same way). */
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (int)(unsigned char)*a - (int)(unsigned char)*b; }
int memcmp(const void *a, const void *b, size_t n) { const unsigned char *x = a, *y = b; for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i]; return 0; }
void rtc_now(struct rtc_time *t) { if (!t) return; t->year = 2026; t->month = 6; t->day = 27; t->hour = 12; t->min = 0; t->sec = 0; }

#define PATMAX  128
#define SUBJMAX 256
#define OUTMAX  16384
#define MAXM    256

static char pat[PATMAX];   static int patlen;
static char flags[8] = "g"; static int flagslen = 1;
static char subj[SUBJMAX]; static int subjlen;
static char repl[SUBJMAX]; static int repllen;   /* replacement string ($1.. per JS replace) — M1742 */
static int  field;                         /* 0 = pattern, 1 = flags, 2 = subject, 3 = replacement */

static char jsbuf[4096];
static char out[OUTMAX];
static char status[80];
static int  mcount;
static int  midx[MAXM], mlen[MAXM];
static char mgrp[MAXM][56];                /* a short "group1 group2 ..." summary per match */
static char replaced[2048]; static int has_repl;   /* s.replace(re, repl) preview — M1742 */

static int  slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scopy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }
static void putn(int v) { char b[12]; int n = 0; if (v == 0) b[n++] = '0'; while (v) { b[n++] = (char)('0' + v % 10); v /= 10; } char c[2] = {0,0}; while (n) { c[0] = b[--n]; print(c); } }

/* Append `s` (length n) to jsbuf[p] as a JS string literal, escaped. */
static int js_lit(int p, const char *s, int n) {
    jsbuf[p++] = '"';
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { jsbuf[p++] = '\\'; jsbuf[p++] = (char)c; }
        else if (c == '\n') { jsbuf[p++] = '\\'; jsbuf[p++] = 'n'; }
        else if (c == '\r') { jsbuf[p++] = '\\'; jsbuf[p++] = 'r'; }
        else if (c == '\t') { jsbuf[p++] = '\\'; jsbuf[p++] = 't'; }
        else if (c < 0x20) { const char *h = "0123456789abcdef"; jsbuf[p++]='\\'; jsbuf[p++]='u'; jsbuf[p++]='0'; jsbuf[p++]='0'; jsbuf[p++]=h[(c>>4)&15]; jsbuf[p++]=h[c&15]; }
        else jsbuf[p++] = (char)c;
    }
    jsbuf[p++] = '"';
    return p;
}
static int js_cat(int p, const char *s) { while (*s) jsbuf[p++] = *s++; return p; }

/* Build the JS program that runs the regex and prints one line per match:
 *   "OK\n<index> <len>[\t<group>]...\n..."   |   "NONE"   |   "ERR <message>" */
static void build_js(void) {
    int p = 0;
    p = js_cat(p, "try{var re=new RegExp(");
    p = js_lit(p, pat, patlen);
    p = js_cat(p, ",");
    p = js_lit(p, flags, flagslen);
    p = js_cat(p, ");var s=");
    p = js_lit(p, subj, subjlen);
    p = js_cat(p, ";var o='',m,n=0;"
        "if(re.global){while((m=re.exec(s))!=null){o+=m.index+' '+m[0].length;"
        "for(var i=1;i<m.length;i++)o+='\\t'+(m[i]==null?'':m[i]);o+='\\n';n++;"
        "if(m[0].length==0)re.lastIndex++;if(n>256)break;}}"
        "else{m=re.exec(s);if(m!=null){o+=m.index+' '+m[0].length;"
        "for(var i=1;i<m.length;i++)o+='\\t'+(m[i]==null?'':m[i]);o+='\\n';}}"
        "print(o.length?('OK\\n'+o):'NONE');}catch(e){print('ERR '+e);}");
    jsbuf[p] = 0;
}

/* Build the JS program that applies the replacement: prints 'R'<result> (the
 * leading R distinguishes it from a catch's 'ERR '). Uses the engine's real
 * String.replace, so $1/$&/$` semantics are exactly the JS engine's. */
static void build_replace_js(void) {
    int p = 0;
    p = js_cat(p, "try{var re=new RegExp(");
    p = js_lit(p, pat, patlen);
    p = js_cat(p, ",");
    p = js_lit(p, flags, flagslen);
    p = js_cat(p, ");var s=");
    p = js_lit(p, subj, subjlen);
    p = js_cat(p, ";print('R'+s.replace(re,");
    p = js_lit(p, repl, repllen);
    p = js_cat(p, "));}catch(e){print('ERR '+e);}");
    jsbuf[p] = 0;
}

static void run(void) {
    mcount = 0; status[0] = 0; has_repl = 0;
    if (patlen == 0) { scopy(status, "enter a pattern", sizeof status); return; }
    if (repllen > 0) {                                 /* replacement preview (M1742), independent of the match list */
        build_replace_js();
        js_run(jsbuf, out, OUTMAX - 1); out[OUTMAX - 1] = 0;
        if (out[0] == 'R') { scopy(replaced, out + 1, sizeof replaced); int e = slen(replaced); if (e > 0 && replaced[e - 1] == '\n') replaced[e - 1] = 0; has_repl = 1; }
    }
    build_js();
    js_run(jsbuf, out, OUTMAX - 1);
    out[OUTMAX - 1] = 0;
    if (startswith(out, "NONE")) { scopy(status, "no match", sizeof status); return; }
    if (startswith(out, "ERR ")) { scopy(status, "error: ", sizeof status); int l = slen(status); scopy(status + l, out + 4, (int)sizeof status - l); return; }
    if (!startswith(out, "OK\n")) { scopy(status, "engine error", sizeof status); return; }
    char *c = out + 3;
    while (*c && mcount < MAXM) {
        if (*c == '\n') { c++; continue; }             /* skip blank lines (e.g. the trailing newline) */
        int idx = 0, len = 0, got = 0;
        while (*c >= '0' && *c <= '9') { idx = idx * 10 + (*c - '0'); c++; got = 1; }
        if (!got) { while (*c && *c != '\n') c++; if (*c == '\n') c++; continue; }   /* not a match line */
        if (*c == ' ') c++;
        while (*c >= '0' && *c <= '9') { len = len * 10 + (*c - '0'); c++; }
        midx[mcount] = idx; mlen[mcount] = len;
        int g = 0;                                 /* tab-separated groups -> "g1  g2" */
        while (*c && *c != '\n') {
            char ch = (*c == '\t') ? ' ' : *c;
            if (g < 54) mgrp[mcount][g++] = ch;
            c++;
        }
        mgrp[mcount][g] = 0;
        mcount++;
        if (*c == '\n') c++;
    }
    scopy(status, "", sizeof status);
    int l = 0; { char nb[16]; int n = 0, v = mcount; if (v == 0) nb[n++]='0'; while (v){nb[n++]=(char)('0'+v%10);v/=10;} while(n) status[l++]=nb[--n]; }
    scopy(status + l, mcount == 1 ? " match" : " matches", (int)sizeof status - l);
}

static void field_row(int f, const char *label, const char *val, int vlen) {
    sys_setcolor(f == field ? 3 : 8); print(label);
    sys_setcolor(f == field ? 1 : 7);
    char c[2] = {0,0};
    for (int i = 0; i < vlen; i++) { c[0] = val[i]; print(c); }
    if (f == field) { sys_setcolor(4); print("_"); }   /* caret on the active field */
    sys_setcolor(0); print("\n");
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print(" regex "); sys_setcolor(8); print(" (Tab switches field, Esc quits)\n\n");
    field_row(0, "pattern: ", pat, patlen);
    field_row(1, "flags:   ", flags, flagslen);
    field_row(2, "subject: ", subj, subjlen);
    field_row(3, "replace: ", repl, repllen);
    print("\n");

    /* status line */
    if (startswith(status, "error:")) sys_setcolor(2);
    else if (mcount > 0) sys_setcolor(10);
    else sys_setcolor(8);
    print(" "); print(status[0] ? status : "type a pattern and a subject"); print("\n\n");
    sys_setcolor(0);

    /* the subject with matched ranges highlighted */
    if (subjlen > 0) {
        sys_setcolor(8); print(" match: ");
        char c[2] = {0,0};
        for (int i = 0; i < subjlen; i++) {
            int hit = 0;
            for (int m = 0; m < mcount; m++) if (i >= midx[m] && i < midx[m] + mlen[m]) { hit = 1; break; }
            sys_setcolor(hit ? 3 : 7);         /* matched chars yellow, rest grey */
            c[0] = subj[i]; print(c);
        }
        sys_setcolor(0); print("\n\n");
    }

    /* list the first matches with their groups */
    for (int m = 0; m < mcount && m < 11; m++) {
        sys_setcolor(8); print("  @"); putn(midx[m]); print(" ");
        sys_setcolor(3); char c[2] = {0,0};
        for (int i = midx[m]; i < midx[m] + mlen[m] && i < subjlen; i++) { c[0] = subj[i]; print(c); }
        if (mlen[m] == 0) { sys_setcolor(8); print("(empty)"); }
        if (mgrp[m][0]) { sys_setcolor(6); print("   groups:"); print(mgrp[m]); }
        sys_setcolor(0); print("\n");
    }
    if (mcount > 11) { sys_setcolor(8); print("  ... "); putn(mcount - 11); print(" more\n"); sys_setcolor(0); }

    /* replacement result (when the `replace` field is non-empty) — M1742 */
    if (has_repl) {
        sys_setcolor(8); print("\n replaced: "); sys_setcolor(11); print(replaced); sys_setcolor(0); print("\n");
    }
}

static void edit_field(int k) {
    char *buf; int *len; int max;
    if (field == 0) { buf = pat; len = &patlen; max = PATMAX; }
    else if (field == 1) { buf = flags; len = &flagslen; max = 8; }
    else if (field == 2) { buf = subj; len = &subjlen; max = SUBJMAX; }
    else { buf = repl; len = &repllen; max = SUBJMAX; }
    if (k == 8 || k == 127) { if (*len > 0) buf[--(*len)] = 0; }
    else if (k >= 32 && k < 127) { if (*len < max - 1) { buf[(*len)++] = (char)k; buf[*len] = 0; } }
}

int main(void) {
    scopy(pat, "\\w+", PATMAX); patlen = slen(pat);          /* a friendly default */
    scopy(subj, "the OS-DEV regex engine, reused from js.c", SUBJMAX); subjlen = slen(subj);
    run();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(15); continue; }
        if (k == 27) break;
        else if (k == '\t') { field = (field + 1) % 4; }
        else { edit_field(k); run(); }
        render();
    }
    sys_clear();
    return 0;
}
