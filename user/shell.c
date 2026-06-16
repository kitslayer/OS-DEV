/*
 * shell.c — a tiny interactive shell, running entirely in ring 3.
 *
 * It loops: print a prompt, read a line via SYS_read, and match it against a
 * few built-in commands. Everything it does — printing, reading input, exiting
 * — is a system call into the kernel. This is a real (if minimal) userspace
 * program talking to our OS exactly the way `sh` talks to Linux.
 */
#include "ulib.h"

static void itoa_simple(int v, char *out) {
    char tmp[12];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v > 0) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
}
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }   /* ASCII lowercase */
static void printl(long v) {              /* print a (possibly large) integer */
    char t[24]; int i = 0;
    unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
    if (v < 0) print("-");
    if (!u) t[i++] = '0';
    while (u) { t[i++] = (char)('0' + u % 10); u /= 10; }
    char o[24]; int j = 0;
    while (i) o[j++] = t[--i];
    o[j] = '\0'; print(o);
}
static void print_base(unsigned long n, int base) {   /* print n in base 2-16 */
    const char *dgt = "0123456789abcdef";
    char t[72]; int i = 0;
    if (!n) t[i++] = '0';
    while (n) { t[i++] = dgt[n % (unsigned)base]; n /= (unsigned)base; }
    char o[72]; int j = 0;
    while (i) o[j++] = t[--i];
    o[j] = '\0'; print(o);
}
static unsigned shell_rng = 0;            /* lazily seeded from the clock on first use */
static unsigned shroll(void) {
    if (!shell_rng) {
        char tb[40]; long tn = sys_time(tb, sizeof(tb));
        shell_rng = 0x2545F491u;
        for (long i = 0; i < tn; i++) shell_rng = shell_rng * 31u + (unsigned char)tb[i];
        if (!shell_rng) shell_rng = 12345u;
    }
    shell_rng ^= shell_rng << 13; shell_rng ^= shell_rng >> 17; shell_rng ^= shell_rng << 5;
    return shell_rng;
}

/* Print a month calendar for the current date (from the RTC). */
static void cmd_cal_ym(int y, int m, int today) {   /* render month m (1-12) of year y; today=0 -> no highlight */
    if (m < 1 || m > 12) { print("cal: bad date\n"); return; }

    static const int mdays[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int dim = mdays[m-1];
    if (m == 2 && ((y%4==0 && y%100!=0) || y%400==0)) dim = 29;

    /* Sakamoto's day-of-week for the 1st (0=Sun) */
    static const int tt[] = { 0,3,2,5,0,3,5,1,4,6,2,4 };
    int yy = y - (m < 3);
    int dow = (yy + yy/4 - yy/100 + yy/400 + tt[m-1] + 1) % 7;

    static const char *mn[] = { "January","February","March","April","May","June",
        "July","August","September","October","November","December" };
    char yb[8]; itoa_simple(y, yb);
    print("    "); print(mn[m-1]); print(" "); print(yb); print("\n");
    print(" Su Mo Tu We Th Fr Sa\n");
    char line[32]; int p = 0;
    for (int i = 0; i < dow; i++) { line[p++]=' '; line[p++]=' '; line[p++]=' '; }
    for (int d = 1; d <= dim; d++) {                 /* 3-char cells: "> " marks today */
        line[p++] = (d == today) ? '>' : ' ';
        line[p++] = (d < 10) ? ' ' : (char)('0' + d/10);
        line[p++] = (char)('0' + d%10);
        if ((dow + d) % 7 == 0) { line[p] = 0; print(line); print("\n"); p = 0; }
    }
    if (p) { line[p] = 0; print(line); print("\n"); }
}
static void cmd_cal(void) {                          /* current month, today highlighted */
    char t[24];
    sys_time(t, sizeof(t));                          /* "YYYY-MM-DD HH:MM:SS" */
    int y = (t[0]-'0')*1000 + (t[1]-'0')*100 + (t[2]-'0')*10 + (t[3]-'0');
    int m = (t[5]-'0')*10 + (t[6]-'0');
    int today = (t[8]-'0')*10 + (t[9]-'0');
    cmd_cal_ym(y, m, today);
}

int main(void) {
    print("\n");
    print("  OS-DEV shell v0.1 - running in userspace (ring 3)\n");
    print("  type 'help' for commands\n\n");

    char line[128];
    char cwd[128]; cwd[0] = '/'; cwd[1] = 0;       /* display path (kernel tracks the real cwd) */
    for (;;) {
        print("osdev:"); print(cwd); print("$ ");
        readline(line, sizeof(line));

        if (line[0] == '\0') {
            continue;
        } else if (streq(line, "help")) {
            print("files:  ls cat head tail sort nl tac uniq cut cmp<f1 f2> edit write rm cp mv mkdir cd pwd tree find grep hexdump strings<file> unhex<hex> wc[-lwc] tr fold\n");
            print("net:    get<url> headers<url> wget<url file> browse<url>\n");
            print("        ping[<host>] resolve<host> ifconfig\n");
            print("crypto: sha256<file> sha512<file> crc32<file> genpass[ N] uuidgen crypt base64 unbase64<b64>\n");
            print("        run: apps run<prog> js<file>\n");
            print("math:   factor<n> roll<NdM> seq<n> base<N> dec<0x..> roman<N> gcd<a b> primes<N> fib<N> fizzbuzz<N> stats<n..> size<bytes>\n");
            print("misc:   echo cal[ M Y] weekday<YYYYMMDD> dur<sec> date beep morse<text> unmorse<code> rev<text> rot13<text> ascii cowsay<text> fortune\n");
            print("        todo[ add T|done N|clear] mem ps df history clear reboot exit\n");
        } else if (streq(line, "ls")) {
            char buf[1024];
            sys_list(buf, sizeof(buf));
            print(buf);
        } else if (startswith(line, "cat ")) {
            char buf[2048];
            const char *p = line + 4; int any = 0;
            while (*p) {                                  /* concatenate each space-separated file */
                while (*p == ' ') p++;
                if (!*p) break;
                char name[64]; int i = 0;
                while (*p && *p != ' ' && i < 63) name[i++] = *p++;
                name[i] = '\0'; any = 1;
                long n = sys_readfile(name, buf, sizeof(buf) - 1);
                if (n < 0) { print("cat: no such file: "); print(name); print("\n"); }
                else { buf[n] = '\0'; print(buf); }
            }
            if (!any) print("usage: cat <file>...\n");
        } else if (startswith(line, "head ")) {
            char buf[2048];
            const char *p = line + 5;
            while (*p == ' ') p++;
            int cnt = 20;                                  /* default; -N sets the line count */
            if (*p == '-' && p[1] >= '0' && p[1] <= '9') { p++; cnt = 0; while (*p >= '0' && *p <= '9' && cnt < 100000000) cnt = cnt * 10 + (*p++ - '0'); if (cnt < 1) cnt = 20; while (*p == ' ') p++; }
            const char *cq = p; int fc = 0;                /* count files -> name headers only if >1 */
            while (*cq) { while (*cq == ' ') cq++; if (!*cq) break; fc++; while (*cq && *cq != ' ') cq++; }
            int any = 0;
            while (*p) {                                   /* first 20 lines of each space-separated file */
                while (*p == ' ') p++;
                if (!*p) break;
                char name[64]; int j = 0;
                while (*p && *p != ' ' && j < 63) name[j++] = *p++;
                name[j] = '\0'; any = 1;
                long n = sys_readfile(name, buf, sizeof(buf) - 1);
                if (n < 0) { print("head: no such file: "); print(name); print("\n"); continue; }
                if (fc > 1) { print("==> "); print(name); print(" <==\n"); }
                int i = 0, lines = 0;
                for (; i < n && lines < cnt; i++) if (buf[i] == '\n') lines++;
                buf[i] = '\0'; print(buf);
                if (i < n) print("...\n");
            }
            if (!any) print("usage: head <file>...\n");
        } else if (startswith(line, "nl ")) {
            static char buf[8192];
            long n = sys_readfile(line + 3, buf, sizeof(buf) - 1);
            if (n < 0) { print("nl: no such file: "); print(line + 3); print("\n"); }
            else {
                int ln = 1, start = 0; char num[12];
                for (int i = 0; i < (int)n; i++) {
                    if (buf[i] == '\n') {
                        buf[i] = '\0';
                        itoa_simple(ln++, num);
                        print("  "); print(num); print("  "); print(buf + start); print("\n");
                        start = i + 1;
                    }
                }
                if (start < (int)n) {                       /* final line with no trailing newline */
                    buf[n] = '\0';
                    itoa_simple(ln, num);
                    print("  "); print(num); print("  "); print(buf + start); print("\n");
                }
            }
        } else if (startswith(line, "tail ")) {
            char buf[2048];
            const char *p = line + 5;
            while (*p == ' ') p++;
            int cnt = 20;                                  /* default; -N sets the line count */
            if (*p == '-' && p[1] >= '0' && p[1] <= '9') { p++; cnt = 0; while (*p >= '0' && *p <= '9' && cnt < 100000000) cnt = cnt * 10 + (*p++ - '0'); if (cnt < 1) cnt = 20; while (*p == ' ') p++; }
            const char *cq = p; int fc = 0;
            while (*cq) { while (*cq == ' ') cq++; if (!*cq) break; fc++; while (*cq && *cq != ' ') cq++; }
            int any = 0;
            while (*p) {                                   /* last 20 lines of each space-separated file */
                while (*p == ' ') p++;
                if (!*p) break;
                char name[64]; int j = 0;
                while (*p && *p != ' ' && j < 63) name[j++] = *p++;
                name[j] = '\0'; any = 1;
                long n = sys_readfile(name, buf, sizeof(buf) - 1);
                if (n < 0) { print("tail: no such file: "); print(name); print("\n"); continue; }
                if (fc > 1) { print("==> "); print(name); print(" <==\n"); }
                buf[n] = '\0';
                int total = 0;
                for (int i = 0; i < n; i++) if (buf[i] == '\n') total++;
                if (n > 0 && buf[n - 1] != '\n') total++;
                int skip = total > cnt ? total - cnt : 0;     /* keep the last cnt lines */
                int i = 0, sk = 0;
                while (i < n && sk < skip) { if (buf[i++] == '\n') sk++; }
                print(buf + i);
            }
            if (!any) print("usage: tail <file>...\n");
        } else if (startswith(line, "tac ")) {           /* print a file's lines in reverse order */
            static char buf[2048];
            long n = sys_readfile(line + 4, buf, sizeof(buf) - 1);
            if (n < 0) { print("tac: no such file: "); print(line + 4); print("\n"); }
            else {
                buf[n] = 0;
                static int starts[1024]; int ns = 0; starts[ns++] = 0;   /* 1024 >= max lines in a 2047-byte buffer */
                for (long i = 0; i < n; i++) if (buf[i] == '\n' && ns < 1024) starts[ns++] = (int)(i + 1);
                for (int k = ns - 1; k >= 0; k--) {
                    int s = starts[k]; if (s >= (int)n) continue;     /* skip empty trailing line */
                    int e = s; while (e < (int)n && buf[e] != '\n') e++;
                    char save = buf[e]; buf[e] = 0;
                    print(buf + s); print("\n");
                    buf[e] = save;
                }
            }
        } else if (startswith(line, "uniq ")) {           /* drop adjacent duplicate lines */
            static char buf[2048];
            long n = sys_readfile(line + 5, buf, sizeof(buf) - 1);
            if (n < 0) { print("uniq: no such file: "); print(line + 5); print("\n"); }
            else {
                buf[n] = 0;
                int ps = -1, pl = -1, ls = 0;              /* previous printed line [ps, ps+pl) */
                for (long k = 0; k <= n; k++) {
                    if (k == n || buf[k] == '\n') {
                        int len = (int)(k - ls);
                        if (k == n && len == 0) { ls = (int)k + 1; continue; }   /* skip empty trailing */
                        int same = (pl == len);
                        if (same) for (int j = 0; j < len; j++) if (buf[ls + j] != buf[ps + j]) { same = 0; break; }
                        if (!same) {
                            char save = buf[k]; buf[k] = 0;
                            print(buf + ls); print("\n");
                            buf[k] = save;
                            ps = ls; pl = len;
                        }
                        ls = (int)k + 1;
                    }
                }
            }
        } else if (startswith(line, "sort ")) {
            static char buf[2048];
            const char *fp = line + 5; int rev = 0;        /* -r: reverse (descending) */
            while (*fp == ' ') fp++;
            while (fp[0] == '-' && fp[1] && fp[1] != ' ') {
                int t, valid = 1;
                for (t = 1; fp[t] && fp[t] != ' '; t++) if (fp[t] != 'r') valid = 0;
                if (!valid) break;                          /* not a flag token (e.g. a filename) */
                rev = 1; fp += t; while (*fp == ' ') fp++;
            }
            long n = sys_readfile(fp, buf, sizeof(buf) - 1);
            if (n < 0) { print("sort: no such file: "); print(fp); print("\n"); }
            else {
                buf[n] = '\0';
                char *lns[128]; int nl = 0; char *p = buf;
                while (*p && nl < 128) {                       /* split into lines */
                    lns[nl++] = p;
                    while (*p && *p != '\n') p++;
                    if (*p == '\n') *p++ = '\0';
                }
                for (int i = 1; i < nl; i++) {                 /* insertion sort (byte order) */
                    char *key = lns[i]; int j = i - 1;
                    while (j >= 0) {
                        const char *a = lns[j], *b = key;
                        while (*a && *a == *b) { a++; b++; }
                        int cmp = (int)(unsigned char)*a - (int)(unsigned char)*b;
                        if (rev) cmp = -cmp;
                        if (cmp <= 0) break;                   /* lns[j] already in order vs key */
                        lns[j+1] = lns[j]; j--;
                    }
                    lns[j+1] = key;
                }
                for (int i = 0; i < nl; i++) { print(lns[i]); print("\n"); }
            }
        } else if (startswith(line, "tr ")) {              /* tr -d CHARS FILE (delete) | tr OLD NEW FILE (replace one char) */
            static char buf[2048];
            const char *p = line + 3; while (*p == ' ') p++;
            if (p[0] == '-' && p[1] == 'd' && p[2] == ' ') {
                p += 3; while (*p == ' ') p++;
                char del[16]; int dn = 0;
                while (*p && *p != ' ' && dn < 15) del[dn++] = *p++;
                while (*p == ' ') p++;
                long n = sys_readfile(p, buf, sizeof(buf) - 1);
                if (n < 0) { print("tr: no such file: "); print(p); print("\n"); }
                else {
                    long oi = 0;
                    for (long i = 0; i < n; i++) {
                        int drop = 0;
                        for (int j = 0; j < dn; j++) if (buf[i] == del[j]) { drop = 1; break; }
                        if (!drop) buf[oi++] = buf[i];          /* compact in place (oi <= i) */
                    }
                    buf[oi] = 0; print(buf);
                }
            } else {
                char oldc = *p; if (oldc) p++; while (*p == ' ') p++;
                char newc = *p; if (newc) p++; while (*p == ' ') p++;
                long n = sys_readfile(p, buf, sizeof(buf) - 1);
                if (n < 0 || !oldc || !newc) { print("usage: tr OLD NEW FILE  |  tr -d CHARS FILE\n"); }
                else { buf[n] = 0; for (long i = 0; i < n; i++) if (buf[i] == oldc) buf[i] = newc; print(buf); }
            }
        } else if (startswith(line, "fold ")) {           /* fold [-w]N FILE : wrap each line at N columns (default 60) */
            static char buf[2048], fout[2400];
            const char *p = line + 5; while (*p == ' ') p++;
            int w = 0;
            if (p[0] == '-' && p[1] == 'w') p += 2;
            while (*p >= '0' && *p <= '9') w = w * 10 + (*p++ - '0');
            while (*p == ' ') p++;
            if (w < 1) w = 60;
            if (w > 200) w = 200;
            long n = sys_readfile(p, buf, sizeof(buf) - 1);
            if (n < 0) { print("fold: no such file: "); print(p); print("\n"); }
            else {
                buf[n] = 0;
                int oi = 0, col = 0;
                for (long i = 0; i < n && oi < 2390; i++) {
                    char c = buf[i];
                    if (c == '\n') { fout[oi++] = '\n'; col = 0; continue; }
                    fout[oi++] = c; col++;
                    if (col >= w && oi < 2390) { fout[oi++] = '\n'; col = 0; }
                }
                if (oi > 0 && fout[oi-1] != '\n') fout[oi++] = '\n';
                fout[oi] = 0; print(fout);
            }
        } else if (startswith(line, "cut ")) {            /* cut -cN[-M] FILE : keep a 1-based char range of each line */
            static char buf[2048];
            const char *p = line + 4; while (*p == ' ') p++;
            if (p[0] != '-' || p[1] != 'c') { print("usage: cut -cN[-M] <file>  (e.g. cut -c1-5 FILE)\n"); }
            else {
                p += 2;
                int from = 0, to = 0, openend = 0;
                while (*p >= '0' && *p <= '9') { if (from < 100000000) from = from * 10 + (*p - '0'); p++; }   /* cap: no int overflow on absurd N */
                if (*p == '-') { p++; if (*p >= '0' && *p <= '9') { while (*p >= '0' && *p <= '9') { if (to < 100000000) to = to * 10 + (*p - '0'); p++; } } else openend = 1; }
                else to = from;                            /* -cN alone = just column N */
                while (*p == ' ') p++;
                if (from < 1) from = 1;
                long n = sys_readfile(p, buf, sizeof(buf) - 1);
                if (n < 0) { print("cut: no such file: "); print(p); print("\n"); }
                else {
                    buf[n] = 0;
                    char out[256]; int oi = 0, col = 0;
                    for (long k = 0; k < n; k++) {
                        if (buf[k] == '\n') { out[oi] = 0; print(out); print("\n"); oi = 0; col = 0; continue; }
                        col++;
                        if (col >= from && (openend || col <= to) && oi < 255) out[oi++] = buf[k];
                    }
                    if (col > 0) { out[oi] = 0; print(out); print("\n"); }   /* trailing line w/o newline, even if its slice is empty */
                }
            }
        } else if (streq(line, "js") || startswith(line, "js ")) {
            static char src[8192];
            static char out[8192];
            int have = 0;
            if (streq(line, "js")) {                 /* no file: run a built-in demo */
                const char *demo =
                    "print(\"Hello from from-scratch JavaScript!\");\n"
                    "function fib(n){ return n<2 ? n : fib(n-1)+fib(n-2); }\n"
                    "var s=\"fib: \"; for (var i=0;i<12;i++) s+=fib(i)+\" \"; print(s);\n"
                    "var a=[3,1,2]; a.push(4); print(\"array: len \"+a.length+\" = [\"+a.join(\",\")+\"]\");\n"
                    "var o={name:\"OS-DEV\", year:2026}; print(o.name+\" \"+o.year);\n"
                    "function fact(n){ return n<=1?1:n*fact(n-1); } print(\"6! = \"+fact(6));\n"
                    "print(\"2^10 = \"+(1<<10)+\", 17%5 = \"+(17%5));\n";
                int i = 0; while (demo[i] && i < (int)sizeof(src) - 1) { src[i] = demo[i]; i++; } src[i] = 0;
                have = 1;
            } else if (startswith(line, "js -e ")) {     /* inline: js -e <code> */
                const char *code = line + 6;
                int i = 0; while (code[i] && i < (int)sizeof(src) - 1) { src[i] = code[i]; i++; } src[i] = 0;
                have = 1;
            } else {
                long n = sys_readfile(line + 3, src, sizeof(src) - 1);
                if (n < 0) { print("js: no such file: "); print(line + 3); print("\n"); }
                else { src[n] = 0; have = 1; }
            }
            if (have) { sys_js(src, out, sizeof(out) - 1); out[sizeof(out) - 1] = 0; print(out); }
        } else if (streq(line, "beep")) {
            sys_beep(880, 150);
            print("beep!\n");
        } else if (startswith(line, "morse ")) {
            /* print AND beep the Morse code for the text (a-z, 0-9, space) */
            static const char *M[36] = {
                ".-","-...","-.-.","-..",".","..-.","--.","....","..",".---","-.-",".-..","--",
                "-.","---",".--.","--.-",".-.","...","-","..-","...-",".--","-..-","-.--","--..",
                "-----",".----","..---","...--","....-",".....","-....","--...","---..","----."
            };
            for (const char *p = line + 6; *p; p++) {
                char c = *p;
                if (c >= 'A' && c <= 'Z') c += 32;
                const char *code = 0;
                if (c >= 'a' && c <= 'z') code = M[c - 'a'];
                else if (c >= '0' && c <= '9') code = M[26 + c - '0'];
                else if (c == ' ') { print("  "); sys_sleep(400); continue; }
                if (!code) continue;
                print(code); print(" ");
                for (const char *s = code; *s; s++) {
                    sys_beep(700, *s == '-' ? 240 : 80);   /* dash long, dot short */
                    sys_sleep(80);                          /* intra-character gap */
                }
                sys_sleep(150);                             /* inter-character gap */
            }
            print("\n");
        } else if (startswith(line, "factor ")) {
            const char *q = line + 7; while (*q == ' ') q++;
            long n = 0; int any = 0;
            while (*q >= '0' && *q <= '9' && n < 900000000000000000L) { n = n * 10 + (*q - '0'); q++; any = 1; }   /* bound so n*10+digit can't overflow long */
            if (!any || n < 2) { print("usage: factor <n>  (n >= 2)\n"); }
            else {
                printl(n); print(":");
                long m = n;
                while (m % 2 == 0) { print(" 2"); m /= 2; }
                for (long d = 3; d <= 3000000L && d <= m / d; d += 2)   /* trial division to ~sqrt, capped */
                    while (m % d == 0) { print(" "); printl(d); m /= d; }
                if (m > 1) { print(" "); printl(m); }                  /* remaining prime (or a large factor) */
                print("\n");
            }
        } else if (startswith(line, "roll ")) {
            const char *q = line + 5; while (*q == ' ') q++;
            int a = 0; while (*q >= '0' && *q <= '9' && a < 100000000) { a = a * 10 + (*q - '0'); q++; }
            int n, sides;
            if (*q == 'd' || *q == 'D') { q++; sides = 0; while (*q >= '0' && *q <= '9' && sides < 100000000) { sides = sides * 10 + (*q - '0'); q++; } n = a ? a : 1; }
            else { n = 1; sides = a; }                         /* "roll 6" = 1d6 */
            if (n < 1) n = 1; if (n > 40) n = 40;
            if (sides < 2 || sides > 1000) { print("usage: roll NdM  (e.g. roll 2d6)\n"); }
            else {
                int total = 0; char nb[12]; print(" ");
                for (int i = 0; i < n; i++) { int r = (int)(shroll() % (unsigned)sides) + 1; total += r; itoa_simple(r, nb); print(nb); print(" "); }
                if (n > 1) { print(" total "); itoa_simple(total, nb); print(nb); }
                print("\n");
            }
        } else if (startswith(line, "rev ")) {
            const char *t = line + 4;
            char r[128]; int len = 0; while (t[len] && len < 127) len++;
            for (int i = 0; i < len; i++) r[i] = t[len - 1 - i];
            r[len] = 0;
            print(r); print("\n");
        } else if (startswith(line, "seq ")) {
            const char *q = line + 4; while (*q == ' ') q++;
            int m = 0; while (*q >= '0' && *q <= '9' && m < 100000) { m = m * 10 + (*q - '0'); q++; }
            if (m < 1) print("usage: seq <n>  (prints 1..n)\n");
            else {
                if (m > 1000) m = 1000;                /* cap the output */
                char nb[12]; print(" ");
                for (int i = 1; i <= m; i++) { itoa_simple(i, nb); print(nb); print(" "); }
                print("\n");
            }
        } else if (streq(line, "todo") || startswith(line, "todo ")) {
            static char buf[2048];
            long n = sys_readfile("TODO.TXT", buf, sizeof(buf) - 1);
            if (n < 0) n = 0;
            buf[n] = 0;
            if (startswith(line, "todo add ")) {                  /* append a new item */
                const char *t = line + 9; while (*t == ' ') t++;
                if (!*t) print("usage: todo add <text>\n");
                else {
                    int p = (int)n;
                    const char *pre = "[ ] ";
                    for (int i = 0; pre[i] && p < (int)sizeof(buf) - 2; i++) buf[p++] = pre[i];
                    for (; *t && p < (int)sizeof(buf) - 2; t++) buf[p++] = *t;
                    buf[p++] = '\n'; buf[p] = 0;
                    sys_writefile("TODO.TXT", buf, (unsigned long)p);
                    print("added.\n");
                }
            } else if (startswith(line, "todo done ")) {          /* toggle item N's checkbox */
                int target = 0; const char *d = line + 10; while (*d == ' ') d++;
                while (*d >= '0' && *d <= '9') target = target * 10 + (*d++ - '0');
                int item = 0, found = 0;
                for (int i = 0; i < (int)n; ) {
                    int ls = i; while (i < (int)n && buf[i] != '\n') i++;
                    if (++item == target && i - ls >= 3 && buf[ls] == '[' && buf[ls + 2] == ']') {
                        buf[ls + 1] = (buf[ls + 1] == 'x') ? ' ' : 'x';
                        found = 1;
                    }
                    if (i < (int)n) i++;
                }
                if (found) { sys_writefile("TODO.TXT", buf, (unsigned long)n); print("toggled.\n"); }
                else print("todo: no such item\n");
            } else if (streq(line, "todo clear")) {               /* drop completed ([x]) items */
                int w = 0;
                for (int i = 0; i < (int)n; ) {
                    int ls = i; while (i < (int)n && buf[i] != '\n') i++;
                    int le = i; if (i < (int)n) i++;
                    if (!(le - ls >= 3 && buf[ls] == '[' && buf[ls + 1] == 'x' && buf[ls + 2] == ']')) {
                        for (int j = ls; j < le; j++) buf[w++] = buf[j];   /* compact in place (w <= ls) */
                        buf[w++] = '\n';
                    }
                }
                buf[w] = 0;
                sys_writefile("TODO.TXT", buf, (unsigned long)w);
                print("cleared completed items.\n");
            } else {                                              /* list */
                if (n == 0) print("  (empty; add with: todo add <text>)\n");
                else {
                    int item = 0;
                    for (int i = 0; i < (int)n; ) {
                        int ls = i; while (i < (int)n && buf[i] != '\n') i++;
                        char save = buf[i]; buf[i] = 0;
                        char num[8]; itoa_simple(++item, num);
                        print("  "); print(num); print(". "); print(buf + ls); print("\n");
                        buf[i] = save;
                        if (i < (int)n) i++;
                    }
                }
            }
        } else if (streq(line, "ifconfig") || streq(line, "netinfo")) {
            char info[128];
            if (sys_netinfo(info, sizeof(info)) > 0) print(info);
            else print("ifconfig: unavailable\n");
        } else if (streq(line, "ping")) {
            long n = sys_ping();
            if (n < 0) print("ping: no network\n");
            else {
                char c[2] = { (char)('0' + (int)n), 0 };
                print("gateway 10.0.2.2: "); print(c); print("/3 replies\n");
            }
        } else if (startswith(line, "ping ")) {
            /* ping a host by name: resolve (also shows the IP), then ICMP-echo */
            char host[64]; char *p = line + 5; while (*p == ' ') p++;
            int i = 0; while (*p && *p != ' ' && i < 63) host[i++] = *p++; host[i] = 0;
            if (host[0] == 0) { print("usage: ping <host>\n"); }
            else {
                char ip[40];
                if (sys_resolve(host, ip, sizeof(ip)) < 0) { print("ping: cannot resolve "); print(host); print("\n"); }
                else {
                    for (int k = 0; ip[k]; k++) if (ip[k] == '\n') ip[k] = 0;   /* inline the IP */
                    print("PING "); print(host); print(" ("); print(ip); print(") ...\n");
                    long n = sys_ping_host(host);
                    if (n < 0) print("ping: no route to host\n");
                    else { char c[2] = { (char)('0' + (int)n), 0 }; print(c); print("/3 echo replies\n"); }
                }
            }
        } else if (startswith(line, "resolve ")) {
            char ip[40];
            if (sys_resolve(line + 8, ip, sizeof(ip)) < 0) print("resolve: failed\n");
            else { print(line + 8); print(" -> "); print(ip); }
        } else if (streq(line, "pwd")) {
            print(cwd); print("\n");
        } else if (startswith(line, "crypt ")) {
            char *p = line + 6, fn[32]; int i = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && i < 31) fn[i++] = *p++;
            fn[i] = 0;
            while (*p == ' ') p++;
            if (fn[0] == 0 || *p == 0) print("usage: crypt <file> <pass>\n");
            else if (sys_crypt(fn, p) < 0) print("crypt: failed\n");
            else { print("crypt: "); print(fn); print(" (run again to reverse)\n"); }
        } else if (startswith(line, "base64 ")) {
            static char buf[1536];
            long n = sys_readfile(line + 7, buf, sizeof(buf));
            if (n < 0) { print("base64: no such file\n"); }
            else {
                static const char *B =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                char ln[48]; int col = 0;
                for (long i = 0; i < n; i += 3) {
                    unsigned b0 = (unsigned char)buf[i];
                    unsigned b1 = (i+1 < n) ? (unsigned char)buf[i+1] : 0;
                    unsigned b2 = (i+2 < n) ? (unsigned char)buf[i+2] : 0;
                    ln[col++] = B[b0 >> 2];
                    ln[col++] = B[((b0 & 3) << 4) | (b1 >> 4)];
                    ln[col++] = (i+1 < n) ? B[((b1 & 15) << 2) | (b2 >> 6)] : '=';
                    ln[col++] = (i+2 < n) ? B[b2 & 63] : '=';
                    if (col >= 40) { ln[col] = '\n'; ln[col+1] = 0; print(ln); col = 0; }
                }
                if (col) { ln[col] = '\n'; ln[col+1] = 0; print(ln); }
            }
        } else if (startswith(line, "sha256 ")) {
            char hex[72];
            if (sys_sha256(line + 7, hex, sizeof(hex)) < 0) print("sha256: no such file\n");
            else { print("  "); print(hex); print("\n"); }
        } else if (startswith(line, "sha512 ")) {
            char hex[136];
            if (sys_sha512(line + 7, hex, sizeof(hex)) < 0) print("sha512: no such file\n");
            else { print("  "); print(hex); print("\n"); }
        } else if (startswith(line, "crc32 ")) {       /* CRC-32 (IEEE 802.3, as in zip/gzip/png); first 16 KB, matching sha256/512 */
            static unsigned char cbuf[16384];
            long n = sys_readfile(line + 6, cbuf, sizeof(cbuf));
            if (n < 0) { print("crc32: no such file: "); print(line + 6); print("\n"); }
            else {
                unsigned c = 0xFFFFFFFFu;
                for (long i = 0; i < n; i++) {
                    c ^= cbuf[i];
                    for (int k = 0; k < 8; k++) c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0u);
                }
                c ^= 0xFFFFFFFFu;
                char hex[9]; const char *hd = "0123456789abcdef";
                for (int i = 0; i < 8; i++) hex[i] = hd[(c >> ((7 - i) * 4)) & 0xF];
                hex[8] = 0;
                print("  "); print(hex); print("\n");
            }
        } else if (startswith(line, "grep ")) {
            char *p = line + 5, pat[40]; int i = 0, ci = 0, nn = 0, cc = 0, vv = 0;
            while (*p == ' ') p++;
            while (p[0] == '-' && p[1] && p[1] != ' ') {   /* flags -i (case-insens), -n (line#s), -c (count), -v (invert); combinable as -in */
                if (p[1] == '-' && (p[2] == ' ' || p[2] == 0)) { p += 2; while (*p == ' ') p++; break; }  /* "--": end of flags (pattern may then start with '-') */
                int t, valid = 1;
                for (t = 1; p[t] && p[t] != ' '; t++) if (p[t] != 'i' && p[t] != 'n' && p[t] != 'c' && p[t] != 'v') valid = 0;
                if (!valid) break;                          /* not a flag token (e.g. a pattern starting with '-') */
                for (t = 1; p[t] && p[t] != ' '; t++) { if (p[t] == 'i') ci = 1; else if (p[t] == 'n') nn = 1; else if (p[t] == 'c') cc = 1; else vv = 1; }
                p += t; while (*p == ' ') p++;
            }
            while (*p && *p != ' ' && i < 39) pat[i++] = *p++;
            pat[i] = 0;
            while (*p == ' ') p++;
            if (pat[0] == 0 || *p == 0) { print("usage: grep [-incv] <pattern> <file>...\n"); }
            else {
                static char buf[2048];
                const char *cq = p; int fcount = 0;               /* count files: prefix names only if >1 */
                while (*cq) { while (*cq == ' ') cq++; if (!*cq) break; fcount++; while (*cq && *cq != ' ') cq++; }
                int hits = 0;
                while (*p) {                                       /* grep each space-separated file */
                    while (*p == ' ') p++;
                    if (!*p) break;
                    char name[64]; int j = 0;
                    while (*p && *p != ' ' && j < 63) name[j++] = *p++;
                    name[j] = 0;
                    long n = sys_readfile(name, buf, sizeof(buf) - 1);   /* -1 so buf[n]=0 can't write past the array */
                    if (n < 0) { print("grep: no such file: "); print(name); print("\n"); continue; }
                    buf[n] = 0;
                    int ls = 0, lno = 0;
                    for (long k = 0; k <= n; k++) {
                        if (k == n || buf[k] == '\n') {
                            lno++;
                            int found = 0;
                            for (long a = ls; a < k && !found; a++) {
                                int b = 0;
                                while (a + b < k && pat[b] && (ci ? lc(buf[a+b]) == lc(pat[b]) : buf[a+b] == pat[b])) b++;
                                if (!pat[b]) found = 1;
                            }
                            int hit = vv ? !found : found;   /* -v inverts: act on non-matching lines */
                            if (hit) {
                                hits++;
                                if (!cc) {                  /* -c: count only, don't print the line */
                                    char save = buf[k]; buf[k] = 0;
                                    if (fcount > 1) print(name);
                                    if (fcount > 1 && nn) print(":");
                                    if (nn) { char ln_[12]; itoa_simple(lno, ln_); print(ln_); }
                                    if (fcount > 1 || nn) print(": "); else print("  ");
                                    print(buf + ls); print("\n");
                                    buf[k] = save;
                                }
                            }
                            ls = (int)k + 1;
                        }
                    }
                }
                if (cc) { char cb[12]; itoa_simple(hits, cb); print("  "); print(cb); print("\n"); }
                else if (!hits) print("  (no matches)\n");
            }
        } else if (startswith(line, "wc ")) {
            static char buf[2048];
            const char *p = line + 3; char num[12];
            int tl = 0, tw = 0, nfiles = 0; long tb = 0;
            int wl = 0, ww = 0, wcb = 0;                   /* -l/-w/-c: which counts to show (no flag = all three) */
            while (*p == ' ') p++;
            while (p[0] == '-' && p[1] && p[1] != ' ') {
                int t, valid = 1;
                for (t = 1; p[t] && p[t] != ' '; t++) if (p[t] != 'l' && p[t] != 'w' && p[t] != 'c') valid = 0;
                if (!valid) break;                          /* not a flag token (e.g. a filename) */
                for (t = 1; p[t] && p[t] != ' '; t++) { if (p[t] == 'l') wl = 1; else if (p[t] == 'w') ww = 1; else wcb = 1; }
                p += t; while (*p == ' ') p++;
            }
            if (!wl && !ww && !wcb) { wl = ww = wcb = 1; }
            while (*p) {                                   /* count each space-separated file */
                while (*p == ' ') p++;
                if (!*p) break;
                char name[64]; int j = 0;
                while (*p && *p != ' ' && j < 63) name[j++] = *p++;
                name[j] = 0;
                long n = sys_readfile(name, buf, sizeof(buf));
                if (n < 0) { print("wc: no such file: "); print(name); print("\n"); continue; }
                int lines = 0, words = 0, inword = 0;
                for (long i = 0; i < n; i++) {
                    char c = buf[i];
                    if (c == '\n') lines++;
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') inword = 0;
                    else if (!inword) { inword = 1; words++; }
                }
                if (wl) { print("  lines "); itoa_simple(lines, num); print(num); }
                if (ww) { print("  words "); itoa_simple(words, num); print(num); }
                if (wcb) { print("  bytes "); itoa_simple((int)n, num); print(num); }
                print("  "); print(name); print("\n");
                tl += lines; tw += words; tb += n; nfiles++;
            }
            if (nfiles > 1) {
                if (wl) { print("  lines "); itoa_simple(tl, num); print(num); }
                if (ww) { print("  words "); itoa_simple(tw, num); print(num); }
                if (wcb) { print("  bytes "); itoa_simple((int)tb, num); print(num); }
                print("  total\n");
            }
            if (nfiles == 0) print("usage: wc [-lwc] <file>...\n");
        } else if (startswith(line, "hexdump ")) {
            static char buf[512];
            long n = sys_readfile(line + 8, buf, sizeof(buf));
            if (n < 0) { print("hexdump: no such file\n"); }
            else {
                const char *H = "0123456789abcdef";
                for (long off = 0; off < n; off += 8) {
                    char h[48]; int p = 0;
                    for (int s = 12; s >= 0; s -= 4) h[p++] = H[(off >> s) & 0xF];
                    h[p++] = ' '; h[p++] = ' ';
                    for (int i = 0; i < 8; i++) {
                        if (off + i < n) { unsigned char c = (unsigned char)buf[off+i];
                            h[p++] = H[c >> 4]; h[p++] = H[c & 0xF]; }
                        else { h[p++] = ' '; h[p++] = ' '; }
                        h[p++] = ' ';
                    }
                    h[p++] = ' ';
                    for (int i = 0; i < 8 && off + i < n; i++) {
                        unsigned char c = (unsigned char)buf[off+i];
                        h[p++] = (c >= 32 && c < 127) ? (char)c : '.';
                    }
                    h[p++] = '\n'; h[p] = 0;
                    print(h);
                }
            }
        } else if (startswith(line, "find ")) {
            static char fb[2048];
            long n = sys_find(line + 5, fb, sizeof(fb));
            if (n > 0) { fb[n] = 0; print(fb); } else print("(no matches)\n");
        } else if (streq(line, "tree")) {
            static char tb[2048];
            long n = sys_tree(tb, sizeof(tb));
            if (n <= 0) print("(empty)\n"); else { tb[n] = 0; print(tb); }
        } else if (startswith(line, "mkdir ")) {
            if (sys_mkdir(line + 6) < 0) print("mkdir: failed (exists?)\n");
            else { print("created "); print(line + 6); print("/\n"); }
        } else if (startswith(line, "cd ")) {
            char *path = line + 3;
            if (sys_chdir(path) < 0) print("cd: no such directory\n");
            else if (streq(path, "/")) { cwd[0] = '/'; cwd[1] = 0; }
            else if (streq(path, "..")) {
                int n = (int)ustrlen(cwd);
                while (n > 1 && cwd[n-1] != '/') n--;
                if (n <= 1) { cwd[0] = '/'; cwd[1] = 0; } else cwd[n-1] = 0;
            } else if (path[0] == '/') {
                int i = 0; while (path[i] && i < 126) { cwd[i] = path[i]; i++; } cwd[i] = 0;
            } else {
                int n = (int)ustrlen(cwd);
                if (n > 0 && cwd[n-1] != '/' && n < 126) cwd[n++] = '/';
                int i = 0; while (path[i] && n < 126) cwd[n++] = path[i++];
                cwd[n] = 0;
            }
        } else if (streq(line, "ps")) {
            char buf[512];
            long n = sys_ps(buf, sizeof(buf));
            if (n > 0) { buf[n] = 0; print("  PID STATE  NAME\n"); print(buf); }
            else print("ps: none\n");
        } else if (streq(line, "history")) {
            char buf[640];
            long n = sys_history(buf, sizeof(buf));
            if (n > 0) { buf[n] = 0; print(buf); } else print("  (no history yet)\n");
        } else if (streq(line, "df")) {
            char b[96]; long n = sys_df(b, sizeof(b));
            if (n > 0) { b[n] = 0; print(b); } else print("df: no disk\n");
        } else if (streq(line, "mem")) {
            char buf[128];
            sys_sysinfo(buf, sizeof(buf));
            print(buf);
        } else if (streq(line, "clear")) {
            sys_clear();
        } else if (streq(line, "reboot")) {
            print("rebooting...\n");
            sys_reboot();
        } else if (streq(line, "cal")) {
            cmd_cal();
        } else if (startswith(line, "cal ")) {         /* cal <month> <year>, or cal -y <year> for the whole year */
            const char *p = line + 4; while (*p == ' ') p++;
            if (p[0] == '-' && p[1] == 'y') {
                p += 2; while (*p == ' ') p++;
                int y = 0; while (*p >= '0' && *p <= '9') { if (y < 1000000) y = y*10 + (*p - '0'); p++; }
                if (y < 1 || y > 9999) print("usage: cal -y <year>\n");
                else for (int mm = 1; mm <= 12; mm++) cmd_cal_ym(y, mm, 0);    /* the full year, month by month */
            } else if (p[0] == '-' && p[1] == '3') {      /* cal -3 : previous, current, next month */
                char t[24]; sys_time(t, sizeof(t));
                int y = (t[0]-'0')*1000 + (t[1]-'0')*100 + (t[2]-'0')*10 + (t[3]-'0');
                int m = (t[5]-'0')*10 + (t[6]-'0'), today = (t[8]-'0')*10 + (t[9]-'0');
                int pm = m-1, py = y; if (pm < 1) { pm = 12; py--; }
                int nm = m+1, ny = y; if (nm > 12) { nm = 1; ny++; }
                cmd_cal_ym(py, pm, 0); cmd_cal_ym(y, m, today); cmd_cal_ym(ny, nm, 0);
            } else {
                int m = 0, y = 0;
                while (*p >= '0' && *p <= '9') { if (m < 1000000) m = m*10 + (*p - '0'); p++; }
                while (*p == ' ') p++;
                while (*p >= '0' && *p <= '9') { if (y < 1000000) y = y*10 + (*p - '0'); p++; }
                if (m < 1 || m > 12 || y < 1 || y > 9999) print("usage: cal <month 1-12> <year>  |  cal -y <year>  |  cal -3\n");
                else cmd_cal_ym(y, m, 0);
            }
        } else if (startswith(line, "weekday ")) {        /* weekday YYYYMMDD -> the day name (Sakamoto) */
            const char *p = line + 8; while (*p == ' ') p++;
            int dig[8], k = 0;
            while (*p >= '0' && *p <= '9' && k < 8) dig[k++] = *p++ - '0';
            if (k != 8) print("usage: weekday YYYYMMDD   (e.g. weekday 20000101)\n");
            else {
                int y = dig[0]*1000 + dig[1]*100 + dig[2]*10 + dig[3];
                int m = dig[4]*10 + dig[5], d = dig[6]*10 + dig[7];
                if (m < 1 || m > 12 || d < 1 || d > 31) print("weekday: bad date\n");
                else {
                    static const int tt[] = { 0,3,2,5,0,3,5,1,4,6,2,4 };
                    int yy = y - (m < 3);
                    int w = (yy + yy/4 - yy/100 + yy/400 + tt[m-1] + d) % 7;
                    static const char *names[] = { "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday" };
                    print("  "); print(names[w]); print("\n");
                }
            }
        } else if (startswith(line, "dur ")) {            /* dur SECONDS -> Dd Hh Mm Ss */
            const char *p = line + 4; while (*p == ' ') p++;
            long s = 0; int any = 0;
            while (*p >= '0' && *p <= '9') { if (s < 100000000000L) s = s * 10 + (*p - '0'); p++; any = 1; }
            if (!any) print("usage: dur <seconds>   (e.g. dur 90061)\n");
            else {
                long d = s/86400, h = (s%86400)/3600, m = (s%3600)/60, sec = s%60;
                print("  ");
                if (d) { printl(d); print("d "); }
                if (d || h) { printl(h); print("h "); }
                if (d || h || m) { printl(m); print("m "); }
                printl(sec); print("s\n");
            }
        } else if (streq(line, "date")) {
            char buf[24];
            sys_time(buf, sizeof(buf));
            print(buf);
        } else if (streq(line, "ver")) {
            print("OS-DEV 0.1 (x86_64, built from scratch)\n");
        } else if (streq(line, "pid")) {
            char num[12];
            itoa_simple(sys_getpid(), num);
            print("pid = ");
            print(num);
            print("\n");
        } else if (startswith(line, "echo ")) {
            print(line + 5);
            print("\n");
        } else if (startswith(line, "cowsay ")) {         /* the classic: a cow speaks your message */
            const char *msg = line + 7; int len = 0; while (msg[len]) len++;
            print(" "); for (int i = 0; i < len + 2; i++) print("_"); print("\n");
            print("< "); print(msg); print(" >\n");
            print(" "); for (int i = 0; i < len + 2; i++) print("-"); print("\n");
            print("        \\   ^__^\n");
            print("         \\  (oo)\\_______\n");
            print("            (__)\\       )\\/\\\n");
            print("                ||----w |\n");
            print("                ||     ||\n");
        } else if (streq(line, "fortune")) {              /* a random programming aphorism (pairs with cowsay) */
            static const char *fortunes[] = {
                "The best way to predict the future is to invent it.  -Alan Kay",
                "Premature optimization is the root of all evil.  -Knuth",
                "There are 2 hard problems in CS: cache invalidation, naming things, and off-by-one errors.",
                "Talk is cheap. Show me the code.  -Linus Torvalds",
                "Simplicity is the ultimate sophistication.  -da Vinci",
                "Weeks of coding can save hours of planning.",
                "It works on my machine.",
                "First, solve the problem. Then, write the code.  -John Johnson",
                "Any sufficiently advanced technology is indistinguishable from magic.  -Clarke",
                "A user interface is like a joke: if you have to explain it, it is not that good.",
            };
            int fn = (int)(sizeof(fortunes) / sizeof(fortunes[0]));
            print(fortunes[shroll() % (unsigned)fn]); print("\n");
        } else if (streq(line, "genpass") || startswith(line, "genpass ")) {   /* random password (CLI companion to passgen.htm) */
            int n = 16;
            if (line[7] == ' ') { const char *p = line + 8; while (*p == ' ') p++; if (*p >= '0' && *p <= '9') { n = 0; while (*p >= '0' && *p <= '9' && n < 1000) n = n * 10 + (*p++ - '0'); } }
            if (n < 1) n = 16;
            if (n > 64) n = 64;
            static const char gcs[] = "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789!@#$%&*";   /* no ambiguous l/I/O/0/1 */
            int gl = (int)sizeof(gcs) - 1;
            char gp[66]; for (int i = 0; i < n; i++) gp[i] = gcs[shroll() % (unsigned)gl]; gp[n] = 0;
            print("  "); print(gp); print("\n");
        } else if (streq(line, "uuidgen")) {              /* random RFC-4122 v4 UUID (CLI companion to uuid.htm) */
            static const char uhx[] = "0123456789abcdef";
            char u[37]; int j = 0;
            for (int i = 0; i < 36; i++) {
                if (i==8 || i==13 || i==18 || i==23) u[j++] = '-';
                else if (i==14) u[j++] = '4';                       /* version 4 */
                else if (i==19) u[j++] = uhx[8 + (shroll() % 4)];   /* variant 8/9/a/b */
                else u[j++] = uhx[shroll() % 16];
            }
            u[j] = 0;
            print("  "); print(u); print("\n");
        } else if (streq(line, "ascii")) {                /* the printable ASCII table (CLI companion to ascii.htm) */
            for (int c = 32; c <= 126; c++) {
                char nb[8]; itoa_simple(c, nb);
                if (c < 100) print(" ");
                print(nb); print(" ");
                char cb[2] = { (char)c, 0 }; print(cb);
                print("  ");
                if ((c - 31) % 6 == 0) print("\n");      /* 6/row fits the 44-col shell grid */
            }
            print("\n");
        } else if (startswith(line, "roman ")) {          /* roman N -> Roman numerals (1-3999) */
            const char *p = line + 6; while (*p == ' ') p++;
            int n = 0; while (*p >= '0' && *p <= '9' && n < 100000) n = n * 10 + (*p++ - '0');
            if (n < 1 || n > 3999) print("usage: roman <1-3999>\n");
            else {
                static const int vals[] = { 1000,900,500,400,100,90,50,40,10,9,5,4,1 };
                static const char *syms[] = { "M","CM","D","CD","C","XC","L","XL","X","IX","V","IV","I" };
                print("  ");
                for (int i = 0; i < 13; i++) while (n >= vals[i]) { print(syms[i]); n -= vals[i]; }
                print("\n");
            }
        } else if (startswith(line, "base ")) {           /* base N -> binary / octal / hex (CLI companion to base.htm) */
            const char *p = line + 5; while (*p == ' ') p++;
            unsigned long n = 0; int any = 0;
            while (*p >= '0' && *p <= '9') { if (n < 100000000000000000UL) n = n * 10 + (unsigned)(*p - '0'); p++; any = 1; }
            if (!any) print("usage: base <decimal>   (e.g. base 255)\n");
            else {
                print("  bin "); print_base(n, 2);
                print("  oct "); print_base(n, 8);
                print("  hex "); print_base(n, 16);
                print("\n");
            }
        } else if (startswith(line, "gcd ")) {            /* gcd A B -> gcd + lcm (Euclid) */
            const char *p = line + 4; while (*p == ' ') p++;
            long a = 0; while (*p >= '0' && *p <= '9' && a < 100000000L) a = a * 10 + (*p++ - '0');   /* cap ~1e9 so a*b can't overflow long */
            while (*p == ' ') p++;
            long b = 0; while (*p >= '0' && *p <= '9' && b < 100000000L) b = b * 10 + (*p++ - '0');
            if (a < 1 || b < 1) print("usage: gcd <a> <b>   (e.g. gcd 12 18)\n");
            else {
                long x = a, y = b; while (y) { long t = x % y; x = y; y = t; }   /* Euclid's algorithm */
                print("  gcd="); printl(x);
                print("  lcm="); printl(a / x * b);                              /* a/g*b avoids a*b overflow */
                print("\n");
            }
        } else if (startswith(line, "primes ")) {         /* primes N -> all primes up to N */
            const char *p = line + 7; while (*p == ' ') p++;
            int n = 0; while (*p >= '0' && *p <= '9' && n < 100000) n = n * 10 + (*p++ - '0');
            if (n < 2 || n > 10000) print("usage: primes <2-10000>\n");
            else {
                char nb[12]; print("  ");
                for (int k = 2; k <= n; k++) {
                    int prime = 1;
                    for (int d = 2; d * d <= k; d++) if (k % d == 0) { prime = 0; break; }
                    if (prime) { itoa_simple(k, nb); print(nb); print(" "); }
                }
                print("\n");
            }
        } else if (startswith(line, "rot13 ")) {          /* rot13 TEXT -> ROT13 (CLI companion to rot13.htm) */
            const char *p = line + 6;
            char out[256]; int i = 0;
            while (*p && i < 255) {
                char c = *p++;
                if (c >= 'A' && c <= 'Z') out[i++] = (char)((c - 'A' + 13) % 26 + 'A');
                else if (c >= 'a' && c <= 'z') out[i++] = (char)((c - 'a' + 13) % 26 + 'a');
                else out[i++] = c;
            }
            out[i] = 0;
            print("  "); print(out); print("\n");
        } else if (startswith(line, "fizzbuzz ")) {       /* the classic FizzBuzz up to N */
            const char *p = line + 9; while (*p == ' ') p++;
            int n = 0; while (*p >= '0' && *p <= '9' && n < 10000) n = n * 10 + (*p++ - '0');
            if (n < 1 || n > 1000) print("usage: fizzbuzz <1-1000>\n");
            else {
                char nb[12]; print("  ");
                for (int k = 1; k <= n; k++) {
                    if (k % 15 == 0) print("FizzBuzz");
                    else if (k % 3 == 0) print("Fizz");
                    else if (k % 5 == 0) print("Buzz");
                    else { itoa_simple(k, nb); print(nb); }
                    print(" ");
                }
                print("\n");
            }
        } else if (startswith(line, "dec ")) {            /* dec 0x.. / 0b.. / 0o.. / decimal -> decimal value */
            const char *p = line + 4; while (*p == ' ') p++;
            int base = 10;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
            else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) { base = 2; p += 2; }
            else if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) { base = 8; p += 2; }
            unsigned long v = 0; int any = 0;
            while (*p) {
                char c = *p; int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else break;
                if (d >= base) break;
                if (v > 1000000000000000000UL) break;
                v = v * (unsigned)base + (unsigned)d; any = 1; p++;
            }
            if (!any) print("usage: dec <0x.. | 0b.. | 0o.. | decimal>\n");
            else { print("  "); print_base(v, 10); print("\n"); }   /* unsigned: correct even when v > LONG_MAX */
        } else if (startswith(line, "fib ")) {            /* fib N -> the Nth Fibonacci number */
            const char *p = line + 4; while (*p == ' ') p++;
            int n = 0; while (*p >= '0' && *p <= '9' && n < 1000) n = n * 10 + (*p++ - '0');
            if (n > 90) print("fib: max 90 (fib(91)+ overflows 64-bit)\n");
            else {
                long a = 0, b = 1;
                for (int i = 0; i < n; i++) { long t = a + b; a = b; b = t; }
                print("  "); printl(a); print("\n");
            }
        } else if (startswith(line, "stats ")) {          /* stats N1 N2 ... -> count/min/max/sum */
            const char *p = line + 6;
            long mn = 0, mx = 0, sum = 0; int cnt = 0;
            while (*p) {
                while (*p == ' ') p++;
                if (*p < '0' || *p > '9') break;
                long v = 0; while (*p >= '0' && *p <= '9') { if (v < 1000000000000L) v = v * 10 + (*p - '0'); p++; }
                if (cnt == 0) { mn = mx = v; } else { if (v < mn) mn = v; if (v > mx) mx = v; }
                sum += v; cnt++;
            }
            if (cnt == 0) print("usage: stats <n1> <n2> ...\n");
            else {
                print("  count="); printl(cnt);
                print("  min="); printl(mn);
                print("  max="); printl(mx);
                print("  sum="); printl(sum);
                print("\n");
            }
        } else if (startswith(line, "size ")) {           /* size BYTES -> GB/MB/KB/B (1024-based) */
            const char *p = line + 5; while (*p == ' ') p++;
            long b = 0; int any = 0;
            while (*p >= '0' && *p <= '9') { if (b < 100000000000000L) b = b * 10 + (*p - '0'); p++; any = 1; }
            if (!any) print("usage: size <bytes>   (e.g. size 1536000)\n");
            else {
                long gb = b/1073741824, mb = (b%1073741824)/1048576, kb = (b%1048576)/1024, by = b%1024;
                print("  ");
                if (gb) { printl(gb); print(" GB "); }
                if (gb || mb) { printl(mb); print(" MB "); }
                if (gb || mb || kb) { printl(kb); print(" KB "); }
                printl(by); print(" B\n");
            }
        } else if (startswith(line, "unmorse ")) {        /* unmorse CODE -> text (reverse of morse; / = word space) */
            const char *p = line + 8;
            static const char *codes[] = { ".-","-...","-.-.","-..",".","..-.","--.","....","..",".---","-.-",".-..","--","-.","---",".--.","--.-",".-.","...","-","..-","...-",".--","-..-","-.--","--..","-----",".----","..---","...--","....-",".....","-....","--...","---..","----." };
            static const char *letters = "abcdefghijklmnopqrstuvwxyz0123456789";
            char out[256]; int oi = 0;
            while (*p && oi < 255) {
                if (*p == '/') { out[oi++] = ' '; p++; continue; }
                if (*p == ' ') { p++; continue; }
                char tok[12]; int ti = 0;
                while ((*p == '.' || *p == '-') && ti < 11) tok[ti++] = *p++;
                tok[ti] = 0;
                if (ti == 0) { p++; continue; }                       /* skip any other char */
                for (int i = 0; i < 36; i++) {
                    const char *c = codes[i], *t = tok;
                    while (*c && *t && *c == *t) { c++; t++; }
                    if (!*c && !*t) { out[oi++] = letters[i]; break; }
                }
            }
            out[oi] = 0;
            print("  "); print(out); print("\n");
        } else if (startswith(line, "unhex ")) {          /* unhex HEXSTRING -> ASCII text */
            const char *p = line + 6; while (*p == ' ') p++;
            char out[256]; int oi = 0;
            while (*p && oi < 255) {
                int hi = (*p>='0'&&*p<='9') ? *p-'0' : (*p>='a'&&*p<='f') ? *p-'a'+10 : (*p>='A'&&*p<='F') ? *p-'A'+10 : -1;
                if (hi < 0) { p++; continue; }                /* skip spaces / non-hex */
                p++;
                int lo = (*p>='0'&&*p<='9') ? *p-'0' : (*p>='a'&&*p<='f') ? *p-'a'+10 : (*p>='A'&&*p<='F') ? *p-'A'+10 : -1;
                if (lo < 0) break;                            /* odd trailing nibble: stop */
                p++;
                out[oi++] = (char)(hi * 16 + lo);
            }
            out[oi] = 0;
            print("  "); print(out); print("\n");
        } else if (startswith(line, "unbase64 ")) {       /* unbase64 B64 -> decoded text */
            const char *p = line + 9; while (*p == ' ') p++;
            char out[256]; int oi = 0, acc = 0, bits = 0;
            while (*p && oi < 255) {
                char c = *p++; int v;
                if (c >= 'A' && c <= 'Z') v = c - 'A';
                else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
                else if (c >= '0' && c <= '9') v = c - '0' + 52;
                else if (c == '+') v = 62;
                else if (c == '/') v = 63;
                else if (c == '=') break;                 /* padding: done */
                else continue;                            /* skip whitespace/other */
                acc = (acc << 6) | v; bits += 6;
                if (bits >= 8) { bits -= 8; out[oi++] = (char)((acc >> bits) & 0xFF); }
            }
            out[oi] = 0;
            print("  "); print(out); print("\n");
        } else if (startswith(line, "cmp ")) {            /* cmp F1 F2 -> identical, or the first differing line */
            static char b1[2048], b2[2048];
            const char *p = line + 4; while (*p == ' ') p++;
            char f1[64]; int j = 0; while (*p && *p != ' ' && j < 63) f1[j++] = *p++; f1[j] = 0;
            while (*p == ' ') p++;
            char f2[64]; j = 0; while (*p && *p != ' ' && j < 63) f2[j++] = *p++; f2[j] = 0;
            if (!f1[0] || !f2[0]) { print("usage: cmp <file1> <file2>\n"); }
            else {
                long n1 = sys_readfile(f1, b1, sizeof(b1));
                long n2 = sys_readfile(f2, b2, sizeof(b2));
                if (n1 < 0)      { print("cmp: no such file: "); print(f1); print("\n"); }
                else if (n2 < 0) { print("cmp: no such file: "); print(f2); print("\n"); }
                else {
                    long i1 = 0, i2 = 0; int lineno = 1, differ = 0;
                    while (i1 < n1 || i2 < n2) {
                        long s1 = i1; while (i1 < n1 && b1[i1] != '\n') i1++; long e1 = i1; if (i1 < n1) i1++;
                        long s2 = i2; while (i2 < n2 && b2[i2] != '\n') i2++; long e2 = i2; if (i2 < n2) i2++;
                        long l1 = e1 - s1, l2 = e2 - s2; int same = (l1 == l2);
                        for (long k = 0; same && k < l1; k++) if (b1[s1 + k] != b2[s2 + k]) same = 0;
                        if (!same) {
                            char t[256]; long k;
                            print("  line "); printl(lineno); print(" differs:\n");
                            print("  < "); for (k = 0; k < l1 && k < 255; k++) t[k] = b1[s1 + k]; t[k] = 0; print(t); print("\n");
                            print("  > "); for (k = 0; k < l2 && k < 255; k++) t[k] = b2[s2 + k]; t[k] = 0; print(t); print("\n");
                            differ = 1; break;
                        }
                        lineno++;
                    }
                    if (!differ) print("  files are identical\n");
                }
            }
        } else if (startswith(line, "strings ")) {        /* strings FILE -> runs of >=4 printable chars */
            static char buf[2048];
            const char *p = line + 8; while (*p == ' ') p++;
            char name[64]; int j = 0; while (*p && *p != ' ' && j < 63) name[j++] = *p++; name[j] = 0;
            if (!name[0]) { print("usage: strings <file>\n"); }
            else {
                long n = sys_readfile(name, buf, sizeof(buf));
                if (n < 0) { print("strings: no such file: "); print(name); print("\n"); }
                else {
                    char run[80]; int rl = 0;
                    for (long i = 0; i < n; i++) {
                        char c = buf[i];
                        if (c >= 32 && c < 127) { if (rl < 79) run[rl++] = c; }   /* printable ASCII */
                        else { if (rl >= 4) { run[rl] = 0; print("  "); print(run); print("\n"); } rl = 0; }
                    }
                    if (rl >= 4) { run[rl] = 0; print("  "); print(run); print("\n"); }   /* trailing run */
                }
            }
        } else if (startswith(line, "get ")) {
            char host[64], path[160]; int i = 0; char *p = line + 4;
            while (*p == ' ') p++;
            int secure = 0;                            /* https:// -> fetch over TLS */
            if (startswith(p, "https://")) { secure = 1; p += 8; }
            else if (startswith(p, "http://")) { p += 7; }
            while (*p && *p != ' ' && *p != '/' && i < 63) host[i++] = *p++;
            host[i] = 0;
            if (*p == '/' || *p == ' ') {              /* optional path */
                if (*p == ' ') p++;
                int j = 0; while (*p && j < 159) path[j++] = *p++; path[j] = 0;
            } else { path[0] = '/'; path[1] = 0; }
            if (host[0] == 0) { print("usage: get [http(s)://]<host>[/path]\n"); }
            else {
                static char resp[8192];
                print(secure ? "fetching https://" : "fetching http://"); print(host); print(path); print(" ...\n");
                long n = secure ? sys_https(host, path, resp, sizeof(resp) - 1)
                                : sys_http(host, path, resp, sizeof(resp) - 1);
                if (n < 0) print("get: failed (no net/DNS/connect)\n");
                else {
                    resp[n] = 0;
                    char num[12]; itoa_simple((int)n, num);
                    print("--- "); print(num); print(" bytes ---\n");
                    print(resp); print("\n");
                }
            }
        } else if (startswith(line, "headers ")) {
            /* curl -I style: show just the HTTP response headers — status line,
             * Content-Type, redirects (Location:) — that 'browse' hides. Reuses
             * the same fetch; a 2 KB buffer suffices since headers lead the body. */
            char host[64], path[160]; int i = 0; char *p = line + 8;
            while (*p == ' ') p++;
            int secure = 0;
            if (startswith(p, "https://")) { secure = 1; p += 8; }
            else if (startswith(p, "http://")) { p += 7; }
            while (*p && *p != ' ' && *p != '/' && i < 63) host[i++] = *p++; host[i] = 0;
            if (*p == '/') { int j = 0; while (*p && *p != ' ' && j < 159) path[j++] = *p++; path[j] = 0; }
            else { path[0] = '/'; path[1] = 0; }
            if (host[0] == 0) { print("usage: headers [http(s)://]<host>[/path]\n"); }
            else {
                static char resp[2048];
                print(secure ? "headers https://" : "headers http://"); print(host); print(path); print(" ...\n");
                long n = secure ? sys_https(host, path, resp, sizeof(resp) - 1)
                                : sys_http(host, path, resp, sizeof(resp) - 1);
                if (n < 0) print("headers: failed (no net/DNS/connect)\n");
                else {
                    int end = (int)n;                        /* print up to the blank line */
                    for (int t = 0; t + 3 < (int)n; t++)
                        if (resp[t]=='\r'&&resp[t+1]=='\n'&&resp[t+2]=='\r'&&resp[t+3]=='\n') { end = t + 2; break; }
                    resp[end] = 0;
                    print(resp); print("\n");
                }
            }
        } else if (startswith(line, "wget ")) {
            /* download a URL's body to a file: wget <host>[/path] <outfile> */
            char host[64], path[160], out[32];
            char *p = line + 5; while (*p == ' ') p++;
            int secure = 0;                            /* https:// -> download over TLS */
            if (startswith(p, "https://")) { secure = 1; p += 8; }
            else if (startswith(p, "http://")) { p += 7; }
            int i = 0; while (*p && *p != ' ' && *p != '/' && i < 63) host[i++] = *p++; host[i] = 0;
            if (*p == '/') { int j = 0; while (*p && *p != ' ' && j < 159) path[j++] = *p++; path[j] = 0; }
            else { path[0] = '/'; path[1] = 0; }
            while (*p == ' ') p++;
            int k = 0; while (*p && *p != ' ' && k < 31) out[k++] = *p++; out[k] = 0;
            if (host[0] == 0 || out[0] == 0) { print("usage: wget [http(s)://]<host>[/path] <outfile>\n"); }
            else {
                static char wbuf[16384];
                print(secure ? "downloading https://" : "downloading http://"); print(host); print(path); print(" ...\n");
                long n = secure ? sys_https(host, path, wbuf, sizeof(wbuf))
                                : sys_http(host, path, wbuf, sizeof(wbuf));
                if (n < 0) { print("wget: failed (no net/DNS/connect)\n"); }
                else {
                    int bo = 0;                              /* skip HTTP headers -> body */
                    for (int t = 0; t + 3 < (int)n; t++)
                        if (wbuf[t]=='\r'&&wbuf[t+1]=='\n'&&wbuf[t+2]=='\r'&&wbuf[t+3]=='\n') { bo = t + 4; break; }
                    long bl = n - bo;
                    if (sys_writefile(out, wbuf + bo, (unsigned long)bl) < 0) print("wget: write failed\n");
                    else {
                        char num[12]; itoa_simple((int)bl, num);
                        print("saved "); print(num); print(" bytes to "); print(out); print("\n");
                    }
                }
            }
        } else if (startswith(line, "browse ")) {
            sys_browse(line + 7);
            print("opening browser: "); print(line + 7); print("\n");
        } else if (streq(line, "apps")) {
            char b[256];
            if (sys_apps(b, sizeof(b)) > 0) {
                print("apps: "); print(b); print("\n");
                print("(launch: run <name>, or the F9 Apps menu)\n");
            } else print("apps: (none)\n");
        } else if (startswith(line, "run ")) {
            if (sys_spawn(line + 4) < 0) print("run: no such program. type 'apps' for the list (or run a disk .elf)\n");
            else { print("launched "); print(line + 4); print("\n"); }
        } else if (startswith(line, "cp ") || startswith(line, "mv ")) {
            int move = (line[0] == 'm');
            char src[64]; int i = 0; char *p = line + 3;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && i < 63) src[i++] = *p++;
            src[i] = 0;
            while (*p == ' ') p++;
            if (src[0] == 0 || *p == 0) { print("usage: "); print(move?"mv":"cp"); print(" <src> <dst>\n"); }
            else {
                static char buf[4096];
                long n = sys_readfile(src, buf, sizeof(buf));
                if (n < 0) print("no such file\n");
                else if (sys_writefile(p, buf, (unsigned long)n) < 0) print("write failed\n");
                else {
                    if (move) sys_delete(src);
                    print(move ? "moved " : "copied "); print(src);
                    print(" -> "); print(p); print("\n");
                }
            }
        } else if (startswith(line, "rm ")) {
            if (sys_delete(line + 3) < 0) print("rm: no such file\n");
            else { print("removed "); print(line + 3); print("\n"); }
        } else if (startswith(line, "edit ")) {
            char fname[32]; int i = 0;
            for (char *q = line + 5; *q && i < 31; q++) fname[i++] = *q;
            fname[i] = 0;
            print("-- editor: type lines, '.' on its own line to save --\n");
            char doc[1024], l[128]; int dl = 0;
            for (;;) {
                print("> ");
                int n = readline(l, sizeof(l));
                if (n == 1 && l[0] == '.') break;
                for (int k = 0; l[k] && dl < 1023; k++) doc[dl++] = l[k];
                if (dl < 1023) doc[dl++] = '\n';
            }
            if (sys_writefile(fname, doc, dl) < 0) print("edit: write failed\n");
            else { print("saved "); print(fname); print("\n"); }
        } else if (startswith(line, "write ")) {
            char *p = line + 6, fname[32]; int i = 0;
            while (*p && *p != ' ' && i < 31) fname[i++] = *p++;
            fname[i] = 0;
            if (*p == ' ') p++;
            long n = sys_writefile(fname, p, ustrlen(p));
            if (n < 0) print("write: failed\n");
            else { print("wrote "); print(fname); print("\n"); }
        } else if (streq(line, "exit")) {
            print("bye!\n");
            return 0;
        } else {
            print("unknown command: ");
            print(line);
            print("  (try 'help')\n");
        }
    }
}
