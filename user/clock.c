/*
 * clock.c — a second userspace program: a live clock + system monitor.
 *
 * Unlike the shell (which waits for input), this loops forever: clear the
 * window, print the date/time and a memory/uptime summary, sleep a second,
 * repeat. It proves the desktop can host *distinct* programs as windows, and
 * that a program can update its own window continuously.
 */
#include "ulib.h"

int main(void) {
    for (;;) {
        sys_clear();
        sys_setcolor(4); print("  == OS-DEV Clock ==\n\n");      /* title: cyan */

        char when[24];
        sys_time(when, sizeof(when));            /* "YYYY-MM-DD HH:MM:SS" */
        char date[11]; for (int i = 0; i < 10 && when[i]; i++) date[i] = when[i]; date[10] = 0;
        sys_setcolor(8); print("  Date   ");     /* labels: grey */
        sys_setcolor(1); print(date); print("\n");
        sys_setcolor(8); print("  Time   ");
        sys_setcolor(3); print(when + 11); print("\n\n");        /* time: yellow */

        char info[128];
        sys_sysinfo(info, sizeof(info));
        sys_setcolor(9); print(info);            /* RAM / uptime / tasks: lime */
        sys_setcolor(0);

        sys_sleep(1000);          /* refresh once a second */
    }
    return 0;
}
