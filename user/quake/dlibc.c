/*
 * dlibc.c — the libc shim that lets the vendored quakegeneric (Quake) sources
 * build and link against this OS's tiny userspace runtime (ulib + umalloc +
 * syscalls).  Derived from the DOOM port's shim (user/doom/dlibc.c) and extended
 * for what Quake additionally needs: fscanf (savegame/config/particle loaders),
 * and a fuller math set.  setjmp/longjmp live in setjmp.S (Host_Error recovery).
 *
 * It implements the subset of <stdio.h> <stdlib.h> <string.h> <strings.h>
 * <ctype.h> <math.h> that the sources actually reference.  The malloc family and
 * the mem* primitives are NOT redefined here — they live in ulib.c/umalloc.c and
 * are merely declared in the shim headers.
 *
 * The interesting part is file I/O.  Quake opens files by path ("id1/pak0.pak",
 * "config.cfg", savegames) with fopen(path,"rb"/"wb") and then
 * fread/fseek/ftell/fscanf against them; our filesystem is FLAT uppercase-8.3,
 * so fopen reduces a path to its BASENAME and UPPERCASEs it (so "id1/pak0.pak"
 * -> "PAK0.PAK") before sys_readfile.  A read-mode FILE slurps the whole file
 * into a malloc'd buffer up front (grow-and-retry, since PAK0.PAK is ~18 MB).
 * Write-mode FILEs accumulate into a growable buffer flushed to sys_writefile on
 * fflush/fclose (config/savegames).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>

/* ---- functions provided by ulib (do NOT redefine) ---- */
extern void  print(const char *s);
extern long  sys_write(int fd, const void *buf, unsigned long len);
extern long  sys_readfile(const char *name, void *buf, unsigned long max);
extern long  sys_writefile(const char *name, const void *buf, unsigned long len);
extern void  sys_exit(int code);
extern void *malloc(unsigned long n);
extern void  free(void *p);
extern void *realloc(void *p, unsigned long n);
extern void *memset(void *d, int c, unsigned long n);
extern void *memcpy(void *d, const void *s, unsigned long n);
extern void *memmove(void *d, const void *s, unsigned long n);

/* ============================ errno =================================== */
int errno = 0;

/* ============================ ctype =================================== */
int isalpha(int c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isspace(int c)  { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int iscntrl(int c)  { return (c >= 0 && c < 0x20) || c == 0x7f; }
int isprint(int c)  { return c >= 0x20 && c < 0x7f; }
int isgraph(int c)  { return c > 0x20 && c < 0x7f; }
int ispunct(int c)  { return isgraph(c) && !isalnum(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int toupper(int c)  { return islower(c) ? c - 'a' + 'A' : c; }
int tolower(int c)  { return isupper(c) ? c - 'A' + 'a' : c; }

/* ============================ string ================================== */
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }

size_t strnlen(const char *s, size_t maxlen)
{
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst + strlen(dst);
    while ((*d++ = *src++)) {}
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    size_t i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    d[i] = '\0';
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strcasecmp(const char *a, const char *b)
{
    int ca, cb;
    do {
        ca = tolower((unsigned char)*a++);
        cb = tolower((unsigned char)*b++);
    } while (ca && ca == cb);
    return ca - cb;
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    int ca = 0, cb = 0;
    while (n--) {
        ca = tolower((unsigned char)*a++);
        cb = tolower((unsigned char)*b++);
        if (ca != cb || ca == 0) return ca - cb;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (*s == '\0') return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (*s == '\0') return (char *)last;
    }
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *strndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *p = (char *)malloc(len + 1);
    if (p) { memcpy(p, s, len); p[len] = '\0'; }
    return p;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return (int)*x - (int)*y;
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p;
    return NULL;
}

char *strerror(int errnum)
{
    (void)errnum;
    return (char *)"error";
}

/* ============================ stdlib ================================== */
int abs(int x)   { return x < 0 ? -x : x; }
long labs(long x){ return x < 0 ? -x : x; }

int atoi(const char *s)        { return (int)strtol(s, NULL, 10); }
long atol(const char *s)       { return strtol(s, NULL, 10); }

long strtol(const char *s, char **endptr, int base)
{
    const char *p = s;
    long sign = 1, val = 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+') p++;
    else if (*p == '-') { sign = -1; p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2; base = 16;
    } else if (base == 0 && p[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    for (;;) {
        int c = (unsigned char)*p, d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        val = val * base + d;
        p++;
    }
    if (endptr) *endptr = (char *)p;
    return sign * val;
}

unsigned long strtoul(const char *s, char **endptr, int base)
{
    return (unsigned long)strtol(s, endptr, base);
}

double atof(const char *s) { return strtod(s, NULL); }

double strtod(const char *s, char **endptr)
{
    const char *p = s;
    double sign = 1.0, val = 0.0;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+') p++;
    else if (*p == '-') { sign = -1.0; p++; }
    while (*p >= '0' && *p <= '9') { val = val * 10.0 + (*p - '0'); p++; }
    if (*p == '.') {
        double frac = 0.1;
        p++;
        while (*p >= '0' && *p <= '9') { val += (*p - '0') * frac; frac *= 0.1; p++; }
    }
    if (*p == 'e' || *p == 'E') {
        int esign = 1, exp = 0;
        p++;
        if (*p == '+') p++;
        else if (*p == '-') { esign = -1; p++; }
        while (*p >= '0' && *p <= '9') { exp = exp * 10 + (*p - '0'); p++; }
        double scale = 1.0;
        while (exp--) scale *= 10.0;
        if (esign < 0) val /= scale; else val *= scale;
    }
    if (endptr) *endptr = (char *)p;
    return sign * val;
}

/* qsort: simple insertion sort (DOOM sorts only tiny arrays, and our sources
 * don't actually call it — provided for completeness/link safety). */
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    unsigned char *a = (unsigned char *)base;
    unsigned char *tmp = (unsigned char *)malloc(size);
    if (!tmp) return;
    for (size_t i = 1; i < nmemb; i++) {
        memcpy(tmp, a + i * size, size);
        size_t j = i;
        while (j > 0 && compar(a + (j - 1) * size, tmp) > 0) {
            memcpy(a + j * size, a + (j - 1) * size, size);
            j--;
        }
        memcpy(a + j * size, tmp, size);
    }
    free(tmp);
}

void *bsearch(const void *key, void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *))
{
    size_t lo = 0, hi = nmemb;
    unsigned char *a = (unsigned char *)base;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = compar(key, a + mid * size);
        if (c < 0) hi = mid;
        else if (c > 0) lo = mid + 1;
        else return a + mid * size;
    }
    return NULL;
}

static unsigned long rand_state = 1;
int rand(void)
{
    rand_state = rand_state * 1103515245UL + 12345UL;
    return (int)((rand_state >> 16) & 0x7fffffff);
}
void srand(unsigned int seed) { rand_state = seed; }

char *getenv(const char *name) { (void)name; return NULL; }
int   system(const char *command) { (void)command; return -1; }

/* atexit handlers: DOOM registers a couple but we never run a clean exit path
 * (the WM reaps the task), so just record nothing and succeed. */
int atexit(void (*func)(void)) { (void)func; return 0; }

void exit(int status)  { sys_exit(status); for (;;) {} }
void abort(void)       { print("quake: abort()\n"); sys_exit(134); for (;;) {} }

void __quake_assert_fail(const char *expr, const char *file, int line)
{
    char buf[256];
    snprintf(buf, sizeof buf, "quake: assertion failed: %s (%s:%d)\n", expr, file, line);
    print(buf);
    sys_exit(134);
    for (;;) {}
}

/* ============================ math ==================================== */
/* SSE is enabled; use builtins where they lower to single instructions, and
 * small series elsewhere.  Only sin/cos/tan/atan/fabs/sqrt are referenced
 * (tan via sin/cos); the rest are here for header/link completeness. */

double fabs(double x)  { return __builtin_fabs(x); }
double sqrt(double x)  { return __builtin_sqrt(x); }
double floor(double x) { return __builtin_floor(x); }
double ceil(double x)  { return __builtin_ceil(x); }
double fmod(double x, double y)
{
    if (y == 0.0) return 0.0;
    double q = x / y;
    /* truncate toward zero */
    double t = (q < 0.0) ? -__builtin_floor(-q) : __builtin_floor(q);
    return x - t * y;
}

/* Range-reduce to [-pi, pi] then evaluate a Taylor series.  Plenty accurate
 * for DOOM's renderer (it only uses these to build the tan/atan tables once at
 * startup, and to fold a few angles). */
double sin(double x)
{
    const double PI = 3.14159265358979323846;
    const double TWO_PI = 2.0 * PI;
    /* reduce to [-pi, pi] */
    x = fmod(x, TWO_PI);
    if (x > PI)  x -= TWO_PI;
    if (x < -PI) x += TWO_PI;
    double term = x, sum = x, x2 = x * x;
    for (int n = 1; n < 12; n++) {
        term *= -x2 / ((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

double cos(double x) { return sin(x + 1.57079632679489661923); }

double tan(double x)
{
    double c = cos(x);
    if (c == 0.0) c = 1e-12;
    return sin(x) / c;
}

/* atan via the identity atan(x) = asin(x / sqrt(1+x^2)) is awkward; use a
 * polynomial after folding |x| <= 1.  DOOM only passes 0..1 (i/SLOPERANGE). */
double atan(double x)
{
    int neg = 0, inv = 0;
    if (x < 0) { x = -x; neg = 1; }
    if (x > 1.0) { x = 1.0 / x; inv = 1; }
    /* minimax-ish odd polynomial on [0,1] */
    double x2 = x * x;
    double r = x * (0.9998660 + x2 * (-0.3302995 + x2 * (0.1801410 +
               x2 * (-0.0851330 + x2 * 0.0208351))));
    if (inv) r = 1.57079632679489661923 - r;
    return neg ? -r : r;
}

double atan2(double y, double x)
{
    const double PI = 3.14159265358979323846;
    if (x > 0) return atan(y / x);
    if (x < 0) return (y >= 0) ? atan(y / x) + PI : atan(y / x) - PI;
    /* x == 0 */
    if (y > 0) return PI / 2;
    if (y < 0) return -PI / 2;
    return 0.0;
}

double exp(double x)
{
    double term = 1.0, sum = 1.0;
    for (int n = 1; n < 20; n++) { term *= x / n; sum += term; }
    return sum;
}

double log(double x)
{
    if (x <= 0.0) return 0.0;
    /* ln(x) via atanh series: x = (1+t)/(1-t) */
    double t = (x - 1.0) / (x + 1.0), t2 = t * t, sum = 0.0, term = t;
    for (int n = 0; n < 30; n++) { sum += term / (2 * n + 1); term *= t2; }
    return 2.0 * sum;
}

double pow(double base, double e)
{
    /* integer-exponent fast path (the only way DOOM could reach here) */
    if (e == (double)(int)e) {
        int n = (int)e;
        double r = 1.0, b = base;
        int neg = n < 0;
        if (neg) n = -n;
        while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
        return neg ? 1.0 / r : r;
    }
    if (base <= 0.0) return 0.0;
    return exp(e * log(base));
}

/* ======================= printf / formatting ========================= */
/* A self-contained vsnprintf supporting the conversions DOOM uses:
 * %d %i %u %x %X %c %s %p %% with field width, left-justify '-', zero-pad '0',
 * and the 'l'/'ll'/'z' length modifiers, plus a minimal %f. */

struct outbuf {
    char  *buf;     /* may be NULL for counting only */
    size_t cap;     /* capacity including space for NUL */
    size_t len;     /* chars emitted so far (excluding NUL) */
};

static void ob_putc(struct outbuf *o, char c)
{
    if (o->buf && o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static void ob_pad(struct outbuf *o, char c, int n)
{
    while (n-- > 0) ob_putc(o, c);
}

static const char *DIGITS_LC = "0123456789abcdef";
static const char *DIGITS_UC = "0123456789ABCDEF";

/* render an unsigned value in the given base into tmp (reversed), return len */
static int u_to_str(char *tmp, unsigned long long v, int base, const char *digits)
{
    int i = 0;
    if (v == 0) { tmp[i++] = '0'; return i; }
    while (v) { tmp[i++] = digits[v % base]; v /= base; }
    return i;
}

static void emit_number(struct outbuf *o, char *tmp, int ntmp,
                        const char *prefix, int neg,
                        int width, int precision, int left, int zero)
{
    int prefixlen = 0;
    while (prefix && prefix[prefixlen]) prefixlen++;
    int signlen = neg ? 1 : 0;

    /* precision: minimum number of digits */
    int zeros_for_prec = 0;
    if (precision >= 0 && precision > ntmp) zeros_for_prec = precision - ntmp;
    if (precision == 0 && ntmp == 1 && tmp[0] == '0') { ntmp = 0; }

    int bodylen = ntmp + zeros_for_prec;
    int totallen = signlen + prefixlen + bodylen;
    int pad = width - totallen;
    if (pad < 0) pad = 0;

    /* zero-padding only applies when not left-justified and no precision given */
    if (zero && !left && precision < 0) {
        if (neg) ob_putc(o, '-');
        for (int i = 0; i < prefixlen; i++) ob_putc(o, prefix[i]);
        ob_pad(o, '0', pad);
    } else {
        if (!left) ob_pad(o, ' ', pad);
        if (neg) ob_putc(o, '-');
        for (int i = 0; i < prefixlen; i++) ob_putc(o, prefix[i]);
    }
    ob_pad(o, '0', zeros_for_prec);
    for (int i = ntmp - 1; i >= 0; i--) ob_putc(o, tmp[i]);
    if (left) ob_pad(o, ' ', pad);
}

static int do_format(struct outbuf *o, const char *fmt, va_list ap)
{
    char tmp[32];
    for (; *fmt; fmt++) {
        if (*fmt != '%') { ob_putc(o, *fmt); continue; }
        fmt++;
        if (*fmt == '%') { ob_putc(o, '%'); continue; }

        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '#') alt = 1;
            else break;
        }

        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; if (width < 0) { left = 1; width = -width; } }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');

        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') { precision = va_arg(ap, int); fmt++; if (precision < 0) precision = -1; }
            else while (*fmt >= '0' && *fmt <= '9') precision = precision * 10 + (*fmt++ - '0');
        }

        int lng = 0;     /* 0=int, 1=long, 2=long long */
        for (;;) {
            if (*fmt == 'l') { lng++; fmt++; }
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { lng = 1; fmt++; }
            else if (*fmt == 'h') { fmt++; }   /* ignore: promoted anyway */
            else break;
        }

        char conv = *fmt;
        switch (conv) {
        case 'd':
        case 'i': {
            long long v = (lng >= 2) ? va_arg(ap, long long)
                       : (lng == 1) ? (long long)va_arg(ap, long)
                                    : (long long)va_arg(ap, int);
            int neg = v < 0;
            unsigned long long uv = neg ? (unsigned long long)(-(v + 1)) + 1ULL
                                        : (unsigned long long)v;
            int n = u_to_str(tmp, uv, 10, DIGITS_LC);
            const char *pfx = neg ? NULL : (plus ? "+" : (space ? " " : NULL));
            int forced_sign = (!neg && (plus || space));
            if (forced_sign) {
                /* treat the leading +/space like a sign for width accounting */
                int signlen = 1;
                int zeros_for_prec = (precision > n) ? precision - n : 0;
                int bodylen = n + zeros_for_prec;
                int totallen = signlen + bodylen;
                int pad = width - totallen; if (pad < 0) pad = 0;
                if (zero && !left && precision < 0) { ob_putc(o, plus ? '+' : ' '); ob_pad(o, '0', pad); }
                else { if (!left) ob_pad(o, ' ', pad); ob_putc(o, plus ? '+' : ' '); }
                ob_pad(o, '0', zeros_for_prec);
                for (int i = n - 1; i >= 0; i--) ob_putc(o, tmp[i]);
                if (left) ob_pad(o, ' ', pad);
            } else {
                (void)pfx;
                emit_number(o, tmp, n, NULL, neg, width, precision, left, zero);
            }
            break;
        }
        case 'u': {
            unsigned long long v = (lng >= 2) ? va_arg(ap, unsigned long long)
                                : (lng == 1) ? (unsigned long long)va_arg(ap, unsigned long)
                                             : (unsigned long long)va_arg(ap, unsigned int);
            int n = u_to_str(tmp, v, 10, DIGITS_LC);
            emit_number(o, tmp, n, NULL, 0, width, precision, left, zero);
            break;
        }
        case 'x':
        case 'X': {
            unsigned long long v = (lng >= 2) ? va_arg(ap, unsigned long long)
                                : (lng == 1) ? (unsigned long long)va_arg(ap, unsigned long)
                                             : (unsigned long long)va_arg(ap, unsigned int);
            const char *digs = (conv == 'X') ? DIGITS_UC : DIGITS_LC;
            int n = u_to_str(tmp, v, 16, digs);
            const char *pfx = (alt && v != 0) ? (conv == 'X' ? "0X" : "0x") : NULL;
            emit_number(o, tmp, n, pfx, 0, width, precision, left, zero);
            break;
        }
        case 'o': {
            unsigned long long v = (lng >= 2) ? va_arg(ap, unsigned long long)
                                : (lng == 1) ? (unsigned long long)va_arg(ap, unsigned long)
                                             : (unsigned long long)va_arg(ap, unsigned int);
            int n = u_to_str(tmp, v, 8, DIGITS_LC);
            emit_number(o, tmp, n, NULL, 0, width, precision, left, zero);
            break;
        }
        case 'p': {
            unsigned long long v = (unsigned long long)(uintptr_t)va_arg(ap, void *);
            int n = u_to_str(tmp, v, 16, DIGITS_LC);
            emit_number(o, tmp, n, "0x", 0, width, precision, left, zero);
            break;
        }
        case 'c': {
            char ch = (char)va_arg(ap, int);
            int pad = width - 1; if (pad < 0) pad = 0;
            if (!left) ob_pad(o, ' ', pad);
            ob_putc(o, ch);
            if (left) ob_pad(o, ' ', pad);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int n = 0;
            while (s[n] && (precision < 0 || n < precision)) n++;
            int pad = width - n; if (pad < 0) pad = 0;
            if (!left) ob_pad(o, ' ', pad);
            for (int i = 0; i < n; i++) ob_putc(o, s[i]);
            if (left) ob_pad(o, ' ', pad);
            break;
        }
        case 'f':
        case 'F': {
            double d = va_arg(ap, double);
            int neg = 0;
            if (d < 0) { neg = 1; d = -d; }
            if (precision < 0) precision = 6;
            /* integer part */
            unsigned long long ip = (unsigned long long)d;
            double frac = d - (double)ip;
            int ni = u_to_str(tmp, ip, 10, DIGITS_LC);
            /* build into a scratch then pad to width */
            char fbuf[64]; int fl = 0;
            if (neg) fbuf[fl++] = '-';
            for (int i = ni - 1; i >= 0 && fl < (int)sizeof(fbuf); i--) fbuf[fl++] = tmp[i];
            if (precision > 0 && fl < (int)sizeof(fbuf)) {
                fbuf[fl++] = '.';
                for (int k = 0; k < precision && fl < (int)sizeof(fbuf); k++) {
                    frac *= 10.0;
                    int dig = (int)frac;
                    if (dig > 9) dig = 9;
                    fbuf[fl++] = (char)('0' + dig);
                    frac -= dig;
                }
            }
            int pad = width - fl; if (pad < 0) pad = 0;
            if (!left) ob_pad(o, zero ? '0' : ' ', pad);
            for (int i = 0; i < fl; i++) ob_putc(o, fbuf[i]);
            if (left) ob_pad(o, ' ', pad);
            break;
        }
        default:
            /* unknown conversion: emit literally */
            ob_putc(o, '%');
            if (conv) ob_putc(o, conv);
            break;
        }
    }
    if (o->buf) {
        if (o->len < o->cap) o->buf[o->len] = '\0';
        else if (o->cap > 0) o->buf[o->cap - 1] = '\0';
    }
    return (int)o->len;
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap)
{
    struct outbuf o = { str, size, 0 };
    return do_format(&o, fmt, ap);
}

int snprintf(char *str, size_t size, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char *str, const char *fmt, va_list ap)
{
    struct outbuf o = { str, (size_t)0x7fffffff, 0 };
    return do_format(&o, fmt, ap);
}

int sprintf(char *str, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsprintf(str, fmt, ap);
    va_end(ap);
    return r;
}

/* ============================ stdio =================================== */
/* The three standard streams. stdin is never read (we have no console input
 * on this path); stdout/stderr route to print()/sys_write. */
static FILE _stdin  = { 0,0,0,0, 0,0,0, 0, 0 };
static FILE _stdout = { 0,0,0,0, 0,0,0, 1, 0 };
static FILE _stderr = { 0,0,0,0, 0,0,0, 2, 0 };
FILE *stdin  = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

/* Uppercase a filename in place into out[] (the OS uses uppercase 8.3 names). */
static void upper_name(const char *in, char *out, size_t outmax)
{
    /* Our filesystem is flat 8.3, but Quake opens paths like "id1/pak0.pak":
     * reduce to the basename (after the last slash) and upper-case it, so the
     * data is found at "PAK0.PAK". */
    const char *base = in;
    for (const char *p = in; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    size_t i = 0;
    for (; base[i] && i + 1 < outmax; i++) {
        char c = base[i];
        out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    out[i] = '\0';
}

FILE *fopen(const char *path, const char *mode)
{
    if (!path || !mode) return NULL;
    int writing = (mode[0] == 'w' || mode[0] == 'a');

    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) return NULL;
    memset(f, 0, sizeof(*f));

    char name[64];
    upper_name(path, name, sizeof name);

    if (!writing) {
        /* read mode: slurp the whole file into a malloc'd buffer.  We don't
         * know the size up front, so probe with a generous cap and grow if it
         * comes back full. */
        long cap = 4 * 1024 * 1024;
        for (;;) {
            unsigned char *b = (unsigned char *)malloc((unsigned long)cap);
            if (!b) { free(f); return NULL; }
            long n = sys_readfile(name, b, (unsigned long)cap);
            if (n < 0) { free(b); free(f); return NULL; }
            if (n < cap) {          /* fit (or empty) */
                f->buf = b; f->len = n; f->cap = cap; f->pos = 0;
                f->mode = 0;
                return f;
            }
            /* came back exactly full: file may be larger — grow and retry */
            free(b);
            if (cap > 64 * 1024 * 1024) {   /* sane ceiling for the WAD */
                f->buf = NULL; f->len = 0; f->cap = 0; f->pos = 0; f->mode = 0;
                /* fall through with what we can: re-read at the ceiling */
                unsigned char *b2 = (unsigned char *)malloc((unsigned long)cap);
                if (!b2) { free(f); return NULL; }
                long n2 = sys_readfile(name, b2, (unsigned long)cap);
                if (n2 < 0) { free(b2); free(f); return NULL; }
                f->buf = b2; f->len = n2; f->cap = cap;
                return f;
            }
            cap *= 2;
        }
    } else {
        /* write mode: start with a small growable buffer */
        f->mode = 1;
        f->cap = 4096;
        f->buf = (unsigned char *)malloc((unsigned long)f->cap);
        if (!f->buf) { free(f); return NULL; }
        f->len = 0; f->pos = 0;
        f->name = strdup(name);
        return f;
    }
}

static int ensure_cap(FILE *f, long need)
{
    if (need <= f->cap) return 1;
    long nc = f->cap ? f->cap : 4096;
    while (nc < need) nc *= 2;
    unsigned char *nb = (unsigned char *)realloc(f->buf, (unsigned long)nc);
    if (!nb) return 0;
    f->buf = nb; f->cap = nc;
    return 1;
}

int fflush(FILE *f)
{
    if (!f) return 0;
    if (f->is_std) return 0;
    if (f->mode == 1 && f->name) {
        sys_writefile(f->name, f->buf, (unsigned long)f->len);
    }
    return 0;
}

int fclose(FILE *f)
{
    if (!f) return EOF;
    if (f->is_std) return 0;
    if (f->mode == 1) fflush(f);
    if (f->buf) free(f->buf);
    if (f->name) free(f->name);
    free(f);
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f)
{
    if (!f || f->mode != 0 || size == 0) return 0;
    long want = (long)(size * nmemb);
    long avail = f->len - f->pos;
    if (avail <= 0) { f->eof = 1; return 0; }
    long take = want < avail ? want : avail;
    memcpy(ptr, f->buf + f->pos, (unsigned long)take);
    f->pos += take;
    if (take < want) f->eof = 1;
    return (size_t)(take / (long)size);
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f)
{
    if (!f || size == 0) return 0;
    long n = (long)(size * nmemb);
    if (f->is_std) {
        sys_write(f->is_std == 2 ? 2 : 1, ptr, (unsigned long)n);
        return nmemb;
    }
    if (f->mode != 1) return 0;
    if (!ensure_cap(f, f->pos + n)) return 0;
    memcpy(f->buf + f->pos, ptr, (unsigned long)n);
    f->pos += n;
    if (f->pos > f->len) f->len = f->pos;
    return nmemb;
}

int fseek(FILE *f, long offset, int whence)
{
    if (!f) return -1;
    long base = 0;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = f->pos;
    else if (whence == SEEK_END) base = f->len;
    else return -1;
    long np = base + offset;
    if (np < 0) return -1;
    f->pos = np;
    f->eof = 0;
    return 0;
}

long ftell(FILE *f) { return f ? f->pos : -1; }
void rewind(FILE *f) { if (f) { f->pos = 0; f->eof = 0; } }
int  feof(FILE *f) { return f ? f->eof : 1; }
int  ferror(FILE *f) { return f ? f->err : 0; }

int fgetc(FILE *f)
{
    unsigned char c;
    if (fread(&c, 1, 1, f) != 1) return EOF;
    return (int)c;
}
int getc(FILE *f) { return fgetc(f); }

char *fgets(char *s, int size, FILE *f)
{
    if (size <= 0 || !f) return NULL;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = '\0';
    return s;
}

int fputc(int c, FILE *f)
{
    unsigned char ch = (unsigned char)c;
    if (fwrite(&ch, 1, 1, f) != 1) return EOF;
    return c;
}
int putc(int c, FILE *f) { return fputc(c, f); }

int putchar(int c)
{
    char ch = (char)c;
    sys_write(1, &ch, 1);
    return c;
}

int fputs(const char *s, FILE *f)
{
    size_t n = strlen(s);
    return (fwrite(s, 1, n, f) == n) ? (int)n : EOF;
}

int puts(const char *s)
{
    sys_write(1, s, (unsigned long)strlen(s));
    sys_write(1, "\n", 1);
    return 0;
}

int fileno(FILE *f) { return f ? f->is_std : -1; }

int remove(const char *path) { (void)path; return 0; }
int rename(const char *oldp, const char *newp) { (void)oldp; (void)newp; return 0; }
void setbuf(FILE *f, char *buf) { (void)f; (void)buf; }
int  setvbuf(FILE *f, char *buf, int mode, size_t size) { (void)f; (void)buf; (void)mode; (void)size; return 0; }

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    /* Render into a stack buffer, then push to the stream. */
    char buf[1024];
    struct outbuf o = { buf, sizeof buf, 0 };
    int r = do_format(&o, fmt, ap);
    size_t n = o.len < sizeof buf - 1 ? o.len : sizeof buf - 1;
    if (f && f->is_std) sys_write(f->is_std == 2 ? 2 : 1, buf, n);
    else if (f) fwrite(buf, 1, n, f);
    return r;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

/* Shared scanf core over a NUL-terminated input string `str`.  Supports the
 * conversions the engine uses: %d %i %u %x %X %o (-> int*), %f %e %g (-> float*),
 * %s (whitespace-delimited, optional width), and %[...] / %[^...] (set scan).
 * On return *consumed = number of input chars eaten (so fscanf can advance the
 * FILE position).  Returns the number of assignments made. */
static int vscan_str(const char *str, const char *fmt, va_list ap, int *consumed)
{
    int assigned = 0;
    const char *s = str;
    for (const char *p = fmt; *p; p++) {
        if (isspace((unsigned char)*p)) { while (isspace((unsigned char)*s)) s++; continue; }
        if (*p != '%') {
            if (*p == *s) { s++; continue; }
            else break;
        }
        p++;                       /* past '%' */
        int suppress = 0;
        if (*p == '*') { suppress = 1; p++; }
        /* skip optional field width digits (e.g. %79s) */
        int width = 0, havew = 0;
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p++ - '0'); havew = 1; }
        /* swallow length modifiers (l, h, …) — our targets are int/float */
        while (*p == 'l' || *p == 'h' || *p == 'L' || *p == 'z' || *p == 'j' || *p == 't') p++;
        char conv = *p;
        if (conv != '[' && conv != 'c') while (isspace((unsigned char)*s)) s++;
        if (conv == 'x' || conv == 'X' || conv == 'i' || conv == 'd' ||
            conv == 'o' || conv == 'u') {
            int base = 10;
            if (conv == 'x' || conv == 'X') base = 16;
            else if (conv == 'o') base = 8;
            char *end;
            long v = strtol(s, &end, (conv == 'i') ? 0 : base);
            if (end == s) break;
            if (!suppress) { int *out = va_arg(ap, int *); *out = (int)v; assigned++; }
            s = end;
        } else if (conv == 'f' || conv == 'e' || conv == 'g' ||
                   conv == 'F' || conv == 'E' || conv == 'G') {
            char *end;
            double v = strtod(s, &end);
            if (end == s) break;
            if (!suppress) { float *out = va_arg(ap, float *); *out = (float)v; assigned++; }
            s = end;
        } else if (conv == 's') {
            char *out = suppress ? NULL : va_arg(ap, char *);
            int n = 0;
            while (*s && !isspace((unsigned char)*s) && (!havew || n < width)) {
                if (out) out[n] = *s;
                n++; s++;
            }
            if (out) out[n] = '\0';
            if (n) { if (!suppress) assigned++; } else break;
        } else if (conv == '[') {
            /* %[...] / %[^...] set scan (config/level loaders use %[^\n]) */
            p++;
            int negate = 0;
            if (*p == '^') { negate = 1; p++; }
            char setbuf[16]; int sl = 0;
            if (*p == ']') { setbuf[sl++] = ']'; p++; }   /* leading ] is a literal */
            while (*p && *p != ']' && sl < (int)sizeof(setbuf)) setbuf[sl++] = *p++;
            char *out = suppress ? NULL : va_arg(ap, char *);
            int n = 0;
            while (*s && (!havew || n < width)) {
                int in_set = 0;
                for (int k = 0; k < sl; k++) if (*s == setbuf[k]) { in_set = 1; break; }
                if (negate ? in_set : !in_set) break;
                if (out) out[n] = *s;
                n++; s++;
            }
            if (out) out[n] = '\0';
            if (n) { if (!suppress) assigned++; } else break;
        } else {
            break;
        }
        (void)havew;
    }
    if (consumed) *consumed = (int)(s - str);
    return assigned;
}

int sscanf(const char *str, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int assigned = vscan_str(str, fmt, ap, NULL);
    va_end(ap);
    return assigned;
}

/* fscanf over a read-mode FILE.  The whole file already lives in f->buf; we copy
 * the bytes from the current cursor into a bounded, NUL-terminated scratch buffer
 * (Quake's fscanf calls each read at most a line / a few floats), run the shared
 * scan core, then advance the cursor by however many bytes were consumed. */
int vfscanf(FILE *f, const char *fmt, va_list ap)
{
    if (!f || f->mode != 0) return -1;
    long avail = f->len - f->pos;
    if (avail <= 0) { f->eof = 1; return -1; }      /* EOF -> -1, like C */
    long take = avail < 4095 ? avail : 4095;
    char tmp[4096];
    memcpy(tmp, f->buf + f->pos, (unsigned long)take);
    tmp[take] = '\0';
    int consumed = 0;
    int assigned = vscan_str(tmp, fmt, ap, &consumed);
    f->pos += consumed;
    if (f->pos >= f->len) f->eof = 1;
    return assigned;
}

int fscanf(FILE *f, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

/* ===================== unistd / fcntl / stat stubs ==================== */
int   access(const char *path, int mode) { (void)path; (void)mode; return -1; }
int   unlink(const char *path) { (void)path; return 0; }
int   isatty(int fd) { (void)fd; return 0; }
int   close(int fd) { (void)fd; return 0; }
long  read(int fd, void *buf, unsigned long count) { (void)fd; (void)buf; (void)count; return -1; }
long  write(int fd, const void *buf, unsigned long count)
{
    if (fd == 1 || fd == 2) return sys_write(fd, buf, count);
    return -1;
}
long  lseek(int fd, long offset, int whence) { (void)fd; (void)offset; (void)whence; return -1; }
int   usleep(unsigned long usec) { (void)usec; return 0; }
unsigned int sleep(unsigned int seconds) { (void)seconds; return 0; }
char *getcwd(char *buf, size_t size) { if (buf && size) buf[0] = '\0'; return buf; }
int   chdir(const char *path) { (void)path; return 0; }
int   open(const char *path, int flags, ...) { (void)path; (void)flags; return -1; }

int mkdir(const char *path, mode_t mode) { (void)path; (void)mode; return 0; }
int stat(const char *path, struct stat *st) { (void)path; if (st) memset(st, 0, sizeof *st); return -1; }
int fstat(int fd, struct stat *st) { (void)fd; if (st) memset(st, 0, sizeof *st); return -1; }
int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv) { tv->tv_sec = 0; tv->tv_usec = 0; }
    return 0;
}
