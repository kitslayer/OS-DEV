// overlay.c — overlay/union filesystem demo (M1142 copy-up + M1143 whiteout &
// merged listing). /over unions a read-only LOWER (the ISO /disk2) and a
// writable UPPER (tmpfs /tmp): reads fall through to the lower, writes copy-up to
// the upper, a merged `ls` shows the union, and deleting a lower file lays a
// whiteout that hides it — while the lower stays read-only and intact.
#include "ulib.h"

static void show(const char *label, const char *s, long n) {
    print(label); print(" \"");
    for (long i = 0; i < n && s[i] != '\n'; i++) { char c[2] = { s[i], 0 }; print(c); }
    print("\"\n");
}

int main(void) {
    if (sys_overlay("/disk2", "/tmp") < 0) { print("overlay: mount failed (need an ISO on /disk2)\n"); sys_sleep(20000); return 1; }
    print("overlay /over = lower:/disk2 (read-only ISO) + upper:/tmp (writable tmpfs)\n\n");

    char a[256], b[256], c[256], ls[512];
    long na = sys_readfile("/over/HELLO.TXT", a, sizeof a - 1); if (na > 0) a[na] = 0;
    show("read /over/HELLO.TXT (falls through to lower):", a, na);
    char orig[256]; long on = na; for (long i = 0; i <= na; i++) orig[i] = a[i];

    sys_chdir("/over");
    sys_list(ls, sizeof ls - 1);
    print("ls /over (merged upper+lower):\n"); print(ls);

    const char *mod = "MODIFIED via overlay copy-up.";
    sys_writefile("/over/HELLO.TXT", mod, ustrlen(mod));
    long nb = sys_readfile("/over/HELLO.TXT", b, sizeof b - 1); if (nb > 0) b[nb] = 0;
    show("after write, read /over/HELLO.TXT (upper):", b, nb);
    int copied = (nb == (long)ustrlen(mod));
    for (long i = 0; i < nb && copied; i++) if (b[i] != mod[i]) copied = 0;

    sys_delete("/over/HELLO.TXT");                       /* whiteout */
    long g = sys_readfile("/over/HELLO.TXT", c, sizeof c - 1);
    print("after rm, read /over/HELLO.TXT -> "); print(g < 0 ? "GONE (whiteout hides the lower)\n" : "STILL PRESENT?!\n");
    sys_list(ls, sizeof ls - 1);
    print("ls /over after rm:\n"); print(ls);

    long ln = sys_readfile("/disk2/HELLO.TXT", c, sizeof c - 1); if (ln > 0) c[ln] = 0;
    show("lower /disk2/HELLO.TXT (still read-only/intact):", c, ln);
    int lower_ok = (ln == on);
    for (long i = 0; i < ln && lower_ok; i++) if (c[i] != orig[i]) lower_ok = 0;

    print((copied && g < 0 && lower_ok)
          ? "\nPASS: copy-up + merged listing + whiteout all work; the lower stayed intact.\n"
          : "\nFAIL\n");
    sys_chdir("/");                                     /* restore the (process-global) cwd off /over */
    sys_sleep(20000);
    return 0;
}
