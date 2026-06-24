// pcwd.c — per-process current directory demo (M1144). The parent cd's to /proc,
// then forks a child that cd's to /tmp. If the cwd is truly per-process, the
// child's chdir leaves the parent's cwd alone — so after the child exits, the
// parent still lists /proc. (With the old global cwd, the child's cd to /tmp
// would have changed the parent's listing too.)
#include "ulib.h"

static int contains(const char *hay, const char *needle) {
    for (int i = 0; hay[i]; i++) {
        int j = 0; while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

int main(void) {
    sys_chdir("/proc");                       // parent's cwd = /proc
    long pid = sys_fork();
    if (pid == 0) { sys_chdir("/tmp"); sys_exit(0); }   // child: cd /tmp, then exit

    int st; sys_waitpid((int)pid, &st);        // let the child cd + exit first
    char b[1024]; long n = sys_list(b, sizeof b - 1); if (n > 0) b[n] = 0;

    print("per-process cwd:\n");
    print("  parent cd'd to /proc; forked a child that cd'd to /tmp.\n");
    print("  after the child exited, the parent lists its OWN cwd:\n\n");
    print(b);

    int ok = contains(b, "uptime");            // /proc has 'uptime'; /tmp would not
    print(ok ? "\nPASS: parent still sees /proc -- the child's cd to /tmp did NOT leak.\n"
             : "\nFAIL: the child's chdir corrupted the parent's cwd (global-cwd leak).\n");

    sys_chdir("/");
    sys_sleep(20000);
    return 0;
}
