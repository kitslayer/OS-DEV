/* sys/time.h — DOOM libc shim (only pulled in by a disabled code path). */
#ifndef _OSDEV_SYS_TIME_H
#define _OSDEV_SYS_TIME_H

#include <sys/types.h>

struct timeval {
    time_t tv_sec;
    long   tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

int gettimeofday(struct timeval *tv, void *tz);

#endif /* _OSDEV_SYS_TIME_H */
