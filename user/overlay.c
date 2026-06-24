// overlay.c — union/overlay filesystem demo (M1142). Mount /over as a union of a
// read-only LOWER (the ISO9660 disk /disk2) and a writable UPPER (tmpfs /tmp).
// A read of /over/X falls through to the lower; a write copies-up to the upper,
// shadowing the lower — which stays byte-for-byte untouched. This is the
// mechanism behind container image layers and writable-overlay live CDs.
#include "ulib.h"

static void show(const char *label, const char *s, long n) {
    print(label); print(" \"");
    for (long i = 0; i < n && s[i] != '\n'; i++) { char c[2] = { s[i], 0 }; print(c); }
    print("\"\n");
}

int main(void) {
    if (sys_overlay("/disk2", "/tmp") < 0) { print("overlay: mount failed (need an ISO on /disk2)\n"); sys_sleep(20000); return 1; }
    print("overlay /over = lower:/disk2 (read-only ISO) + upper:/tmp (writable tmpfs)\n\n");

    char a[256], b[256], c[256];
    long na = sys_readfile("/over/HELLO.TXT", a, sizeof a - 1); if (na > 0) a[na] = 0;
    show("read /over/HELLO.TXT  (falls through to the lower):", a, na);

    const char *mod = "MODIFIED through the overlay -- this copy lives in the tmpfs upper.";
    sys_writefile("/over/HELLO.TXT", mod, ustrlen(mod));
    print("wrote /over/HELLO.TXT -> copy-up to the upper layer.\n");

    long nb = sys_readfile("/over/HELLO.TXT", b, sizeof b - 1); if (nb > 0) b[nb] = 0;
    show("read /over/HELLO.TXT  (upper now shadows the lower):", b, nb);

    long nc = sys_readfile("/disk2/HELLO.TXT", c, sizeof c - 1); if (nc > 0) c[nc] = 0;
    show("read /disk2/HELLO.TXT (the LOWER, directly):", c, nc);

    int lower_untouched = (na == nc);
    for (long i = 0; i < na && lower_untouched; i++) if (a[i] != c[i]) lower_untouched = 0;
    int copied_up = (nb == (long)ustrlen(mod));
    for (long i = 0; i < nb && copied_up; i++) if (b[i] != mod[i]) copied_up = 0;

    print((lower_untouched && copied_up)
          ? "\nPASS: read fell through to the lower; write copied up to the upper; lower untouched.\n"
          : "\nFAIL: overlay semantics wrong.\n");
    sys_sleep(20000);
    return 0;
}
