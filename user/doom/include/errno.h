/* errno.h — DOOM libc shim. */
#ifndef _OSDEV_ERRNO_H
#define _OSDEV_ERRNO_H

extern int errno;

#define EPERM    1
#define ENOENT   2
#define EIO      5
#define EBADF    9
#define ENOMEM  12
#define EACCES  13
#define EEXIST  17
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ERANGE  34

#endif /* _OSDEV_ERRNO_H */
