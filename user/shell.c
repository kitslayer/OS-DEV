/*
 * shell.c — a tiny interactive shell, running entirely in ring 3.
 *
 * It loops: print a prompt, read a line via SYS_read, and match it against a
 * few built-in commands. Everything it does — printing, reading input, exiting
 * — is a system call into the kernel. This is a real (if minimal) userspace
 * program talking to our OS exactly the way `sh` talks to Linux.
 */
#include "ulib.h"
#include "shgrep.h"   /* gr_match(): the grep regex matcher (^ $ . * [..] \), host-tested by tests/shgrep */
#include "shmath.h"   /* sh_eval(): the $((expr)) integer evaluator, host-tested by tests/shmath */

static void itoa_simple(int v, char *out) {
    char tmp[12];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v > 0) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
}

/* Exit status of the last command (0 = success), exposed as $? and consumed by
 * the && / || operators. run_command resets it to 0 on entry; failure paths
 * (command-not-found, a missing file via slurp(), cd/mkdir failure, `false`)
 * set it to 1. */
static int g_status;
static int source_depth;   /* recursion guard for `source` (scripts sourcing scripts) */
static char prevcwd[128];  /* the directory before the last cd, for `cd -` */
static void scpy(char *d, const char *s) { int i = 0; while (s[i] && i < 127) { d[i] = s[i]; i++; } d[i] = 0; }
static int nargs(const char *s) { int n = 0; while (*s) { while (*s == ' ') s++; if (!*s) break; n++; while (*s && *s != ' ') s++; } return n; }

/* Forward decls: `source` and `for` run lines/bodies back through the executor.
 * Defined far below, after run_line. run_input_line handles one logical line
 * (a `for ...; do ...; done` loop, else a ';'-split list of && / || commands). */
static int run_andor(char *seg, char *cwd);
static int run_input_line(char *line, char *cwd);
static int run_for(char *line, char *cwd);
static int run_while(char *line, char *cwd);
static void source_file(const char *fn, char *cwd, int silent);   /* run shell commands from a file */

/* Read an entire file into a malloc'd, NUL-terminated buffer (caller free()s).
 * The read API has no size query, so grow the buffer until the read no longer
 * fills it — commands then see the whole file, not a fixed 2KB prefix. *len gets
 * the length; returns 0 on missing file / >=32MB / OOM. */
static char *slurp(const char *name, long *len) {
    unsigned long cap = 65536;
    char *b = malloc(cap);
    long n = b ? sys_readfile(name, b, cap - 1) : -1;
    while (b && n == (long)(cap - 1) && cap < (32UL << 20)) {   /* read filled the buffer: file may be larger */
        cap <<= 1; free(b); b = malloc(cap);
        if (b) n = sys_readfile(name, b, cap - 1);
    }
    if (!b || n < 0 || n == (long)(cap - 1)) { free(b); *len = -1; g_status = 1; return 0; }
    b[n] = 0; *len = n; return b;
}

/* Run inline JS for `js -e <code>`. Intercepted early (in run_input_line) so the
 * code reaches the engine VERBATIM — JS's >, <, |, &, ; are operators here, not
 * shell metacharacters, and must not be redirected/piped/split/globbed away
 * (e.g. an arrow `x => x*2` contains a `>`). Output is still capturable with
 * $(...). */
static void run_js_inline(const char *code) {
    char *out = malloc(1u << 20);                /* 1MB output buffer */
    if (!out) { print("js: out of memory\n"); return; }
    sys_js(code, out, (1u << 20) - 1);
    out[(1u << 20) - 1] = 0;
    print(out);
    free(out);
}
/* parse a leading (optionally signed) integer from a line, for `sort -n`. */
static long sort_numval(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0; if (*s == '-') { neg = 1; s++; }
    long v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}
static int sort_foldeq(const char *a, const char *b) {   /* case-insensitive string equality (for sort -uf) */
    while (*a && gr_lc(*a) == gr_lc(*b)) { a++; b++; }
    return gr_lc(*a) == gr_lc(*b);
}
/* Expand a `tr` SET token (literal chars + a-z ranges) into out[]; advance *pp past it. */
static int tr_expand(const char **pp, char *out, int max) {
    const char *s = *pp; int o = 0;
    while (*s && *s != ' ' && o < max) {
        if (s[1] == '-' && s[2] && s[2] != ' ' && (unsigned char)s[2] >= (unsigned char)s[0]) {
            for (int c = (unsigned char)s[0]; c <= (unsigned char)s[2] && o < max; c++) out[o++] = (char)c;
            s += 3;
        } else out[o++] = *s++;
    }
    *pp = s;
    return o;
}

/* (the grep regex matcher gr_match() now lives in shgrep.h, #included above) */
static int b64v(char c) {                 /* base64 digit -> value, or -1 */
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
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

/*
 * Dispatch one command line. Returns 1 if the shell should exit (the "exit"
 * command), else 0. `cwd` is the display path (an array in main); cd/pwd mutate
 * it in place and the changes persist via the pointer.
 *
 * The whole if/else-if chain is wrapped in `do { ... } while (0);` so that the
 * original dispatch-level `continue;` (used to skip to the next command) still
 * does exactly that — it jumps to the `while (0)` and falls out of the block —
 * without disturbing any `continue` inside a command's own for/while loop.
 */
/* ---- shell variables: `set NAME=value` (or `export`), use as $NAME / ${NAME},
 * `unset NAME`, `set`/`env` list them. Persist for the shell process's lifetime. ---- */
static struct { char name[24], val[160]; } g_vars[24];
static int g_nvars;
static const char *vget(const char *n, int nl){
    for (int i=0;i<g_nvars;i++){ int j=0; while(j<nl && g_vars[i].name[j] && g_vars[i].name[j]==n[j]) j++;
        if (j==nl && g_vars[i].name[j]==0) return g_vars[i].val; }
    return 0;
}
/* shmath.h's variable hook: a name's integer value (0 if unset / non-numeric). */
static long sh_var(const char *name, int len){ const char *v = vget(name, len); return v ? sh_str2long(v) : 0; }
static void vset(const char *n, int nl, const char *v){
    if (nl<1) return; if (nl>23) nl=23;
    int slot=-1;
    for (int i=0;i<g_nvars && slot<0;i++){ int j=0; while(j<nl && g_vars[i].name[j]==n[j]) j++; if (j==nl && g_vars[i].name[j]==0) slot=i; }
    if (slot<0){ if (g_nvars>=24) return; slot=g_nvars++; int k=0; for(;k<nl;k++) g_vars[slot].name[k]=n[k]; g_vars[slot].name[k]=0; }
    int k=0; for(;v[k] && k<159;k++) g_vars[slot].val[k]=v[k]; g_vars[slot].val[k]=0;
}
static void vunset(const char *n){
    int nl=0; while(n[nl] && n[nl]!=' ') nl++;
    for (int i=0;i<g_nvars;i++){ int j=0; while(j<nl && g_vars[i].name[j]==n[j]) j++;
        if (j==nl && g_vars[i].name[j]==0){ g_vars[i]=g_vars[--g_nvars]; return; } }
}

/* ---- command aliases: `alias name=value`, expanded on the first word ------ */
static struct { char name[16], val[160]; } g_alias[16];
static int g_nalias;
static const char *alias_get(const char *n, int nl){
    for (int i=0;i<g_nalias;i++){ int j=0; while(j<nl && g_alias[i].name[j] && g_alias[i].name[j]==n[j]) j++;
        if (j==nl && g_alias[i].name[j]==0) return g_alias[i].val; }
    return 0;
}
static void alias_set(const char *n, int nl, const char *v){
    if (nl<1) return; if (nl>15) nl=15;
    int slot=-1;
    for (int i=0;i<g_nalias && slot<0;i++){ int j=0; while(j<nl && g_alias[i].name[j]==n[j]) j++; if (j==nl && g_alias[i].name[j]==0) slot=i; }
    if (slot<0){ if (g_nalias>=16) return; slot=g_nalias++; int k=0; for(;k<nl;k++) g_alias[slot].name[k]=n[k]; g_alias[slot].name[k]=0; }
    int k=0; for(;v[k] && k<159;k++) g_alias[slot].val[k]=v[k]; g_alias[slot].val[k]=0;
}
static void alias_del(const char *n){
    int nl=0; while(n[nl] && n[nl]!=' ') nl++;
    for (int i=0;i<g_nalias;i++){ int j=0; while(j<nl && g_alias[i].name[j]==n[j]) j++;
        if (j==nl && g_alias[i].name[j]==0){ g_alias[i]=g_alias[--g_nalias]; return; } }
}
/* The $((expr)) evaluator (sh_eval, sh_vchar, sh_askip, …) now lives in
 * shmath.h, host-tested by tests/shmath; the sh_var hook above resolves names. */

/* Expand $NAME / ${NAME} and $((expr)) in src into dst; returns 1 if a '$'
 * appeared (dst is then the result). */
static int expand_vars(const char *src, char *dst, int cap){
    int has=0; for (int i=0;src[i];i++) if (src[i]=='$'){ has=1; break; }
    if (!has) return 0;
    int o=0;
    for (int i=0; src[i] && o<cap-1; ){
        if (src[i]=='$' && src[i+1]=='(' && src[i+2]=='('){        /* $((expr)) arithmetic */
            const char *q = src + i + 3;
            long val = sh_eval(&q);
            sh_askip(&q); if (*q==')') q++; if (*q==')') q++;       /* consume the closing )) */
            char tmp[24]; int ti=0; int neg = val<0;
            unsigned long uv = neg ? (unsigned long)(-val) : (unsigned long)val;
            if (uv==0) tmp[ti++]='0'; while(uv){ tmp[ti++]=(char)('0'+uv%10); uv/=10; }
            if (neg) tmp[ti++]='-';
            while (ti>0 && o<cap-1) dst[o++]=tmp[--ti];
            i = (int)(q - src);
        } else if (src[i]=='$' && src[i+1]=='?'){                   /* $? -> last exit status */
            char tmp[12]; int ti=0; unsigned uv=(unsigned)(g_status<0?0:g_status);
            if (uv==0) tmp[ti++]='0';
            while (uv){ tmp[ti++]=(char)('0'+uv%10); uv/=10; }
            while (ti>0 && o<cap-1) dst[o++]=tmp[--ti];
            i += 2;
        } else if (src[i]=='$'){                                    /* $NAME / ${NAME} */
            int br=(src[i+1]=='{'); int s=i+1+br, e=s; while (src[e] && sh_vchar(src[e])) e++;
            const char *v=(e>s)?vget(src+s,e-s):0;
            if (v) for (int k=0; v[k] && o<cap-1; k++) dst[o++]=v[k];
            i = e + ((br && src[e]=='}')?1:0);
        } else dst[o++]=src[i++];
    }
    dst[o]=0; return 1;
}

static int run_command(char *line, char *cwd) {
    g_status = 0;                          /* assume success; failure paths set $? = 1 */
    do {
        if (line[0] == '\0') {
            continue;
        } else if (streq(line, "help")) {
            print("files:  ls cat head tail sort[-nruf] nl tac uniq[-cdu] cut[-c/-f] cmp<f1 f2> paste<f1 f2> comm<f1 f2> diff<f1 f2> edit write rm cp mv mkdir touch cd pwd basename<p> dirname<p> tree find grep[-incvel,regex] file<n> hexdump strings<file> unhex<hex> gzip<f> gunzip<f.gz> unzip<f.zip> tar<f.tgz> wc[-lwc] tr fold seq[a b c]\n");
            print("net:    get<url> headers<url> wget<url file> browse<url>\n");
            print("        ping[<host>] resolve<host> ifconfig\n");
            print("crypto: sha256<file> sha512<file> crc32<file> genpass[ N] uuidgen crypt base64 unbase64<b64>\n");
            print("        run: apps run<prog> js<file>\n");
            print("math:   factor<n> roll<NdM> seq<n> base<N> dec<0x..> roman<N> gcd<a b> primes<N> fib<N> fizzbuzz<N> stats<n..> size<bytes>\n");
            print("misc:   echo cal[ M Y] weekday<YYYYMMDD> dur<sec> date beep tone[ hz ms] play<f.wav> stop morse<text> unmorse<code> rev<text> rot13<text> ascii cowsay<text> fortune\n");
            print("        todo[ add T|done N|clear] clip[ file] mem ps df scores history clear reboot exit\n");
            print("syntax: cmd1 | cmd2 (pipe)   cmd > file (write)   cmd >> file (append)   cmd < file (read)   $(cmd) (substitute)\n");
            print("        a && b (b if a ok)   a || b (b if a fails)   $? (last status)  true false\n");
            print("        source file (or '. file'): run shell commands from a file (# = comment)\n");
            print("        .SHRC in / is auto-run at shell start (put aliases/set/etc. there)\n");
            print("        read VAR: read a line of input into VAR (for interactive scripts)\n");
            print("        for V in WORDS; do CMDS; done   (loop: WORDS get glob/$var expansion)\n");
            print("        while COND; do CMDS; done   (loops while COND succeeds; Ctrl-C to stop)\n");
            print("        if COND; then CMDS; [else CMDS;] fi   (COND's exit status picks the branch)\n");
            print("        test/[ ]: A -eq/-ne/-lt/-gt/-le/-ge B, A =/!= B, -z/-n S, -e/-f F, ! EXPR\n");
            print("        alias name=value   unalias name   (shortcuts, expanded on the first word)\n");
            print("        *.txt ? (glob)   cmd1 ; cmd2 (run both)   !! (repeat last command)\n");
            print("        set NAME=val (variables) $NAME / ${NAME}   $((expr)) arithmetic   unset NAME   env\n");
            print("edit:   arrows move  Home/End  Del  up/down=history  ^W/^U/^K=kill  ^C=cancel\n");
            print("        Tab completes a filename (longest common prefix); a 2nd Tab lists the matches\n");
        } else if (startswith(line, "set ") || startswith(line, "export ")) {
            const char *p = line + (line[1]=='x' ? 7 : 4); while (*p == ' ') p++;   /* skip "set "/"export " */
            int nl = 0; while (p[nl] && p[nl] != '=' && p[nl] != ' ') nl++;
            if (p[nl] == '=' && nl > 0) vset(p, nl, p + nl + 1);                    /* value = rest of line (may contain spaces) */
            else print("usage: set NAME=value\n");
        } else if (streq(line, "set") || streq(line, "env")) {                       /* list all variables */
            for (int i = 0; i < g_nvars; i++) { print(g_vars[i].name); print("="); print(g_vars[i].val); print("\n"); }
            if (g_nvars == 0) print("(no variables set)\n");
        } else if (startswith(line, "read ")) {                                       /* read a line of input into a variable */
            const char *p = line + 5; while (*p == ' ') p++;
            int nl = 0; while (p[nl] && p[nl] != ' ') nl++;
            if (nl > 0) { char rb[256]; readline(rb, sizeof rb); vset(p, nl, rb); }
        } else if (startswith(line, "unset ")) {
            const char *p = line + 6; while (*p == ' ') p++; vunset(p);
        } else if (streq(line, "ls")) {
            char buf[8192];                 /* hold a full directory (~90+ files) */
            sys_list(buf, sizeof(buf));
            print(buf);
        } else if (startswith(line, "ls ")) {     /* ls <dir>: briefly cd there, list, restore cwd */
            const char *dir = line + 3; while (*dir == ' ') dir++;
            char buf[8192];
            if (sys_chdir(dir) < 0) { print("ls: no such directory: "); print(dir); print("\n"); g_status = 1; }
            else { sys_list(buf, sizeof buf); print(buf); sys_chdir(cwd); }
        } else if (startswith(line, "cat ")) {
            const char *p = line + 4; int any = 0;
            while (*p) {                                  /* concatenate each space-separated file */
                while (*p == ' ') p++;
                if (!*p) break;
                char name[64]; int i = 0;
                while (*p && *p != ' ' && i < 63) name[i++] = *p++;
                name[i] = '\0'; any = 1;
                long n; char *buf = slurp(name, &n);
                if (!buf) { print("cat: no such file: "); print(name); print("\n"); }
                else { print(buf); free(buf); }
            }
            if (!any) print("usage: cat <file>...\n");
        } else if (startswith(line, "head ")) {
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
                long n; char *buf = slurp(name, &n);
                if (!buf) { print("head: no such file: "); print(name); print("\n"); continue; }
                if (fc > 1) { print("==> "); print(name); print(" <==\n"); }
                int i = 0, lines = 0;
                for (; i < n && lines < cnt; i++) if (buf[i] == '\n') lines++;
                buf[i] = '\0'; print(buf);
                if (i < n && !cap_active()) print("...\n");   /* "more lines" hint for the screen; never into a pipe/$() data stream */
                free(buf);
            }
            if (!any) print("usage: head <file>...\n");
        } else if (startswith(line, "nl ")) {
            long n; char *buf = slurp(line + 3, &n);
            if (!buf) { print("nl: no such file: "); print(line + 3); print("\n"); }
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
                free(buf);
            }
        } else if (startswith(line, "tail ")) {
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
                long n; char *buf = slurp(name, &n);
                if (!buf) { print("tail: no such file: "); print(name); print("\n"); continue; }
                if (fc > 1) { print("==> "); print(name); print(" <==\n"); }
                buf[n] = '\0';
                int total = 0;
                for (int i = 0; i < n; i++) if (buf[i] == '\n') total++;
                if (n > 0 && buf[n - 1] != '\n') total++;
                int skip = total > cnt ? total - cnt : 0;     /* keep the last cnt lines */
                int i = 0, sk = 0;
                while (i < n && sk < skip) { if (buf[i++] == '\n') sk++; }
                print(buf + i);
                free(buf);
            }
            if (!any) print("usage: tail <file>...\n");
        } else if (startswith(line, "tac ")) {           /* print a file's lines in reverse order */
            long n; char *buf = slurp(line + 4, &n);
            if (!buf) { print("tac: no such file: "); print(line + 4); print("\n"); }
            else {
                buf[n] = 0;
                int cap = 1; for (long i = 0; i < n; i++) if (buf[i] == '\n') cap++;   /* one slot per line (was fixed 1024) */
                int *starts = malloc((unsigned long)cap * sizeof(int));
                if (!starts) print("tac: out of memory\n");
                else {
                    int ns = 0; starts[ns++] = 0;
                    for (long i = 0; i < n; i++) if (buf[i] == '\n' && ns < cap) starts[ns++] = (int)(i + 1);
                    for (int k = ns - 1; k >= 0; k--) {
                        int s = starts[k]; if (s >= (int)n) continue;     /* skip empty trailing line */
                        int e = s; while (e < (int)n && buf[e] != '\n') e++;
                        char save = buf[e]; buf[e] = 0;
                        print(buf + s); print("\n");
                        buf[e] = save;
                    }
                    free(starts);
                }
                free(buf);
            }
        } else if (startswith(line, "uniq ")) {           /* drop adjacent duplicate lines; -c prefixes a run count */
            const char *fp = line + 5; int countf = 0, dflag = 0, uflag = 0;   /* -c count, -d only duplicated runs, -u only non-repeated runs */
            while (*fp == ' ') fp++;
            while (fp[0] == '-' && fp[1] && fp[1] != ' ') {
                int t, valid = 1;
                for (t = 1; fp[t] && fp[t] != ' '; t++) if (fp[t] != 'c' && fp[t] != 'd' && fp[t] != 'u') valid = 0;
                if (!valid) break;                          /* not a flag token (e.g. a filename) */
                for (t = 1; fp[t] && fp[t] != ' '; t++) { if (fp[t] == 'c') countf = 1; else if (fp[t] == 'd') dflag = 1; else uflag = 1; }
                fp += t; while (*fp == ' ') fp++;
            }
            long n; char *buf = slurp(fp, &n);
            if (!buf) { print("uniq: no such file: "); print(fp); print("\n"); }
            else {
                buf[n] = 0;
                int rs = -1, rl = 0, ls = 0, count = 0;    /* current run's first line [rs,rs+rl); occurrence count */
                for (long k = 0; k <= n; k++) {
                    if (k == n || buf[k] == '\n') {
                        int len = (int)(k - ls);
                        if (k == n && len == 0) break;      /* no trailing empty line */
                        int same = (rs >= 0 && rl == len);
                        if (same) for (int j = 0; j < len; j++) if (buf[ls + j] != buf[rs + j]) { same = 0; break; }
                        if (same) count++;
                        else {
                            if (rs >= 0 && (dflag ? count > 1 : uflag ? count == 1 : 1)) {  /* emit the ended run (-d: dups only, -u: non-repeated only) */
                                char save = buf[rs + rl]; buf[rs + rl] = 0;
                                if (countf) { char cb[12]; itoa_simple(count, cb); print("  "); print(cb); print(" "); }
                                print(buf + rs); print("\n");
                                buf[rs + rl] = save;
                            }
                            rs = ls; rl = len; count = 1;
                        }
                        ls = (int)k + 1;
                    }
                }
                if (rs >= 0 && (dflag ? count > 1 : uflag ? count == 1 : 1)) {   /* emit the final run (-d/-u filter) */
                    char save = buf[rs + rl]; buf[rs + rl] = 0;
                    if (countf) { char cb[12]; itoa_simple(count, cb); print("  "); print(cb); print(" "); }
                    print(buf + rs); print("\n");
                    buf[rs + rl] = save;
                }
                free(buf);
            }
        } else if (startswith(line, "seq ")) {             /* seq [first [incr]] last -> one number per line */
            const char *p = line + 4; while (*p == ' ') p++;
            long arg[3]; int na = 0;
            while (*p && na < 3) {
                int neg = 0; if (*p == '-') { neg = 1; p++; }
                if (*p < '0' || *p > '9') break;
                long v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
                arg[na++] = neg ? -v : v;
                while (*p == ' ') p++;
            }
            long first = 1, incr = 1, last = 0;
            if (na == 1) last = arg[0];
            else if (na == 2) { first = arg[0]; last = arg[1]; }
            else if (na == 3) { first = arg[0]; incr = arg[1]; last = arg[2]; }
            if (na < 1 || na > 3 || incr == 0) { print("usage: seq [first [incr]] last\n"); }
            else {
                char num[16]; long cnt = 0;
                for (long v = first; (incr > 0) ? (v <= last) : (v >= last); v += incr) {
                    if (++cnt > 1000000) break;             /* safety cap on output */
                    itoa_simple((int)v, num); print(num); print("\n");
                }
            }
        } else if (startswith(line, "sort ")) {
            const char *fp = line + 5; int rev = 0, nsort = 0, uniq_f = 0, fold = 0;  /* -r reverse, -n numeric, -u unique, -f fold case */
            while (*fp == ' ') fp++;
            while (fp[0] == '-' && fp[1] && fp[1] != ' ') {
                int t, valid = 1;
                for (t = 1; fp[t] && fp[t] != ' '; t++) if (fp[t] != 'r' && fp[t] != 'n' && fp[t] != 'u' && fp[t] != 'f') valid = 0;
                if (!valid) break;                          /* not a flag token (e.g. a filename) */
                for (t = 1; fp[t] && fp[t] != ' '; t++) { if (fp[t] == 'r') rev = 1; else if (fp[t] == 'n') nsort = 1; else if (fp[t] == 'u') uniq_f = 1; else fold = 1; }
                fp += t; while (*fp == ' ') fp++;
            }
            long n; char *buf = slurp(fp, &n);
            if (!buf) { print("sort: no such file: "); print(fp); print("\n"); }
            else {
                buf[n] = '\0';
                int cap = 1; for (long i = 0; i < n; i++) if (buf[i] == '\n') cap++;   /* one slot per line (was fixed at 128) */
                char **lns = malloc((unsigned long)cap * sizeof(char *));
                if (!lns) print("sort: out of memory\n");
                else {
                    int nl = 0; char *p = buf;
                    while (*p && nl < cap) {                       /* split into lines */
                        lns[nl++] = p;
                        while (*p && *p != '\n') p++;
                        if (*p == '\n') *p++ = '\0';
                    }
                    for (int i = 1; i < nl; i++) {                 /* insertion sort (byte order) */
                        char *key = lns[i]; int j = i - 1;
                        while (j >= 0) {
                            int cmp;
                            if (nsort) {                       /* -n: compare by leading integer value */
                                long va = sort_numval(lns[j]), vb = sort_numval(key);
                                cmp = (va < vb) ? -1 : (va > vb) ? 1 : 0;
                            } else if (fold) {                 /* -f: case-insensitive byte order */
                                const char *a = lns[j], *b = key;
                                while (*a && gr_lc(*a) == gr_lc(*b)) { a++; b++; }
                                cmp = (int)(unsigned char)gr_lc(*a) - (int)(unsigned char)gr_lc(*b);
                            } else {                           /* byte order */
                                const char *a = lns[j], *b = key;
                                while (*a && *a == *b) { a++; b++; }
                                cmp = (int)(unsigned char)*a - (int)(unsigned char)*b;
                            }
                            if (rev) cmp = -cmp;
                            if (cmp <= 0) break;                   /* lns[j] already in order vs key */
                            lns[j+1] = lns[j]; j--;
                        }
                        lns[j+1] = key;
                    }
                    const char *prevl = 0;
                    for (int i = 0; i < nl; i++) {
                        if (uniq_f && prevl && (fold ? sort_foldeq(prevl, lns[i]) : streq(prevl, lns[i]))) continue;   /* -u: drop adjacent duplicates (-f: case-insensitively) */
                        print(lns[i]); print("\n");
                        prevl = lns[i];
                    }
                    free(lns);
                }
                free(buf);
            }
        } else if (startswith(line, "tr ")) {              /* tr -d SET FILE (delete) | tr SET1 SET2 FILE (translate); SETs take a-z ranges */
            const char *p = line + 3; while (*p == ' ') p++;
            if (p[0] == '-' && p[1] == 'd' && p[2] == ' ') {
                p += 3; while (*p == ' ') p++;
                char del[128]; int dn = tr_expand(&p, del, 128);   /* SET to delete (chars + ranges) */
                while (*p == ' ') p++;
                long n; char *buf = slurp(p, &n);
                if (!buf) { print("tr: no such file: "); print(p); print("\n"); }
                else {
                    long oi = 0;
                    for (long i = 0; i < n; i++) {
                        int drop = 0;
                        for (int j = 0; j < dn; j++) if (buf[i] == del[j]) { drop = 1; break; }
                        if (!drop) buf[oi++] = buf[i];          /* compact in place (oi <= i) */
                    }
                    buf[oi] = 0; print(buf); free(buf);
                }
            } else {
                char s1[128], s2[128];
                int n1 = tr_expand(&p, s1, 128); while (*p == ' ') p++;   /* SET1 */
                int n2 = tr_expand(&p, s2, 128); while (*p == ' ') p++;   /* SET2 (shorter set: last char repeats) */
                long n; char *buf = slurp(p, &n);
                if (!buf || n1 == 0 || n2 == 0) { print("usage: tr SET1 SET2 FILE  |  tr -d SET FILE   (SETs: chars or a-z ranges)\n"); if (buf) free(buf); }
                else {
                    buf[n] = 0;
                    for (long i = 0; i < n; i++)
                        for (int j = 0; j < n1; j++) if (buf[i] == s1[j]) { buf[i] = s2[j < n2 ? j : n2 - 1]; break; }
                    print(buf); free(buf);
                }
            }
        } else if (startswith(line, "fold ")) {           /* fold [-w]N FILE : wrap each line at N columns (default 60) */
            const char *p = line + 5; while (*p == ' ') p++;
            int w = 0;
            if (p[0] == '-' && p[1] == 'w') p += 2;
            while (*p >= '0' && *p <= '9') w = w * 10 + (*p++ - '0');
            while (*p == ' ') p++;
            if (w < 1) w = 60;
            if (w > 200) w = 200;
            long n; char *buf = slurp(p, &n);
            if (!buf) { print("fold: no such file: "); print(p); print("\n"); }
            else {
                buf[n] = 0;
                char lbuf[202]; int li = 0, col = 0;       /* one physical line at a time (w <= 200) — flushed as we go, so no whole-output cap */
                for (long i = 0; i < n; i++) {
                    char c = buf[i];
                    if (c == '\n') { lbuf[li] = 0; print(lbuf); print("\n"); li = 0; col = 0; continue; }
                    lbuf[li++] = c; col++;
                    if (col >= w) { lbuf[li] = 0; print(lbuf); print("\n"); li = 0; col = 0; }
                }
                if (li > 0) { lbuf[li] = 0; print(lbuf); print("\n"); }   /* trailing partial line */
                free(buf);
            }
        } else if (startswith(line, "cut ")) {            /* cut -cN[-M] FILE (char range) | cut -fN[-M] [-dX] FILE (delimited fields, default tab) */
            const char *p = line + 4;
            int mode = 0, from = 0, to = 0, openend = 0; char delim = '\t';
            while (1) {                                    /* parse leading -c/-f/-d flags, any order */
                while (*p == ' ') p++;
                if (*p != '-') break;
                char fl = p[1]; p += 2;
                if (fl == 'c' || fl == 'f') {
                    mode = fl; from = 0; to = 0; openend = 0;
                    while (*p >= '0' && *p <= '9') { if (from < 100000000) from = from * 10 + (*p - '0'); p++; }   /* cap: no int overflow on absurd N */
                    if (*p == '-') { p++; if (*p >= '0' && *p <= '9') { while (*p >= '0' && *p <= '9') { if (to < 100000000) to = to * 10 + (*p - '0'); p++; } } else openend = 1; }
                    else to = from;                        /* -cN / -fN alone = just that column/field */
                } else if (fl == 'd') { if (*p) delim = *p++; }   /* single-char field delimiter */
                else break;                                /* unknown flag */
            }
            while (*p == ' ') p++;
            if (from < 1) from = 1;
            if (mode != 'c' && mode != 'f') { print("usage: cut -cN[-M] <file>  |  cut -fN[-M] [-dX] <file>  (fields; default delim = tab)\n"); }
            else {
                long n; char *buf = slurp(p, &n);
                if (!buf) { print("cut: no such file: "); print(p); print("\n"); }
                else {
                    buf[n] = 0;
                    char out[256]; int oi = 0, col = 0, field = 1, dirty = 0;
                    for (long k = 0; k < n; k++) {
                        char c = buf[k];
                        if (c == '\n') { out[oi] = 0; print(out); print("\n"); oi = 0; col = 0; field = 1; dirty = 0; continue; }
                        dirty = 1;
                        if (mode == 'c') {                            /* char range */
                            col++;
                            if (col >= from && (openend || col <= to) && oi < 255) out[oi++] = c;
                        } else if (c == delim) {                      /* field sep: emit delim only BETWEEN two selected fields (keeps empty fields) */
                            int s1 = field >= from && (openend || field <= to);
                            int s2 = field + 1 >= from && (openend || field + 1 <= to);
                            if (s1 && s2 && oi < 255) out[oi++] = delim;
                            field++;
                        } else if (field >= from && (openend || field <= to) && oi < 255) {
                            out[oi++] = c;                            /* field content */
                        }
                    }
                    if (dirty) { out[oi] = 0; print(out); print("\n"); }   /* trailing line w/o newline */
                }
                free(buf);
            }
        } else if (streq(line, "js") || startswith(line, "js ")) {
            static char src[8192];                   /* demo / inline code (the shell line is <=128 bytes) */
            char *jsrc = src, *filesrc = 0;
            int have = 0;
            if (streq(line, "js")) {                 /* no file: run a built-in demo */
                const char *demo =
                    "print(\"Hello from from-scratch JavaScript!\");\n"
                    "function fib(n){ return n<2 ? n : fib(n-1)+fib(n-2); }\n"
                    "var s=\"fib: \"; for (var i=0;i<12;i++) s+=fib(i)+\" \"; print(s);\n"
                    "var a=[3,1,2]; a.push(4); print(\"array: len \"+a.length+\" = [\"+a.join(\",\")+\"]\");\n"
                    "print(\"arrows: \"+[1,2,3,4,5].map(x=>x*x).filter(x=>x>4).join(\",\"));\n"
                    "var o={name:\"OS-DEV\", year:2026}; print(o.name+\" \"+o.year);\n"
                    "function fact(n){ return n<=1?1:n*fact(n-1); } print(\"6! = \"+fact(6));\n"
                    "print(\"2^10 = \"+(1<<10)+\", 17%5 = \"+(17%5));\n"
                    "class Money{ constructor(c){ this.c=c; } toString(){ return \"$\"+this.c; } }\n"
                    "print(\"toString: \"+new Money(42));\n"
                    "var temp={valueOf:function(){return 20;}}; print(\"valueOf: \"+(temp*2+5));\n"
                    "var dm=\"2026-06-18\".match(/(?<y>\\d+)-(?<mo>\\d+)/); print(\"regex groups: \"+dm.groups.y+\"/\"+dm.groups.mo);\n"
                    "print(\"json reviver: \"+JSON.parse('{\"n\":21}',function(k,v){return typeof v==\"number\"?v*2:v;}).n);\n";
                int i = 0; while (demo[i] && i < (int)sizeof(src) - 1) { src[i] = demo[i]; i++; } src[i] = 0;
                have = 1;
            } else if (startswith(line, "js -e ")) {     /* inline: js -e <code> */
                const char *code = line + 6;
                int i = 0; while (code[i] && i < (int)sizeof(src) - 1) { src[i] = code[i]; i++; } src[i] = 0;
                have = 1;
            } else {
                long n; filesrc = slurp(line + 3, &n);   /* whole JS file (was capped at 8KB) */
                if (!filesrc) { print("js: no such file: "); print(line + 3); print("\n"); }
                else { jsrc = filesrc; have = 1; }
            }
            if (have) {
                char *out = malloc(1u << 20);            /* 1MB JS output buffer (was 8KB) */
                if (out) { sys_js(jsrc, out, (1u << 20) - 1); out[(1u << 20) - 1] = 0; print(out); free(out); }
                else print("js: out of memory\n");
            }
            free(filesrc);                               /* free(NULL) safe */
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
            static char buf[16384];   /* read-modify-write list: generous so edits don't truncate it (~400 items) */
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
        } else if (streq(line, "true")) {
            /* exit status 0 (already set) — useful with && / || */
        } else if (streq(line, "false")) {
            g_status = 1;                          /* exit status 1 — useful with && / || */
        } else if (startswith(line, "test ") || (line[0] == '[' && line[1] == ' ')) {
            /* test EXPR / [ EXPR ] -> set $? (0 = true). Args are already
             * variable-expanded by run_line. Supports: STR (non-empty),
             * -z/-n STR, -e/-f FILE, A -eq/-ne/-lt/-gt/-le/-ge B, A =/!= B,
             * and a leading ! to negate. */
            const char *p = line + (line[0] == '[' ? 1 : 4);
            char *av[12]; static char tok[256]; int ac = 0, ti = 0;
            while (*p && ac < 12) {
                while (*p == ' ') p++;
                if (!*p) break;
                av[ac++] = tok + ti;
                while (*p && *p != ' ' && ti < 255) tok[ti++] = *p++;
                tok[ti++] = 0;
            }
            if (line[0] == '[' && ac > 0 && streq(av[ac-1], "]")) ac--;   /* drop closing ] */
            int neg = 0, i0 = 0;
            if (ac > 0 && streq(av[0], "!")) { neg = 1; i0 = 1; }
            int rem = ac - i0, res = 0;
            if (rem == 1) res = (av[i0][0] != 0);
            else if (rem == 2) {
                const char *op = av[i0], *a = av[i0+1];
                if (streq(op, "-z")) res = (a[0] == 0);
                else if (streq(op, "-n")) res = (a[0] != 0);
                else if (streq(op, "-e") || streq(op, "-f")) { char b; res = (sys_readfile(a, &b, 1) >= 0); }
            }
            else if (rem == 3) {
                const char *a = av[i0], *op = av[i0+1], *b = av[i0+2];
                if (streq(op, "=")) res = streq(a, b);
                else if (streq(op, "!=")) res = !streq(a, b);
                else { long x = sh_str2long(a), y = sh_str2long(b);
                    if (streq(op, "-eq")) res = (x == y); else if (streq(op, "-ne")) res = (x != y);
                    else if (streq(op, "-lt")) res = (x < y); else if (streq(op, "-gt")) res = (x > y);
                    else if (streq(op, "-le")) res = (x <= y); else if (streq(op, "-ge")) res = (x >= y); }
            }
            if (neg) res = !res;
            g_status = res ? 0 : 1;
        } else if (streq(line, "alias")) {         /* list aliases */
            for (int i = 0; i < g_nalias; i++) { print(g_alias[i].name); print("='"); print(g_alias[i].val); print("'\n"); }
            if (!g_nalias) print("(no aliases)\n");
        } else if (startswith(line, "alias ")) {   /* alias name=value  (or `alias name` to show one) */
            const char *p = line + 6; while (*p == ' ') p++;
            int nl = 0; while (p[nl] && p[nl] != '=' && p[nl] != ' ') nl++;
            if (p[nl] == '=' && nl > 0) alias_set(p, nl, p + nl + 1);
            else { const char *v = alias_get(p, nl); if (v) { print(p); print("='"); print(v); print("'\n"); } else { print("alias: not set\n"); g_status = 1; } }
        } else if (startswith(line, "unalias ")) {
            const char *p = line + 8; while (*p == ' ') p++; alias_del(p);
        } else if (streq(line, "clip")) {          /* print the system clipboard (GUI selection -> shell) */
            char cb[2048]; int n = sys_clip_get(cb, sizeof cb);
            if (n <= 0) print("(clipboard empty)\n");
            else { cb[n] = 0; print(cb); if (cb[n-1] != '\n') print("\n"); }
        } else if (startswith(line, "clip ")) {    /* clip <file>: set the clipboard from a file (or a pipe) */
            const char *fn = line + 5; while (*fn == ' ') fn++;
            long n; char *buf = slurp(fn, &n);
            if (!buf) { print("clip: no such file: "); print(fn); print("\n"); }
            else {
                int len = (int)n; if (len > 2047) len = 2047;
                sys_clip_set(buf, len); free(buf);
                char nb[12]; itoa_simple(len, nb);
                print("copied "); print(nb); print(" bytes to clipboard\n");
            }
        } else if (startswith(line, "source ") || startswith(line, ". ")) {
            /* Run shell commands from a file: each line goes through the same
             * ';' split + &&/|| layer as interactive input. '#' lines and blank
             * lines are skipped. The filename is copied out first because the
             * nested run_line() reuses run_line's static expand/glob buffers. */
            char fn[128]; const char *p = line + (line[0] == '.' ? 2 : 7);
            while (*p == ' ') p++;
            int fi = 0; while (*p && *p != ' ' && fi < 127) fn[fi++] = *p++; fn[fi] = 0;
            source_file(fn, cwd, 0);
        } else if (startswith(line, "crypt ")) {
            char *p = line + 6, fn[32]; int i = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && i < 31) fn[i++] = *p++;
            fn[i] = 0;
            while (*p == ' ') p++;
            if (fn[0] == 0 || *p == 0) print("usage: crypt <file> <pass>\n");
            else if (sys_crypt(fn, p) < 0) print("crypt: failed\n");
            else { print("crypt: "); print(fn); print(" (run again to reverse)\n"); }
        } else if (startswith(line, "base64 -d ")) {       /* decode base64 -> bytes, written to a file */
            char *q = line + 10; while (*q == ' ') q++;
            char src[64]; int si = 0; while (*q && *q != ' ' && si < 63) src[si++] = *q++; src[si] = 0;
            while (*q == ' ') q++;
            char dst[64]; int di = 0;
            if (*q) { while (*q && *q != ' ' && di < 63) dst[di++] = *q++; dst[di] = 0; }
            else { dst[0]='O'; dst[1]='U'; dst[2]='T'; dst[3]=0; }
            long n = -1; char *inb = src[0] ? slurp(src, &n) : 0;
            char *outb = inb ? malloc((unsigned long)n + 1) : 0;   /* decoded output is <= 3/4 of the base64 input */
            if (!inb || !outb) print("base64: no such file (usage: base64 -d <file> [out])\n");
            else {
                unsigned acc = 0; int nbits = 0, op = 0;
                for (long i = 0; i < n; i++) {
                    char c = inb[i];
                    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
                    if (c == '=') break;
                    int v = b64v(c); if (v < 0) break;
                    acc = (acc << 6) | (unsigned)v; nbits += 6;
                    if (nbits >= 8) { nbits -= 8; outb[op++] = (char)((acc >> nbits) & 0xff); }
                }
                if (sys_writefile(dst, outb, (unsigned long)op) < 0) print("base64: write failed\n");
                else { char nb[12]; itoa_simple(op, nb); print("base64: wrote "); print(dst); print(" ("); print(nb); print(" bytes)\n"); }
            }
            free(inb); free(outb);
        } else if (startswith(line, "base64 ")) {
            char *a = line + 7; while (*a == ' ') a++;     /* trim spaces so `base64 F > OUT` works */
            char fn[64]; int fi = 0; while (*a && *a != ' ' && fi < 63) fn[fi++] = *a++; fn[fi] = 0;
            long n = -1; char *buf = fn[0] ? slurp(fn, &n) : 0;
            if (!buf) { print("base64: no such file\n"); }
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
                free(buf);
            }
        } else if (startswith(line, "sha256 ")) {
            const char *p = line + 7; int any = 0;       /* hash each space-separated file */
            while (*p) {
                while (*p == ' ') p++;
                if (!*p) break;
                char fn[64]; int j = 0; while (*p && *p != ' ' && j < 63) fn[j++] = *p++; fn[j] = 0;
                any = 1; char hex[72];
                if (sys_sha256(fn, hex, sizeof hex) < 0) { print("sha256: no such file: "); print(fn); print("\n"); g_status = 1; }
                else { print("  "); print(hex); print("  "); print(fn); print("\n"); }
            }
            if (!any) print("usage: sha256 <file>...\n");
        } else if (startswith(line, "sha512 ")) {
            const char *p = line + 7; int any = 0;
            while (*p) {
                while (*p == ' ') p++;
                if (!*p) break;
                char fn[64]; int j = 0; while (*p && *p != ' ' && j < 63) fn[j++] = *p++; fn[j] = 0;
                any = 1; char hex[136];
                if (sys_sha512(fn, hex, sizeof hex) < 0) { print("sha512: no such file: "); print(fn); print("\n"); g_status = 1; }
                else { print("  "); print(hex); print("  "); print(fn); print("\n"); }
            }
            if (!any) print("usage: sha512 <file>...\n");
        } else if (startswith(line, "crc32 ")) {       /* CRC-32 (IEEE 802.3, as in zip/gzip/png) over each file */
            const char *fp = line + 6; int any = 0;
            while (*fp) {
                while (*fp == ' ') fp++;
                if (!*fp) break;
                char fn[64]; int j = 0; while (*fp && *fp != ' ' && j < 63) fn[j++] = *fp++; fn[j] = 0;
                any = 1;
                long cn; char *cbuf = slurp(fn, &cn);   /* slurp grows the buffer + sets $? on a missing file */
                if (!cbuf) { print("crc32: no such file: "); print(fn); print("\n"); continue; }
                unsigned c = 0xFFFFFFFFu;
                for (long i = 0; i < cn; i++) {
                    c ^= (unsigned char)cbuf[i];
                    for (int k = 0; k < 8; k++) c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0u);
                }
                c ^= 0xFFFFFFFFu; free(cbuf);
                char hex[9]; const char *hd = "0123456789abcdef";
                for (int i = 0; i < 8; i++) hex[i] = hd[(c >> ((7 - i) * 4)) & 0xF];
                hex[8] = 0;
                print("  "); print(hex); print("  "); print(fn); print("\n");
            }
            if (!any) print("usage: crc32 <file>...\n");
        } else if (startswith(line, "grep ")) {
            char *p = line + 5; char pats[8][40]; int npat = 0, ci = 0, nn = 0, cc = 0, vv = 0, ll = 0;
            while (*p == ' ') p++;
            while (p[0] == '-' && p[1] && p[1] != ' ') {   /* flags -i (case-insens), -n (line#s), -c (count), -v (invert); combinable as -in. -e <pat> adds a pattern: a line matches ANY (the alternation the tiny regex lacks, and avoids the shell `|` = pipe clash) */
                if (p[1] == '-' && (p[2] == ' ' || p[2] == 0)) { p += 2; while (*p == ' ') p++; break; }  /* "--": end of flags (pattern may then start with '-') */
                if (p[1] == 'e' && (p[2] == ' ' || p[2] == 0)) {   /* -e <pattern> (repeatable) */
                    p += 2; while (*p == ' ') p++;
                    int q = 0; while (*p && *p != ' ') { if (npat < 8 && q < 39) pats[npat][q++] = *p; p++; }
                    if (npat < 8) { pats[npat][q] = 0; npat++; }
                    while (*p == ' ') p++;
                    continue;
                }
                int t, valid = 1;
                for (t = 1; p[t] && p[t] != ' '; t++) if (p[t] != 'i' && p[t] != 'n' && p[t] != 'c' && p[t] != 'v' && p[t] != 'l') valid = 0;
                if (!valid) break;                          /* not a flag token (e.g. a pattern starting with '-') */
                for (t = 1; p[t] && p[t] != ' '; t++) { if (p[t] == 'i') ci = 1; else if (p[t] == 'n') nn = 1; else if (p[t] == 'c') cc = 1; else if (p[t] == 'v') vv = 1; else ll = 1; }   /* -l: list matching filenames only */
                p += t; while (*p == ' ') p++;
            }
            if (npat == 0) {                                /* no -e given: the first non-flag word is the pattern */
                int i = 0; while (*p && *p != ' ' && i < 39) pats[0][i++] = *p++;
                pats[0][i] = 0; if (i) npat = 1;
                while (*p == ' ') p++;
            }
            if (npat == 0 || *p == 0) { print("usage: grep [-incvl] [-e pat]... <pattern> <file>...  (regex: ^ $ . * [..] \\)\n"); }
            else {
                const char *cq = p; int fcount = 0;               /* count files: prefix names only if >1 */
                while (*cq) { while (*cq == ' ') cq++; if (!*cq) break; fcount++; while (*cq && *cq != ' ') cq++; }
                int hits = 0;
                while (*p) {                                       /* grep each space-separated file */
                    while (*p == ' ') p++;
                    if (!*p) break;
                    char name[64]; int j = 0;
                    while (*p && *p != ' ' && j < 63) name[j++] = *p++;
                    name[j] = 0;
                    long n; char *buf = slurp(name, &n);
                    if (!buf) { print("grep: no such file: "); print(name); print("\n"); continue; }
                    buf[n] = 0;
                    int ls = 0, lno = 0;
                    for (long k = 0; k <= n; k++) {
                        if (k == n || buf[k] == '\n') {
                            lno++;
                            char save = buf[k]; buf[k] = 0;   /* NUL-terminate this line for matching + printing */
                            int found = 0;                 /* ^ $ . * + literal/escape (tiny regex); match ANY -e pattern */
                            for (int pi = 0; pi < npat; pi++) if (gr_match(pats[pi], buf + ls, ci)) { found = 1; break; }
                            int hit = vv ? !found : found;   /* -v inverts: act on non-matching lines */
                            if (hit) {
                                hits++;
                                if (ll) { print(name); print("\n"); buf[k] = save; break; }   /* -l: this file matches; name once, next file */
                                if (!cc) {                  /* -c: count only, don't print the line */
                                    if (fcount > 1) print(name);
                                    if (fcount > 1 && nn) print(":");
                                    if (nn) { char ln_[12]; itoa_simple(lno, ln_); print(ln_); }
                                    if (fcount > 1 || nn) print(": "); else print("  ");
                                    print(buf + ls); print("\n");
                                }
                            }
                            buf[k] = save;
                            ls = (int)k + 1;
                        }
                    }
                    free(buf);
                }
                if (ll) { /* -l already printed the matching filenames; no summary line */ }
                else if (cc) { char cb[12]; itoa_simple(hits, cb); print("  "); print(cb); print("\n"); }
                else if (!hits && !cap_active()) print("  (no matches)\n");   /* screen-only note; would otherwise pollute a pipe/$() */
            }
        } else if (startswith(line, "wc ")) {
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
                long n; char *buf = slurp(name, &n);
                if (!buf) { print("wc: no such file: "); print(name); print("\n"); continue; }
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
                free(buf);
            }
            if (nfiles > 1) {
                if (wl) { print("  lines "); itoa_simple(tl, num); print(num); }
                if (ww) { print("  words "); itoa_simple(tw, num); print(num); }
                if (wcb) { print("  bytes "); itoa_simple((int)tb, num); print(num); }
                print("  total\n");
            }
            if (nfiles == 0) print("usage: wc [-lwc] <file>...\n");
        } else if (startswith(line, "hexdump ")) {
            const char *fp = line + 8; int any = 0, fc = 0;
            { const char *cq = fp; while (*cq) { while (*cq==' ') cq++; if (!*cq) break; fc++; while (*cq && *cq!=' ') cq++; } }
            while (*fp) {
                while (*fp == ' ') fp++;
                if (!*fp) break;
                char name[64]; int j = 0; while (*fp && *fp != ' ' && j < 63) name[j++] = *fp++; name[j] = 0;
                any = 1;
                long n; char *buf = slurp(name, &n);
                if (!buf) { print("hexdump: no such file: "); print(name); print("\n"); continue; }
                if (fc > 1) { print("==> "); print(name); print(" <==\n"); }
                const char *H = "0123456789abcdef";
                for (long off = 0; off < n; off += 8) {
                    char h[56]; int p = 0;
                    for (int s = 28; s >= 0; s -= 4) h[p++] = H[(off >> s) & 0xF];   /* 8-hex-digit offset (files can exceed 64KB now) */
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
                free(buf);
            }
            if (!any) print("usage: hexdump <file>...\n");
        } else if (startswith(line, "find ")) {
            static char fb[2048];
            long n = sys_find(line + 5, fb, sizeof(fb));
            if (n > 0) { fb[n] = 0; print(fb); } else if (!cap_active()) print("(no matches)\n");   /* hint, not data: keep it out of `for f in $(find ...)` */
        } else if (streq(line, "tree")) {
            static char tb[2048];
            long n = sys_tree(tb, sizeof(tb));
            if (n <= 0) print("(empty)\n"); else { tb[n] = 0; print(tb); }
        } else if (startswith(line, "mkdir ")) {
            const char *p = line + 6; int any = 0;       /* make each space-separated directory */
            while (*p) {
                while (*p == ' ') p++;
                if (!*p) break;
                char name[64]; int j = 0;
                while (*p && *p != ' ' && j < 63) name[j++] = *p++;
                name[j] = 0; any = 1;
                if (sys_mkdir(name) < 0) { print("mkdir: failed (exists?): "); print(name); print("\n"); g_status = 1; }
                else { print("created "); print(name); print("/\n"); }
            }
            if (!any) print("usage: mkdir <dir>...\n");
        } else if (streq(line, "cd -")) {                        /* swap to the previous directory */
            if (!prevcwd[0]) { print("cd: no previous directory\n"); g_status = 1; }
            else {
                char tmp[128]; scpy(tmp, cwd);
                if (sys_chdir(prevcwd) < 0) { print("cd: previous directory gone\n"); g_status = 1; }
                else { scpy(cwd, prevcwd); scpy(prevcwd, tmp); print(cwd); print("\n"); }
            }
        } else if (streq(line, "cd") || streq(line, "cd ~")) {   /* cd with no arg -> home (root) */
            scpy(prevcwd, cwd);
            sys_chdir("/"); cwd[0] = '/'; cwd[1] = 0;
        } else if (startswith(line, "cd ")) {
            char *path = line + 3;
            if (sys_chdir(path) < 0) { print("cd: no such directory\n"); g_status = 1; }
            else {
                scpy(prevcwd, cwd);                              /* remember where we came from */
                if (streq(path, "/")) { cwd[0] = '/'; cwd[1] = 0; }
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
        } else if (streq(line, "scores")) {
            /* a personal leaderboard: the best each game saved to its *.HI file */
            static const struct { const char *name, *file; } hs[] = {
                {"Snake","SNAKE.HI"},{"Tetris","TETRIS.HI"},{"Breakout","BREAKOUT.HI"},
                {"2048","2048.HI"},{"Minesweeper","MINES.HI"},{"Flappy","FLAPPY.HI"},
                {"Space Invaders","SPACEINV.HI"},{"Frogger","FROGGER.HI"},{"15-Puzzle","FIFTEEN.HI"},
                {"Maze","MAZE.HI"},{"Simon","SIMON.HI"},{"Blackjack","BJ.HI"},
                {"Typing","TYPING.HI"},{"Yahtzee","YAHTZEE.HI"},{"Video Poker","VPOKER.HI"},
                {"Pac-Man","PACMAN.HI"},
            };
            print("High scores (your bests):\n");
            int any = 0;
            for (int i = 0; i < (int)(sizeof(hs)/sizeof(hs[0])); i++) {
                long n; char *b = slurp(hs[i].file, &n);
                if (b && n > 0) {
                    int j = 0; while (j < n && b[j] >= '0' && b[j] <= '9') j++; b[j] = 0;
                    if (j > 0) { print("  "); print(hs[i].name); print(": "); print(b); print("\n"); any = 1; }
                }
                if (b) free(b);
            }
            if (!any) print("  (none yet - go set some!)\n");
        } else if (streq(line, "screenshot") || startswith(line, "screenshot ")) {
            const char *fn = line + 10; while (*fn == ' ') fn++;   /* optional filename arg */
            if (!*fn) fn = "SHOT.BMP";
            if (sys_screenshot(fn) < 0) print("screenshot: failed\n");
            else { print("saved screen to "); print(fn); print("\n"); }
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
        } else if (streq(line, "echo")) {
            print("\n");                                  /* bare echo: a blank line */
        } else if (startswith(line, "echo ")) {
            const char *a = line + 5;
            if (streq(a, "-n")) { }                       /* echo -n: print nothing, no newline */
            else if (startswith(a, "-n ")) print(a + 3);  /* echo -n TEXT: no trailing newline (good for prompts before `read`) */
            else { print(a); print("\n"); }
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
            const char *p = line + 4; while (*p == ' ') p++;
            char f1[64]; int j = 0; while (*p && *p != ' ' && j < 63) f1[j++] = *p++; f1[j] = 0;
            while (*p == ' ') p++;
            char f2[64]; j = 0; while (*p && *p != ' ' && j < 63) f2[j++] = *p++; f2[j] = 0;
            if (!f1[0] || !f2[0]) { print("usage: cmp <file1> <file2>\n"); }
            else {
                long n1; char *b1 = slurp(f1, &n1);
                long n2; char *b2 = slurp(f2, &n2);
                if (!b1)      { print("cmp: no such file: "); print(f1); print("\n"); }
                else if (!b2) { print("cmp: no such file: "); print(f2); print("\n"); }
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
                free(b1); free(b2);
            }
        } else if (startswith(line, "strings ")) {        /* strings FILE... -> runs of >=4 printable chars */
            const char *p = line + 8; int any = 0, fc = 0;
            { const char *cq = p; while (*cq) { while (*cq==' ') cq++; if (!*cq) break; fc++; while (*cq && *cq!=' ') cq++; } }
            while (*p) {
                while (*p == ' ') p++;
                if (!*p) break;
                char name[64]; int j = 0; while (*p && *p != ' ' && j < 63) name[j++] = *p++; name[j] = 0;
                any = 1;
                long n; char *buf = slurp(name, &n);
                if (!buf) { print("strings: no such file: "); print(name); print("\n"); continue; }
                if (fc > 1) { print("==> "); print(name); print(" <==\n"); }   /* header when listing several */
                char run[80]; int rl = 0;
                for (long i = 0; i < n; i++) {
                    char c = buf[i];
                    if (c >= 32 && c < 127) { if (rl < 79) run[rl++] = c; }   /* printable ASCII */
                    else { if (rl >= 4) { run[rl] = 0; print("  "); print(run); print("\n"); } rl = 0; }
                }
                if (rl >= 4) { run[rl] = 0; print("  "); print(run); print("\n"); }   /* trailing run */
                free(buf);
            }
            if (!any) print("usage: strings <file>...\n");
        } else if (startswith(line, "basename ")) {       /* basename PATH -> the last component */
            const char *p = line + 9; while (*p == ' ') p++;
            char path[160]; int pl = 0; while (p[pl] && p[pl] != ' ' && pl < 159) { path[pl] = p[pl]; pl++; } path[pl] = 0;
            while (pl > 1 && path[pl-1] == '/') path[--pl] = 0;        /* strip trailing slashes (keep a lone "/") */
            int last = -1; for (int i = 0; i < pl; i++) if (path[i] == '/') last = i;
            print("  "); print(last >= 0 ? path + last + 1 : path); print("\n");
        } else if (startswith(line, "dirname ")) {        /* dirname PATH -> the directory part */
            const char *p = line + 8; while (*p == ' ') p++;
            char path[160]; int pl = 0; while (p[pl] && p[pl] != ' ' && pl < 159) { path[pl] = p[pl]; pl++; } path[pl] = 0;
            int last = -1; for (int i = 0; i < pl; i++) if (path[i] == '/') last = i;
            if (last < 0) print("  .\n");                              /* no slash -> current dir */
            else if (last == 0) print("  /\n");                        /* "/file" -> "/" */
            else { path[last] = 0; print("  "); print(path); print("\n"); }
        } else if (startswith(line, "paste ")) {          /* paste F1 F2 -> each file's line i, side by side (tab-joined) */
            const char *p = line + 6; while (*p == ' ') p++;
            char f1[64]; int j = 0; while (*p && *p != ' ' && j < 63) f1[j++] = *p++; f1[j] = 0;
            while (*p == ' ') p++;
            char f2[64]; j = 0; while (*p && *p != ' ' && j < 63) f2[j++] = *p++; f2[j] = 0;
            if (!f1[0] || !f2[0]) { print("usage: paste <file1> <file2>\n"); }
            else {
                long n1, n2; char *c1 = slurp(f1, &n1); char *c2 = slurp(f2, &n2);
                if (!c1)      { print("paste: no such file: "); print(f1); print("\n"); }
                else if (!c2) { print("paste: no such file: "); print(f2); print("\n"); }
                else {
                    long i1 = 0, i2 = 0;
                    while (i1 < n1 || i2 < n2) {
                        long s1 = i1; while (i1 < n1 && c1[i1] != '\n') i1++; long e1 = i1; if (i1 < n1) i1++;
                        long s2 = i2; while (i2 < n2 && c2[i2] != '\n') i2++; long e2 = i2; if (i2 < n2) i2++;
                        char t[160]; long k, q = 0;
                        for (k = s1; k < e1 && q < 78; k++) t[q++] = c1[k];   /* file1's line */
                        t[q++] = '\t';
                        for (k = s2; k < e2 && q < 158; k++) t[q++] = c2[k];  /* file2's line */
                        t[q] = 0; print("  "); print(t); print("\n");
                    }
                }
                free(c1); free(c2);
            }
        } else if (startswith(line, "comm ")) {           /* comm F1 F2 (sorted) -> < only-in-1, > only-in-2, = in-both */
            const char *p = line + 5; while (*p == ' ') p++;
            char f1[64]; int j = 0; while (*p && *p != ' ' && j < 63) f1[j++] = *p++; f1[j] = 0;
            while (*p == ' ') p++;
            char f2[64]; j = 0; while (*p && *p != ' ' && j < 63) f2[j++] = *p++; f2[j] = 0;
            if (!f1[0] || !f2[0]) { print("usage: comm <file1> <file2>  (sorted; < only-1, > only-2, = both)\n"); }
            else {
                long n1, n2; char *c1 = slurp(f1, &n1); char *c2 = slurp(f2, &n2);
                if (!c1)      { print("comm: no such file: "); print(f1); print("\n"); }
                else if (!c2) { print("comm: no such file: "); print(f2); print("\n"); }
                else {
                    long i1 = 0, i2 = 0;
                    while (i1 < n1 || i2 < n2) {
                        int haveA = i1 < n1, haveB = i2 < n2;     /* peek each line WITHOUT advancing the cursor */
                        long a_s = i1, a_e = i1; if (haveA) { while (a_e < n1 && c1[a_e] != '\n') a_e++; }
                        long b_s = i2, b_e = i2; if (haveB) { while (b_e < n2 && c2[b_e] != '\n') b_e++; }
                        long a_next = a_e < n1 ? a_e + 1 : a_e, b_next = b_e < n2 ? b_e + 1 : b_e;
                        int rel;                                  /* <0: only-A, >0: only-B, 0: both */
                        if (haveA && !haveB) rel = -1;
                        else if (!haveA && haveB) rel = 1;
                        else { long la = a_e - a_s, lb = b_e - b_s, m = la < lb ? la : lb; rel = 0;
                            for (long k = 0; k < m; k++) if (c1[a_s+k] != c2[b_s+k]) { rel = c1[a_s+k] < c2[b_s+k] ? -1 : 1; break; }
                            if (rel == 0 && la != lb) rel = la < lb ? -1 : 1; }
                        char t[160]; long k, q = 0; long s = rel > 0 ? b_s : a_s, e = rel > 0 ? b_e : a_e; char *src = rel > 0 ? c2 : c1;
                        for (k = s; k < e && q < 157; k++) t[q++] = src[k]; t[q] = 0;
                        print(rel < 0 ? "  < " : rel > 0 ? "  > " : "  = "); print(t); print("\n");
                        if (rel <= 0) i1 = a_next;                /* advance the file(s) whose line was emitted */
                        if (rel >= 0) i2 = b_next;
                    }
                }
                free(c1); free(c2);
            }
        } else if (startswith(line, "diff ")) {           /* diff F1 F2 -> LCS line edit script: '- ' removed from F1, '+ ' added in F2, '  ' unchanged */
            const char *p = line + 5; while (*p == ' ') p++;
            char f1[64]; int j = 0; while (*p && *p != ' ' && j < 63) f1[j++] = *p++; f1[j] = 0;
            while (*p == ' ') p++;
            char f2[64]; j = 0; while (*p && *p != ' ' && j < 63) f2[j++] = *p++; f2[j] = 0;
            if (!f1[0] || !f2[0]) { print("usage: diff <file1> <file2>  (line edit: - removed, + added)\n"); }
            else {
                long n1, n2; char *d1 = slurp(f1, &n1); char *d2 = slurp(f2, &n2);   /* whole files; the LCS still caps lines (warned below) */
                if (!d1) { print("diff: "); print(f1); print(": no such file\n"); }
                else if (!d2) { print("diff: "); print(f2); print(": no such file\n"); }
                else {
                    static int as[129], ae[129], bs[129], be[129];
                    static short L[129][129];                       /* LCS lengths; capped at 128 lines/file */
                    int na = 0, nb = 0, i;
                    for (i = 0; i < n1 && na < 128; na++) { as[na] = i; while (i < n1 && d1[i] != '\n') i++; ae[na] = i; if (i < n1) i++; }
                    for (i = 0; i < n2 && nb < 128; nb++) { bs[nb] = i; while (i < n2 && d2[i] != '\n') i++; be[nb] = i; if (i < n2) i++; }
                    for (int a = na; a >= 0; a--) for (int b = nb; b >= 0; b--) {
                        if (a == na || b == nb) { L[a][b] = 0; continue; }
                        int la = ae[a]-as[a], lb = be[b]-bs[b], eq = (la == lb), k;
                        for (k = 0; eq && k < la; k++) if (d1[as[a]+k] != d2[bs[b]+k]) eq = 0;
                        if (eq) L[a][b] = (short)(L[a+1][b+1] + 1);
                        else    L[a][b] = L[a+1][b] >= L[a][b+1] ? L[a+1][b] : L[a][b+1];
                    }
                    int a = 0, b = 0, diffs = 0;
                    while (a < na || b < nb) {
                        char t[160]; int k, q = 0; int eq = 0;
                        if (a < na && b < nb) {
                            int la = ae[a]-as[a], lb = be[b]-bs[b]; eq = (la == lb);
                            for (k = 0; eq && k < la; k++) if (d1[as[a]+k] != d2[bs[b]+k]) eq = 0;
                        }
                        if (a < na && b < nb && eq) {                              /* unchanged: context */
                            for (k = as[a]; k < ae[a] && q < 157; k++) t[q++] = d1[k]; t[q] = 0;
                            print("  "); print(t); print("\n"); a++; b++;
                        } else if (b >= nb || (a < na && L[a+1][b] >= L[a][b+1])) { /* removed from F1 */
                            for (k = as[a]; k < ae[a] && q < 157; k++) t[q++] = d1[k]; t[q] = 0;
                            print("- "); print(t); print("\n"); a++; diffs++;
                        } else {                                                   /* added in F2 */
                            for (k = bs[b]; k < be[b] && q < 157; k++) t[q++] = d2[k]; t[q] = 0;
                            print("+ "); print(t); print("\n"); b++; diffs++;
                        }
                    }
                    if (!diffs) print("(files are identical)\n");
                    if (na >= 128 || nb >= 128) print("(diff truncated at 128 lines/file)\n");
                }
                free(d1); free(d2);
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
                char *wbuf = malloc(1u << 20);           /* 1MB download buffer (was a fixed 16KB) */
                print(secure ? "downloading https://" : "downloading http://"); print(host); print(path); print(" ...\n");
                long n = !wbuf ? -1 : (secure ? sys_https(host, path, wbuf, (1u << 20) - 1)
                                              : sys_http(host, path, wbuf, (1u << 20) - 1));
                if (n < 0) { print("wget: failed (no net/DNS/connect, or out of memory)\n"); }
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
                free(wbuf);
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
        } else if (startswith(line, "play ")) {            /* play a .wav in the background (non-blocking) */
            char *f = line + 5; while (*f == ' ') f++;
            char fn[64]; int fi = 0; while (*f && *f != ' ' && fi < 63) fn[fi++] = *f++; fn[fi] = 0;
            if (!fn[0]) print("usage: play <file.wav>\n");
            else print(sys_playbg(fn) == 0 ? "play: started in the background ('stop' to halt)\n"
                                           : "play: not a 16-bit PCM WAV (or no file)\n");
        } else if (streq(line, "stop")) {                  /* stop background audio */
            sys_audiostop();
            print("stopped\n");
        } else if (startswith(line, "tone")) {             /* play a tone via AC'97: tone [hz] [ms] */
            const char *p = line + 4; while (*p == ' ') p++;
            int hz = 0; while (*p >= '0' && *p <= '9') hz = hz * 10 + (*p++ - '0');
            while (*p == ' ') p++;
            int ms = 0; while (*p >= '0' && *p <= '9') ms = ms * 10 + (*p++ - '0');
            if (hz <= 0) hz = 440;
            if (ms <= 0) ms = 400;
            if (ms > 5000) ms = 5000;
            int nframes = 48 * ms;                          /* 48000 Hz */
            short *buf = malloc((unsigned long)nframes * 4);
            if (!buf) print("tone: out of memory\n");
            else {
                int period = 48000 / hz; if (period < 2) period = 2;
                for (int i = 0; i < nframes; i++) {         /* a square wave (fundamental = hz) */
                    short s = ((i % period) < period / 2) ? 8000 : -8000;
                    buf[i * 2] = s; buf[i * 2 + 1] = s;
                }
                sys_pcm(buf, nframes);
                free(buf);
                print("tone: played\n");
            }
        } else if (startswith(line, "run ")) {           /* run <prog> [arg] — e.g. `run editor README.TXT` */
            const char *p = line + 4; while (*p == ' ') p++;
            char prog[64]; int pi = 0; while (*p && *p != ' ' && pi < 63) prog[pi++] = *p++; prog[pi] = 0;
            while (*p == ' ') p++;
            char arg[64]; int ai = 0; while (*p && *p != ' ' && ai < 63) arg[ai++] = *p++; arg[ai] = 0;
            long rc = arg[0] ? sys_spawn_arg(prog, arg) : sys_spawn(prog);
            if (rc < 0) print("run: no such program. type 'apps' for the list (or run a disk .elf)\n");
            else { print("launched "); print(prog); print("\n"); }
        } else if (startswith(line, "file ")) {            /* identify a file's type by its magic bytes */
            char *f = line + 5; int any = 0;
            while (*f) {                                 /* identify each space-separated file */
                while (*f == ' ') f++;
                if (!*f) break;
                char fn[64]; int fi = 0; while (*f && *f != ' ' && fi < 63) fn[fi++] = *f++; fn[fi] = 0;
                any = 1;
                static unsigned char b[512];
                long n = sys_readfile(fn, b, sizeof(b));
                if (n < 0) { print(fn); print(": no such file\n"); g_status = 1; }
                else {
                    const char *t;
                    if (n>=8 && b[0]==0x89&&b[1]=='P'&&b[2]=='N'&&b[3]=='G') t = "PNG image";
                    else if (n>=4 && b[0]=='G'&&b[1]=='I'&&b[2]=='F'&&b[3]=='8') t = "GIF image";
                    else if (n>=3 && b[0]==0xFF&&b[1]==0xD8&&b[2]==0xFF) t = "JPEG image";
                    else if (n>=2 && b[0]=='B'&&b[1]=='M') t = "BMP image";
                    else if (n>=4 && b[0]=='<'&&(b[1]|32)=='s'&&(b[2]|32)=='v'&&(b[3]|32)=='g') t = "SVG image";
                    else if (n>=2 && b[0]==0x1F&&b[1]==0x8B) t = "gzip compressed data";
                    else if (n>=4 && b[0]=='P'&&b[1]=='K'&&b[2]==3&&b[3]==4) t = "Zip archive";
                    else if (n>262 && b[257]=='u'&&b[258]=='s'&&b[259]=='t'&&b[260]=='a'&&b[261]=='r') t = "tar archive";
                    else if (n>=4 && b[0]==0x7F&&b[1]=='E'&&b[2]=='L'&&b[3]=='F') t = "ELF executable";
                    else {
                        int txt = 1;             /* printable -> text, else binary data */
                        for (long i = 0; i < n; i++) { unsigned char c = b[i];
                            if (!(c=='\n'||c=='\r'||c=='\t'||(c>=32&&c<127))) { txt = 0; break; } }
                        t = txt ? "ASCII text" : "data";
                    }
                    print(fn); print(": "); print(t); print("\n");
                }
            }
            if (!any) print("usage: file <name>...\n");
        } else if (startswith(line, "tar ")) {             /* extract a .tar / .tar.gz archive */
            char *t = line + 4; while (*t == ' ') t++;
            char tn[64]; int ti = 0; while (*t && *t != ' ' && ti < 63) tn[ti++] = *t++; tn[ti] = 0;
            if (tn[0] == 0) print("usage: tar <file.tar|file.tgz>\n");
            else {
                long n = sys_untar(tn);
                if (n < 0) print("tar: failed (not a tar, too big, or missing)\n");
                else { char nb[12]; itoa_simple((int)n, nb); print("tar: extracted "); print(nb); print(" file(s)\n"); }
            }
        } else if (startswith(line, "unzip ")) {           /* extract a .zip archive (reuses the DEFLATE decoder) */
            char *z = line + 6; while (*z == ' ') z++;
            char zn[64]; int zi = 0; while (*z && *z != ' ' && zi < 63) zn[zi++] = *z++; zn[zi] = 0;
            if (zn[0] == 0) print("usage: unzip <file.zip>\n");
            else {
                long n = sys_unzip(zn);
                if (n < 0) print("unzip: failed (not a .zip, too big, or missing)\n");
                else { char nb[12]; itoa_simple((int)n, nb); print("unzip: extracted "); print(nb); print(" file(s)\n"); }
            }
        } else if (startswith(line, "gzip ")) {            /* compress a file with the from-scratch DEFLATE encoder */
            char src[64]; int i = 0; char *p = line + 5;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && i < 63) src[i++] = *p++;
            src[i] = 0;
            while (*p == ' ') p++;
            char dst[64]; int j = 0;
            if (*p) { while (*p && *p != ' ' && j < 63) dst[j++] = *p++; dst[j] = 0; }
            else {                                          /* no DST: basename (drop ext) + ".GZ" */
                int L = 0; while (src[L]) L++;
                int dot = -1; for (int k = 0; k < L; k++) if (src[k] == '.') dot = k;
                int b = dot >= 0 ? dot : L;
                for (int k = 0; k < b && j < 60; k++) dst[j++] = src[k];
                dst[j++] = '.'; dst[j++] = 'G'; dst[j++] = 'Z'; dst[j] = 0;
            }
            if (src[0] == 0) print("usage: gzip <file> [out.gz]\n");
            else {
                long n = sys_gzip(src, dst);
                if (n < 0) print("gzip: failed (missing or too big)\n");
                else { char nb[12]; itoa_simple((int)n, nb); print("gzip: wrote "); print(dst); print(" ("); print(nb); print(" bytes)\n"); }
            }
        } else if (startswith(line, "gunzip ")) {          /* decompress a .gz file (reuses the DEFLATE decoder) */
            char src[64]; int i = 0; char *p = line + 7;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && i < 63) src[i++] = *p++;
            src[i] = 0;
            while (*p == ' ') p++;
            char dst[64]; int j = 0;
            if (*p) { while (*p && *p != ' ' && j < 63) dst[j++] = *p++; dst[j] = 0; }
            else {                                          /* no DST given: strip a trailing .gz, else "OUT" */
                int L = 0; while (src[L]) L++;
                if (L > 3 && src[L-3]=='.' && (src[L-2]=='g'||src[L-2]=='G') && (src[L-1]=='z'||src[L-1]=='Z')) {
                    for (int k = 0; k < L-3; k++) dst[k] = src[k]; dst[L-3] = 0;
                } else { dst[0]='O'; dst[1]='U'; dst[2]='T'; dst[3]=0; }
            }
            if (src[0] == 0) print("usage: gunzip <file.gz> [out]\n");
            else {
                long n = sys_gunzip(src, dst);
                if (n < 0) print("gunzip: failed (not a .gz, too big, or missing)\n");
                else { char nb[12]; itoa_simple((int)n, nb); print("gunzip: wrote "); print(dst); print(" ("); print(nb); print(" bytes)\n"); }
            }
        } else if ((startswith(line, "cp ") || startswith(line, "mv ")) && nargs(line + 3) > 2) {
            /* multi-file: cp/mv SRC... DESTDIR  (e.g. `cp *.txt backup`). The 2-arg
             * form is handled by the next branch, untouched. The last token is the
             * destination, which must be an existing directory; mv deletes each
             * source only after its copy succeeds. */
            int move = (line[0] == 'm');
            char tbuf[256]; int tn = 0; char *av[16]; int ac = 0;
            const char *q = line + 3;
            while (*q && ac < 16) {
                while (*q == ' ') q++;
                if (!*q) break;
                av[ac++] = tbuf + tn;
                while (*q && *q != ' ' && tn < 255) tbuf[tn++] = *q++;
                tbuf[tn++] = 0;
            }
            const char *dest = av[ac - 1];
            int destdir = 0;
            if (sys_chdir(dest) >= 0) { destdir = 1; sys_chdir(cwd); }
            if (!destdir) { print(move?"mv":"cp"); print(": target is not a directory: "); print(dest); print("\n"); g_status = 1; }
            else for (int s = 0; s < ac - 1; s++) {
                const char *srcf = av[s];
                long n; char *buf = slurp(srcf, &n);
                if (!buf) { print(move?"mv":"cp"); print(": no such file: "); print(srcf); print("\n"); continue; }
                const char *base = srcf; for (const char *t = srcf; *t; t++) if (*t == '/') base = t + 1;
                char dpath[160]; int d = 0; for (const char *t = dest; *t && d < 158; t++) dpath[d++] = *t;
                if (d > 0 && dpath[d-1] != '/' && d < 158) dpath[d++] = '/';
                for (const char *t = base; *t && d < 159; t++) dpath[d++] = *t;
                dpath[d] = 0;
                if (sys_writefile(dpath, buf, (unsigned long)n) < 0) { print(move?"mv":"cp"); print(": write failed: "); print(dpath); print("\n"); g_status = 1; }
                else { if (move) sys_delete(srcf); print(move?"moved ":"copied "); print(srcf); print(" -> "); print(dpath); print("\n"); }
                free(buf);
            }
        } else if (startswith(line, "cp ") || startswith(line, "mv ")) {
            int move = (line[0] == 'm');
            char src[64]; int i = 0; char *p = line + 3;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && i < 63) src[i++] = *p++;
            src[i] = 0;
            while (*p == ' ') p++;
            if (src[0] == 0 || *p == 0) { print("usage: "); print(move?"mv":"cp"); print(" <src> <dst>\n"); }
            else if (sys_chdir(src) >= 0) {        /* src is a directory: cp/mv of dirs isn't supported (no -r) */
                sys_chdir(cwd);
                print(move ? "mv" : "cp"); print(": "); print(src); print(" is a directory\n"); g_status = 1;
            }
            else {
                /* file size is unknown (no stat syscall): read into a heap buffer,
                 * doubling it until the read no longer fills it. Fixes the old
                 * fixed 4KB buffer that silently truncated files >4KB (and, for
                 * mv, deleted the source after a short copy — data loss). */
                unsigned long cap = 65536;
                char *buf = malloc(cap);
                long n = buf ? sys_readfile(src, buf, cap) : -1;
                while (buf && n == (long)cap && cap < (32UL << 20)) {  /* read filled the buffer: file may be larger */
                    cap <<= 1; free(buf); buf = malloc(cap);
                    if (buf) n = sys_readfile(src, buf, cap);
                }
                if (!buf)                print("cp/mv: file too large (out of memory)\n");
                else if (n < 0)          print("no such file\n");
                else if (n == (long)cap) print("cp/mv: file too large to copy\n");
                else {
                    /* If the destination is a directory, copy into it as dir/basename(src). */
                    char dst[128]; int isdir = 0;
                    if (sys_chdir(p) >= 0) { isdir = 1; sys_chdir(cwd); }
                    if (isdir) {
                        const char *base = src; for (const char *s = src; *s; s++) if (*s == '/') base = s + 1;
                        int d = 0; for (const char *s = p; *s && d < 126; s++) dst[d++] = *s;
                        if (d > 0 && dst[d-1] != '/' && d < 126) dst[d++] = '/';
                        for (const char *s = base; *s && d < 127; s++) dst[d++] = *s;
                        dst[d] = 0;
                    } else { int d = 0; for (const char *s = p; *s && d < 127; s++) dst[d++] = *s; dst[d] = 0; }
                    if (sys_writefile(dst, buf, (unsigned long)n) < 0) print("write failed\n");
                    else {
                        if (move && !streq(src, dst)) sys_delete(src);   /* never delete when src==dst */
                        print(move ? "moved " : "copied "); print(src);
                        print(" -> "); print(dst); print("\n");
                    }
                }
                free(buf);
            }
        } else if (startswith(line, "rm ")) {
            const char *p = line + 3; int any = 0;       /* remove each space-separated file (so `rm *.tmp` / `rm a b` work) */
            while (*p) {
                while (*p == ' ') p++;
                if (!*p) break;
                char name[64]; int j = 0;
                while (*p && *p != ' ' && j < 63) name[j++] = *p++;
                name[j] = 0; any = 1;
                if (sys_delete(name) < 0) { print("rm: no such file, or dir not empty: "); print(name); print("\n"); g_status = 1; }
                else { print("removed "); print(name); print("\n"); }
            }
            if (!any) print("usage: rm <file>...\n");
        } else if (startswith(line, "touch ")) {
            const char *p = line + 6; int any = 0;       /* touch each space-separated file */
            while (*p) {
                while (*p == ' ') p++;
                if (!*p) break;
                char name[64]; int j = 0;
                while (*p && *p != ' ' && j < 63) name[j++] = *p++;
                name[j] = 0; any = 1;
                char probe[1];
                if (sys_readfile(name, probe, 1) >= 0) { }      /* exists: leave content (no mtime API) */
                else if (sys_writefile(name, "", 0) < 0) { print("touch: failed: "); print(name); print("\n"); g_status = 1; }
                else { print("created "); print(name); print("\n"); }
            }
            if (!any) print("usage: touch <file>...\n");
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
            return 1;                  /* signal main()'s loop to stop */
        } else {
            print("unknown command: ");
            print(line);
            print("  (try 'help')\n");
            g_status = 1;
        }
    } while (0);
    return 0;
}

/*
 * Run a pipeline: split `line` on the first '|' into two commands and feed the
 * first command's captured output to the second as a trailing file argument.
 *
 * Model: capture cmd1's output into a buffer (cap_begin/cap_end), write it to
 * a temp file, then run `cmd2 PIPE.TMP`. This needs no per-command stdin
 * support — the file-reading commands (grep/wc/sort/head/tail/cat/nl/...) read
 * their last argument via sys_readfile, which now reads the piped data.
 *
 * Supports N stages by looping: each stage after the first reads PIPE.TMP and
 * its output replaces it for the next '|'. The capture buffer grows on demand
 * (cap_begin/cap_end over a heap buffer, up to 32MB), so a stage's output is no
 * longer truncated at a fixed size.
 */
/* Write a command's captured output to a file: overwrite for ">", append for
 * ">>" (append reads the existing contents first, capped to the buffer). */
static void write_redirect(const char *file, const char *buf, unsigned long len, int append) {
    if (append) {
        long oldlen; char *old = slurp(file, &oldlen);     /* read the WHOLE existing file (was capped at 8KB) */
        unsigned long ol = (old && oldlen > 0) ? (unsigned long)oldlen : 0;
        char *combined = malloc(ol + len + 1);
        if (combined) {
            for (unsigned long i = 0; i < ol;  i++) combined[i]      = old[i];
            for (unsigned long i = 0; i < len; i++) combined[ol + i] = buf[i];
            sys_writefile(file, combined, ol + len);       /* existing + appended */
            free(combined);
        }
        free(old);                                         /* free(NULL) is safe */
    } else {
        sys_writefile(file, buf, len);
    }
}

static void run_pipe(char *line, char *cwd, const char *rfile, int append) {
    char cmd[160];                     /* the current stage's command line (cmd2 + " PIPE.TMP") */
    const char *seg = line;            /* start of the current segment */
    int first = 1;

    for (;;) {
        /* find the end of this segment: next unescaped '|' or end of string */
        const char *bar = seg;
        while (*bar && *bar != '|') bar++;

        /* copy the segment into cmd[], trimming leading/trailing spaces */
        const char *s = seg;
        while (*s == ' ') s++;                 /* skip leading spaces */
        const char *e = bar;
        while (e > s && (e[-1] == ' ')) e--;    /* trim trailing spaces */
        int ci = 0;
        while (s < e && ci < 158) cmd[ci++] = *s++;
        cmd[ci] = 0;

        if (!first && ci > 0) {                /* append the piped-in temp file as the last argument */
            const char *suf = " PIPE.TMP";
            for (int i = 0; suf[i] && ci < 159; i++) cmd[ci++] = suf[i];
            cmd[ci] = 0;
        }

        int last = (*bar != '|');              /* this is the final stage */

        if (ci == 0) {
            /* empty stage (e.g. "ls |" or "| grep"): nothing to run.
             * For the last stage, fall through so PIPE.TMP gets cleaned up. */
        } else if (last) {
            if (rfile) {                       /* final stage redirected to a file */
                cap_begin();
                run_command(cmd, cwd);
                unsigned long rlen; char *cb = cap_end(&rlen);
                if (cb) { write_redirect(rfile, cb, rlen, append); free(cb); }
            } else {
                run_command(cmd, cwd);         /* final stage: print to the screen */
            }
        } else {
            cap_begin();                       /* intermediate stage: capture its output */
            run_command(cmd, cwd);
            unsigned long len; char *cb = cap_end(&len);
            if (cb) { sys_writefile("PIPE.TMP", cb, len); free(cb); }
        }

        if (last) break;
        seg = bar + 1;
        first = 0;
    }

    sys_delete("PIPE.TMP");                     /* best-effort cleanup of the scratch file */
}

/* glob_match() (filename '*'/'?' globbing, iterative star/backtrack — no
 * catastrophic recursion) now lives in shgrep.h alongside gr_match, host-tested
 * by tests/shgrep.
 * Expand any '*'/'?' token in `src` against the current directory into `dst`
 * (other tokens pass through verbatim); a pattern with no match is left literal,
 * as a shell does. Output is length-bounded by dstsz (a huge match set truncates
 * rather than overflowing). Reuses sys_list — the same listing `ls` prints, one
 * "NAME size" per line — so the first whitespace-delimited field is the name. */
static void glob_expand(const char *src, char *dst, int dstsz){
    static char listing[8192];   /* hold a full directory so globs match every file */
    long ln = sys_list(listing, sizeof listing - 1); if (ln < 0) ln = 0; listing[ln] = 0;
    int dp = 0;
    const char *p = src;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *ts = p; while (*p && *p != ' ') p++; int tl = (int)(p - ts);
        int isglob = 0; for (int i = 0; i < tl; i++) if (ts[i] == '*' || ts[i] == '?') { isglob = 1; break; }
        int appended = 0;
        if (isglob) {
            char pat[64]; int pl = tl < 63 ? tl : 63; for (int i = 0; i < pl; i++) pat[i] = ts[i]; pat[pl] = 0;
            const char *q = listing;
            while (*q) {
                char name[64]; int ni = 0;
                while (*q && *q != ' ' && *q != '\n' && *q != '\r' && ni < 63) name[ni++] = *q++;
                name[ni] = 0;
                while (*q && *q != '\n') q++; if (*q) q++;          /* advance to the next line */
                if (ni && name[ni-1] != '/' && glob_match(pat, name)) {   /* skip dir entries (shown as NAME/) */
                    for (int i = 0; i < ni && dp < dstsz - 1; i++) dst[dp++] = name[i];
                    if (dp < dstsz - 1) dst[dp++] = ' ';
                    appended = 1;
                }
            }
        }
        if (!appended) {                                   /* non-glob token, or a glob with no match: keep literal */
            for (int i = 0; i < tl && dp < dstsz - 1; i++) dst[dp++] = ts[i];
            if (dp < dstsz - 1) dst[dp++] = ' ';
        }
    }
    if (dp > 0 && dst[dp-1] == ' ') dp--;                  /* trim the trailing separator */
    dst[dp] = 0;
}

/* Command substitution: replace each $(cmd) in `src` with the command's output
 * (trailing newlines stripped, internal newlines -> spaces for word splitting),
 * writing to `dst`. $((..)) arithmetic is left alone. Substitutions nest — e.g.
 * `$(echo $(echo x))` — up to a recursion cap; cap_begin/cap_end stack the output
 * buffers. Returns 1 if any substitution happened.
 * Shared by run_line (whole command) and run_for (the word list). */
static int in_cmdsub;
static int cmdsub_expand(const char *src, char *dst, int dstsz, char *cwd) {
    if (in_cmdsub >= 8) return 0;          /* runaway-recursion guard (CAP_STACK has the headroom) */
    int has = 0;
    for (int i = 0; src[i]; i++) if (src[i]=='$' && src[i+1]=='(' && src[i+2]!='(') { has = 1; break; }
    if (!has) return 0;
    int o = 0;
    for (int i = 0; src[i] && o < dstsz-1; ) {
        if (src[i]=='$' && src[i+1]=='(' && src[i+2]!='(') {
            int depth = 1, j = i + 2, s = j;
            while (src[j] && depth) { if (src[j]=='(') depth++; else if (src[j]==')') { depth--; if (!depth) break; } j++; }
            char inner[1024]; int ii = 0; for (int k = s; k < j && ii < 1023; k++) inner[ii++] = src[k]; inner[ii] = 0;
            in_cmdsub++; cap_begin(); run_input_line(inner, cwd);   /* run_input_line (not run_andor) so a `;` list and for/while/if work inside $() */
            unsigned long clen; char *cb = cap_end(&clen); in_cmdsub--;
            if (cb) {
                while (clen > 0 && cb[clen-1] == '\n') clen--;
                for (unsigned long k = 0; k < clen && o < dstsz-1; k++) dst[o++] = cb[k]=='\n' ? ' ' : cb[k];
                free(cb);
            }
            i = (src[j]==')') ? j + 1 : j;
        } else dst[o++] = src[i++];
    }
    dst[o] = 0;
    return 1;
}

/* Run one command line (a single ';'-separated segment): expand filename globs,
 * then peel off output redirection and pipelines, then dispatch. Returns 1 only
 * when the shell should exit (the "exit" command). */
static int run_line(char *line, char *cwd) {
    static char gline[1024], vline[1024], aline[1024];
    /* cmdsub_expand recurses back through run_line (a nested $(...)), so its dst
     * can't be one shared static buffer — the inner call would clobber the outer
     * one mid-build. Index by the cmdsub depth (in_cmdsub) instead. gline/vline/
     * aline stay shared: run_line touches them only after cmdsub_expand returns. */
    static char subline[9][1024];
    int sbi = in_cmdsub < 9 ? in_cmdsub : 8;
    char *cmd = line;
    if (cmdsub_expand(cmd, subline[sbi], sizeof subline[0], cwd)) cmd = subline[sbi];   /* $(...) command substitution */
    if (expand_vars(cmd, vline, sizeof vline)) cmd = vline;   /* $NAME / ${NAME} variable expansion (before glob/pipe/redirect) */
    /* alias expansion: if the first word is an alias, substitute its value
     * (one level only, so a -> b -> a can't loop). */
    { int wl = 0; while (cmd[wl] && cmd[wl] != ' ') wl++;
      const char *av = wl ? alias_get(cmd, wl) : 0;
      if (av) { int o = 0;
          for (int i = 0; av[i] && o < 1023; i++) aline[o++] = av[i];
          for (int i = wl; cmd[i] && o < 1023; i++) aline[o++] = cmd[i];
          aline[o] = 0; cmd = aline; } }
    for (int i = 0; cmd[i]; i++) if (cmd[i] == '*' || cmd[i] == '?') {
        glob_expand(cmd, gline, sizeof gline); cmd = gline; break;
    }

    const char *rfile = 0; int append = 0;
    for (int i = 0; cmd[i]; i++) if (cmd[i] == '>') {
        if (cmd[i+1] == '>') { append = 1; rfile = &cmd[i+2]; } else { rfile = &cmd[i+1]; }
        cmd[i] = 0;
        while (i > 0 && cmd[i-1] == ' ') cmd[--i] = 0;   /* trim spaces before '>' so "cmd arg > f" doesn't pass "arg " (trailing space) on */
        while (*rfile == ' ') rfile++;
        char *fe = (char *)rfile; while (*fe) fe++; while (fe > rfile && fe[-1] == ' ') *--fe = 0;
        if (!*rfile) rfile = 0;
        break;
    }

    /* Input redirect `cmd < file`: our commands read a file argument, so rewrite
     * it to `cmd file` (append the input file as a trailing arg). The result is
     * never longer than the original, so it rewrites cmd[] in place safely. */
    for (int i = 0; cmd[i]; i++) if (cmd[i] == '<') {
        char *infile = &cmd[i+1]; while (*infile == ' ') infile++;
        char fcopy[160]; int fc = 0; while (infile[fc] && infile[fc] != ' ' && fc < 159) { fcopy[fc] = infile[fc]; fc++; } fcopy[fc] = 0;
        cmd[i] = 0;
        while (i > 0 && cmd[i-1] == ' ') cmd[--i] = 0;   /* trim spaces before '<' */
        if (fcopy[0]) { int e = i; if (e < 1022) cmd[e++] = ' '; for (int k = 0; fcopy[k] && e < 1022; k++) cmd[e++] = fcopy[k]; cmd[e] = 0; }
        break;
    }

    int piped = 0;
    for (int i = 0; cmd[i]; i++) if (cmd[i] == '|') { piped = 1; break; }

    if (piped) { run_pipe(cmd, cwd, rfile, append); return 0; }
    if (rfile) {
        cap_begin();
        run_command(cmd, cwd);
        unsigned long rlen; char *cb = cap_end(&rlen);
        if (cb) { write_redirect(rfile, cb, rlen, append); free(cb); }
        return 0;
    }
    return run_command(cmd, cwd);                /* 1 only for "exit" */
}

/* Run a ';'-separated segment that may contain && / || operators, left to
 * right, honouring each command's exit status ($?: 0 = success). A single |
 * (pipe) or & is left intact for run_line — only the doubled forms are operators
 * here, and they're matched on the raw text so arithmetic like $((a & b)) is
 * untouched. Returns 1 if a command was the "exit" builtin. */
static int run_andor(char *seg, char *cwd) {
    char *p = seg;
    int run_this = 1, exitflag = 0;
    while (p) {
        char *op = p; int oplen = 0, pd = 0;
        while (*op) {
            if (op[0] == '$' && op[1] == '(') { pd++; op += 2; continue; }   /* enter $( or $(( : its &&/|| are arithmetic/command-sub, not ours */
            if (pd > 0) { if (*op == '(') pd++; else if (*op == ')') pd--; op++; continue; }
            if ((op[0] == '&' && op[1] == '&') || (op[0] == '|' && op[1] == '|')) { oplen = 2; break; }
            op++;
        }
        char opc = oplen ? op[0] : 0;
        if (oplen) *op = 0;                            /* terminate this command */
        while (*p == ' ') p++;                         /* trim leading spaces */
        char *e = p; while (*e) e++;                   /* ...and trailing ones, so */
        while (e > p && e[-1] == ' ') *--e = 0;        /* "true && .." matches streq("true") */
        if (run_this && *p && run_line(p, cwd)) exitflag = 1;
        if      (opc == '&') run_this = (g_status == 0);   /* &&: next runs only on success */
        else if (opc == '|') run_this = (g_status != 0);   /* ||: next runs only on failure */
        p = oplen ? op + oplen : 0;
    }
    return exitflag;
}

/* for VAR in WORDS; do BODY; done  (one line). WORDS get $var + glob expansion,
 * then BODY runs once per word with VAR bound to it. Buffers are on the stack so
 * nested loops don't clobber each other. */
static int run_for(char *line, char *cwd) {
    char *p = line + 3; while (*p == ' ') p++;             /* skip "for" */
    char var[32]; int vi = 0;
    while (*p && *p != ' ' && vi < 31) var[vi++] = *p++;
    var[vi] = 0;
    while (*p == ' ') p++;
    if (!(p[0]=='i' && p[1]=='n' && (p[2]==' '||p[2]==0))) { print("for: syntax: for V in WORDS; do CMDS; done\n"); g_status=1; return 0; }
    p += 2; while (*p == ' ') p++;
    char *list = p, *semi = p; while (*semi && *semi != ';') semi++;
    if (*semi != ';') { print("for: missing ';' before do\n"); g_status=1; return 0; }
    *semi = 0;
    char *q = semi + 1; while (*q == ' ') q++;
    if (!(q[0]=='d' && q[1]=='o' && (q[2]==' '||q[2]==0))) { print("for: missing 'do'\n"); g_status=1; return 0; }
    q += 2; while (*q == ' ') q++;
    char *body = q;
    int blen = (int)ustrlen(body);
    while (blen > 0 && body[blen-1]==' ') body[--blen]=0;
    if (!(blen >= 4 && streq(body+blen-4, "done"))) { print("for: missing 'done'\n"); g_status=1; return 0; }
    blen -= 4; while (blen>0 && body[blen-1]==' ') blen--;   /* drop "done" + a trailing "; " */
    if (blen>0 && body[blen-1]==';') blen--;
    while (blen>0 && body[blen-1]==' ') blen--;
    body[blen] = 0;
    char elist[1024], glist[1024], bodybuf[1024], sublist[1024];
    char *lst = list;
    if (cmdsub_expand(lst, sublist, sizeof sublist, cwd)) lst = sublist;   /* for f in $(cmd) */
    if (expand_vars(lst, elist, sizeof elist)) lst = elist;
    for (int i=0; lst[i]; i++) if (lst[i]=='*' || lst[i]=='?') { glob_expand(lst, glist, sizeof glist); lst = glist; break; }
    int doexit = 0;
    char *w = lst;
    while (*w && !doexit) {
        while (*w == ' ') w++;
        if (!*w) break;
        char *we = w; while (*we && *we != ' ') we++;
        char wsave = *we; *we = 0;
        vset(var, vi, w);                                   /* bind the loop variable */
        *we = wsave;
        int bi = 0; for (const char *b = body; *b && bi < 1023; b++) bodybuf[bi++] = *b; bodybuf[bi] = 0;
        if (run_input_line(bodybuf, cwd)) doexit = 1;       /* fresh copy: run_input_line edits it in place */
        w = we;
    }
    return doexit;
}

/* First occurrence of needle in haystack (for the if/then/else markers). */
static char *sh_substr(char *h, const char *n) {
    for (; *h; h++) { int i = 0; while (n[i] && h[i] == n[i]) i++; if (!n[i]) return h; }
    return 0;
}

/* if COND; then THEN; [else ELSE;] fi  (one line). COND's exit status ($?)
 * picks the branch. THEN/ELSE may be ';'-separated lists (and nest), but a
 * nested if/else *inside* THEN on the same line can mis-bind its else — keep
 * deep nesting on separate lines (via source) or use && / ||. */
static int run_if(char *line, char *cwd) {
    char *p = line + 2; while (*p == ' ') p++;             /* skip "if" */
    char *thn = sh_substr(p, "; then");
    if (!thn) { print("if: missing '; then'\n"); g_status = 1; return 0; }
    *thn = 0;                                              /* COND = p */
    char *body = thn + 6; while (*body == ' ') body++;
    int blen = (int)ustrlen(body);
    while (blen > 0 && body[blen-1] == ' ') body[--blen] = 0;
    if (!(blen >= 2 && streq(body+blen-2, "fi"))) { print("if: missing 'fi'\n"); g_status = 1; return 0; }
    blen -= 2; while (blen>0 && body[blen-1]==' ') blen--;   /* drop "fi" + a trailing "; " */
    if (blen>0 && body[blen-1]==';') blen--;
    while (blen>0 && body[blen-1]==' ') blen--;
    body[blen] = 0;
    char *els = sh_substr(body, "; else");
    char *thenb = body, *elseb = 0;
    if (els) { *els = 0; elseb = els + 6; while (*elseb == ' ') elseb++; }
    run_input_line(p, cwd);                                /* run COND -> sets g_status */
    if (g_status == 0) return run_input_line(thenb, cwd);
    if (elseb)         return run_input_line(elseb, cwd);
    return 0;
}

/* while COND; do BODY; done  (one line). Re-runs COND each pass and loops while
 * it succeeds ($? == 0). Bounded at 100000 iterations and interruptible with
 * Ctrl-C / Esc so a runaway loop can't hang the shell. */
static int run_while(char *line, char *cwd) {
    char *p = line + 5; while (*p == ' ') p++;             /* skip "while" */
    char *cond = p, *semi = p; while (*semi && *semi != ';') semi++;
    if (*semi != ';') { print("while: missing ';' before do\n"); g_status = 1; return 0; }
    *semi = 0;
    char *q = semi + 1; while (*q == ' ') q++;
    if (!(q[0]=='d' && q[1]=='o' && (q[2]==' '||q[2]==0))) { print("while: missing 'do'\n"); g_status = 1; return 0; }
    q += 2; while (*q == ' ') q++;
    char *body = q;
    int blen = (int)ustrlen(body);
    while (blen > 0 && body[blen-1]==' ') body[--blen]=0;
    if (!(blen >= 4 && streq(body+blen-4, "done"))) { print("while: missing 'done'\n"); g_status = 1; return 0; }
    blen -= 4; while (blen>0 && body[blen-1]==' ') blen--;
    if (blen>0 && body[blen-1]==';') blen--;
    while (blen>0 && body[blen-1]==' ') blen--;
    body[blen] = 0;
    int doexit = 0, iters = 0;
    char condbuf[1024], bodybuf[1024];
    while (!doexit) {
        if (iters >= 100000) { print("\nwhile: stopped at 100000 iterations\n"); break; }
        int k = sys_pollkey(); if (k == 0x83 || k == 27) { print("\n^C\n"); break; }   /* Ctrl-C / Esc */
        int ci = 0; for (const char *c = cond; *c && ci < 1023; c++) condbuf[ci++] = *c; condbuf[ci] = 0;
        run_input_line(condbuf, cwd);
        if (g_status != 0) break;                          /* COND false -> stop */
        int bi = 0; for (const char *c = body; *c && bi < 1023; c++) bodybuf[bi++] = *c; bodybuf[bi] = 0;
        if (run_input_line(bodybuf, cwd)) doexit = 1;
        iters++;
    }
    return doexit;
}

/* Run one logical input line: a `for`/`while` loop or `if`, else a ';'-separated
 * list of && / || pipelines. Returns 1 if it ran the `exit` builtin. */
static int run_input_line(char *line, char *cwd) {
    char *t = line; while (*t == ' ') t++;
    if (startswith(t, "js -e ")) { run_js_inline(t + 6); g_status = 0; return 0; }   /* literal code (its >|&; are JS, not shell) */
    if (startswith(t, "for "))   return run_for(t, cwd);
    if (startswith(t, "while ")) return run_while(t, cwd);
    if (startswith(t, "if "))    return run_if(t, cwd);
    char *seg = line; int doexit = 0;
    while (seg && !doexit) {
        char *semi = seg; int pd = 0;                  /* find the next top-level ';', skipping $( ... ) / $(( ... )) */
        while (*semi) {
            if (semi[0] == '$' && semi[1] == '(') { pd++; semi += 2; continue; }
            if (pd > 0) { if (*semi == '(') pd++; else if (*semi == ')') pd--; semi++; continue; }
            if (*semi == ';') break;
            semi++;
        }
        int more = (*semi == ';'); if (more) *semi = 0;
        while (*seg == ' ') seg++;
        if (*seg && run_andor(seg, cwd)) doexit = 1;
        seg = more ? semi + 1 : 0;
    }
    return doexit;
}

/* Run shell commands from a file: each non-blank, non-'#' line goes through the
 * same executor as interactive input. `silent` suppresses the not-found message
 * (used for the optional startup .shrc). */
static void source_file(const char *fn, char *cwd, int silent) {
    if (source_depth >= 8) { if (!silent) print("source: nested too deep\n"); g_status = 1; return; }
    long n; char *txt = slurp(fn, &n);
    if (!txt) { if (!silent) { print("source: no such file: "); print(fn); print("\n"); g_status = 1; } return; }
    source_depth++;
    char *ln = txt;
    while (ln && *ln) {
        char *nl = ln; while (*nl && *nl != '\n') nl++;
        int more = (*nl == '\n'); if (more) *nl = 0;
        char *t = ln; while (*t == ' ' || *t == '\t') t++;
        if (*t && *t != '#') run_input_line(t, cwd);   /* skip blanks + # comments */
        ln = more ? nl + 1 : 0;
    }
    source_depth--;
    free(txt);
}

int main(void) {
    print("\n");
    print("  OS-DEV shell v0.1 - running in userspace (ring 3)\n");
    print("  type 'help' for commands\n\n");

    char line[1024];                               /* command line: roomy enough for long URLs + pastes */
    char cwd[128]; cwd[0] = '/'; cwd[1] = 0;       /* display path (kernel tracks the real cwd) */
    char lastcmd[1024]; lastcmd[0] = 0;            /* previous command, for `!!` */
    source_file(".SHRC", cwd, 1); g_status = 0;    /* run the startup rc file if it exists (aliases, set, banner) */
    for (;;) {
        print("osdev:"); print(cwd); print("$ ");
        readline(line, sizeof(line));

        /* `!!` (optionally followed by more, e.g. `!! | grep x`) re-runs the
         * previous command with the trailing text appended; echo the expansion. */
        if (line[0] == '!' && line[1] == '!') {
            if (!lastcmd[0]) { print("!!: no previous command\n"); continue; }
            char exp[1024]; int p = 0;
            for (int i = 0; lastcmd[i] && p < 1023; i++) exp[p++] = lastcmd[i];
            for (int i = 2; line[i] && p < 1023; i++) exp[p++] = line[i];
            exp[p] = 0;
            int i = 0; for (; exp[i] && i < 1023; i++) line[i] = exp[i]; line[i] = 0;
            print(line); print("\n");
        }
        /* remember this command (non-blank) so the next `!!` can repeat it */
        { int ne = 0; for (int i = 0; line[i]; i++) if (line[i] != ' ') { ne = 1; break; }
          if (ne) { int i = 0; for (; line[i] && i < 1023; i++) lastcmd[i] = line[i]; lastcmd[i] = 0; } }

        /* run the line: a `for` loop, or a ';'-separated list of && / || pipelines */
        if (run_input_line(line, cwd)) break;
    }
    return 0;
}
