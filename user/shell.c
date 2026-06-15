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

/* Print a month calendar for the current date (from the RTC). */
static void cmd_cal(void) {
    char t[24];
    sys_time(t, sizeof(t));                          /* "YYYY-MM-DD HH:MM:SS" */
    int y = (t[0]-'0')*1000 + (t[1]-'0')*100 + (t[2]-'0')*10 + (t[3]-'0');
    int m = (t[5]-'0')*10 + (t[6]-'0');
    int today = (t[8]-'0')*10 + (t[9]-'0');
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
            print("files:  ls cat head tail sort edit write rm cp mv mkdir cd pwd tree find grep hexdump wc\n");
            print("net:    get<url> headers<url> wget<url file> browse<url>\n");
            print("        ping resolve<host>\n");
            print("crypto: sha256<file> crypt base64     run: run<prog>  js<file>\n");
            print("misc:   echo cal date beep mem ps df history clear reboot exit\n");
        } else if (streq(line, "ls")) {
            char buf[1024];
            sys_list(buf, sizeof(buf));
            print(buf);
        } else if (startswith(line, "cat ")) {
            char buf[2048];
            long n = sys_readfile(line + 4, buf, sizeof(buf) - 1);
            if (n < 0) {
                print("cat: no such file: ");
                print(line + 4);
                print("\n");
            } else {
                buf[n] = '\0';
                print(buf);
            }
        } else if (startswith(line, "head ")) {
            char buf[2048];
            long n = sys_readfile(line + 5, buf, sizeof(buf) - 1);
            if (n < 0) { print("head: no such file: "); print(line + 5); print("\n"); }
            else {
                int i = 0, lines = 0;
                for (; i < n && lines < 20; i++) if (buf[i] == '\n') lines++;   /* first 20 lines */
                buf[i] = '\0'; print(buf);
                if (i < n) print("...\n");
            }
        } else if (startswith(line, "tail ")) {
            char buf[2048];
            long n = sys_readfile(line + 5, buf, sizeof(buf) - 1);
            if (n < 0) { print("tail: no such file: "); print(line + 5); print("\n"); }
            else {
                buf[n] = '\0';
                int total = 0;
                for (int i = 0; i < n; i++) if (buf[i] == '\n') total++;
                if (n > 0 && buf[n - 1] != '\n') total++;
                int skip = total > 20 ? total - 20 : 0;       /* keep the last 20 lines */
                int i = 0, sk = 0;
                while (i < n && sk < skip) { if (buf[i++] == '\n') sk++; }
                print(buf + i);
            }
        } else if (startswith(line, "sort ")) {
            static char buf[2048];
            long n = sys_readfile(line + 5, buf, sizeof(buf) - 1);
            if (n < 0) { print("sort: no such file: "); print(line + 5); print("\n"); }
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
                        if ((unsigned char)*a <= (unsigned char)*b) break;
                        lns[j+1] = lns[j]; j--;
                    }
                    lns[j+1] = key;
                }
                for (int i = 0; i < nl; i++) { print(lns[i]); print("\n"); }
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
        } else if (streq(line, "ping")) {
            long n = sys_ping();
            if (n < 0) print("ping: no network\n");
            else {
                char c[2] = { (char)('0' + (int)n), 0 };
                print("gateway 10.0.2.2: "); print(c); print("/3 replies\n");
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
        } else if (startswith(line, "grep ")) {
            char *p = line + 5, pat[40]; int i = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && i < 39) pat[i++] = *p++;
            pat[i] = 0;
            while (*p == ' ') p++;
            if (pat[0] == 0 || *p == 0) { print("usage: grep <pattern> <file>\n"); }
            else {
                static char buf[2048];
                long n = sys_readfile(p, buf, sizeof(buf) - 1);   /* -1 so buf[n]=0 can't write past the array */
                if (n < 0) { print("grep: no such file\n"); }
                else {
                    buf[n] = 0;
                    int ls = 0, hits = 0;
                    for (long k = 0; k <= n; k++) {
                        if (k == n || buf[k] == '\n') {
                            int found = 0;
                            for (long a = ls; a < k && !found; a++) {
                                int b = 0;
                                while (a + b < k && pat[b] && buf[a+b] == pat[b]) b++;
                                if (!pat[b]) found = 1;
                            }
                            if (found) {
                                char save = buf[k]; buf[k] = 0;
                                print("  "); print(buf + ls); print("\n");
                                buf[k] = save; hits++;
                            }
                            ls = (int)k + 1;
                        }
                    }
                    if (!hits) print("  (no matches)\n");
                }
            }
        } else if (startswith(line, "wc ")) {
            static char buf[2048];
            long n = sys_readfile(line + 3, buf, sizeof(buf));
            if (n < 0) { print("wc: no such file\n"); }
            else {
                int lines = 0, words = 0, inword = 0;
                for (long i = 0; i < n; i++) {
                    char c = buf[i];
                    if (c == '\n') lines++;
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') inword = 0;
                    else if (!inword) { inword = 1; words++; }
                }
                char num[12];
                print("  lines "); itoa_simple(lines, num); print(num);
                print("  words "); itoa_simple(words, num); print(num);
                print("  bytes "); itoa_simple((int)n, num); print(num); print("\n");
            }
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
        } else if (startswith(line, "run ")) {
            if (sys_spawn(line + 4) < 0) print("run: no such program (built-in: shell clock calc snake editor 2048; or a disk .elf)\n");
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
