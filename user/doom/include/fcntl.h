/* fcntl.h — DOOM libc shim (declarations only; DOOM prefers the stdio path). */
#ifndef _OSDEV_FCNTL_H
#define _OSDEV_FCNTL_H

int open(const char *path, int flags, ...);

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000
#define O_APPEND 02000
#define O_BINARY 0

#endif /* _OSDEV_FCNTL_H */
