/* lsfmt.h — pure formatting helpers for `ls -l` (M1817): the "drwxr-xr-x" mode
 * string and the "YYYY-MM-DD HH:MM" mtime column. Kept pure (no syscalls) so the
 * permission/calendar math is host-unit-tested by tests/lsfmt; user/shell.c
 * #includes it for the `ls -l` builtin.
 * NOTE: keep in sync with its host test (tests/lsfmt/lsfmt_test.c). */
#ifndef LSFMT_H
#define LSFMT_H
#ifndef S_IFMT
#define S_IFMT  0xF000
#define S_IFREG 0x8000
#define S_IFDIR 0x4000
#define S_IFLNK 0xA000
#endif

/* mode -> 10-char "drwxr-xr-x" (type char + rwx triplets), NUL-terminated. */
static void ls_mode_str(unsigned mode, char *out) {
    unsigned t = mode & S_IFMT;
    out[0] = (t == S_IFDIR) ? 'd' : (t == S_IFLNK) ? 'l' : '-';
    static const char rwx[3] = { 'r', 'w', 'x' };
    for (int i = 0; i < 9; i++) out[1 + i] = (mode & (0400u >> i)) ? rwx[i % 3] : '-';
    out[10] = 0;
}

/* epoch day count (days since 1970-01-01) -> Y / M(1-12) / D, via Howard
 * Hinnant's days_from_civil inverse (the same algorithm the kernel Date uses). */
static void ls_civil(long z, int *y, int *m, int *d) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yr = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yr + (mm <= 2)); *m = (int)mm; *d = (int)dd;
}
/* epoch seconds (UTC) -> "YYYY-MM-DD HH:MM" (16 chars + NUL). */
static void ls_fmt_time(unsigned long epoch, char *out) {
    long days = (long)(epoch / 86400); unsigned tod = (unsigned)(epoch % 86400);
    int y, mo, d; ls_civil(days, &y, &mo, &d);
    int H = (int)(tod / 3600), Mi = (int)((tod % 3600) / 60);
    out[0]='0'+(y/1000)%10; out[1]='0'+(y/100)%10; out[2]='0'+(y/10)%10; out[3]='0'+y%10; out[4]='-';
    out[5]='0'+(mo/10)%10; out[6]='0'+mo%10; out[7]='-';
    out[8]='0'+(d/10)%10; out[9]='0'+d%10; out[10]=' ';
    out[11]='0'+(H/10)%10; out[12]='0'+H%10; out[13]=':';
    out[14]='0'+(Mi/10)%10; out[15]='0'+Mi%10; out[16]=0;
}
#endif /* LSFMT_H */
