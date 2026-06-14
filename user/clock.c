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
        print("==  OS-DEV clock  ==\n\n");

        char when[24];
        sys_time(when, sizeof(when));
        print(when);

        char info[128];
        sys_sysinfo(info, sizeof(info));
        print("\n");
        print(info);

        sys_sleep(1000);          /* refresh once a second */
    }
    return 0;
}
