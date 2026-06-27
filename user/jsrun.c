/*
 * jsrun.c — the from-scratch JavaScript engine, running in RING 3.
 *
 * This is the first concrete step of moving the browser/parser stack out of the
 * kernel (ring 0) and into sandboxed userspace. The whole engine — kernel/js.c,
 * ~4600 lines — is pure compute over an arena with callback-based I/O, so it
 * compiles unchanged for ring 3; the Makefile builds it here with -DJS_RING3
 * (which drops the privileged cli/sti single-flight guard) and a smaller arena.
 *
 * Previously the only way to run JS was the in-kernel SYS_js syscall, i.e. the
 * 4600-line interpreter parsed untrusted script IN THE KERNEL. Now it runs as an
 * ordinary ring-3 program: a bug in the parser/interpreter can crash only this
 * process, not the kernel. (The in-kernel path still exists for the browser until
 * that, too, is migrated — see WHATS-NEXT.md.)
 *
 * Usage: `run jsrun.elf` runs a built-in self-test that exercises recursion,
 * arrays/closures, classes, Math and JSON — all evaluated in ring 3.
 */
#include "ulib.h"
#include "js.h"
#include "rtc.h"
#include <stddef.h>

/* libc string helpers kernel/js.c needs that user/ulib.c doesn't provide (ulib has
 * memset/memcpy/memmove; the engine also uses these three). Signatures match
 * kernel/include/string.h. */
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (int)(unsigned char)*a - (int)(unsigned char)*b; }
int memcmp(const void *a, const void *b, size_t n) { const unsigned char *x = a, *y = b; for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i]; return 0; }

/* kernel/js.c calls rtc_now() for the Date object. In the kernel that reads the
 * CMOS clock; here we provide a userspace stub. (Wiring Date to the real wall
 * clock via a time syscall is a follow-up; the engine itself is the point.) */
void rtc_now(struct rtc_time *t) {
    if (!t) return;
    t->year = 2026; t->month = 6; t->day = 27;
    t->hour = 12; t->min = 0; t->sec = 0;
}

static const char *DEMO =
    "print('from-scratch JavaScript engine, now running in RING 3 (not the kernel):');\n"
    "print('');\n"
    "function fib(n){ return n < 2 ? n : fib(n-1) + fib(n-2); }\n"
    "var s = 'fib(0..11): '; for (var i = 0; i < 12; i++) s += fib(i) + ' '; print(s);\n"
    "var a = [5,3,8,1,9,2]; a.sort((x,y) => x - y);\n"
    "print('sort+reduce: ' + JSON.stringify(a) + ' sum=' + a.reduce((p,c) => p + c, 0));\n"
    "print('map/filter:  ' + [1,2,3,4,5,6].map(x => x*x).filter(x => x % 2 == 0).join(','));\n"
    "class Pt { constructor(x,y){ this.x = x; this.y = y; } toString(){ return '(' + this.x + ',' + this.y + ')'; } }\n"
    "print('class:       ' + new Pt(3,4));\n"
    "print('Math/JSON:   sqrt2=' + Math.sqrt(2).toFixed(5) + ' obj=' + JSON.stringify({os:'OS-DEV', ring:3}));\n";

int main(void) {
    static char out[256 * 1024];
    static char src[200 * 1024];

    /* Run JSIN.JS if the shell handed us a script there; otherwise the built-in
     * self-test. The shell's `js` command writes the user's source to JSIN.JS and
     * spawns us, so the JS engine runs in RING 3 for the shell path too. */
    const char *js = DEMO;
    long n = sys_readfile("JSIN.JS", src, sizeof(src) - 1);
    if (n > 0) { src[n] = 0; js = src; }

    int r = js_run(js, out, (int)sizeof(out) - 1);
    out[sizeof(out) - 1] = 0;
    print(out);
    print("\n");
    /* jsrun is its own process, so its stdout isn't the shell's terminal; persist
     * the result to a file the shell (or `cat JSOUT.TXT`) can read back. */
    sys_writefile("JSOUT.TXT", out, (unsigned long)strlen(out));
    return r < 0 ? 1 : 0;
}
