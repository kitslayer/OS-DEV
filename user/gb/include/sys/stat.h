/* sys/stat.h — DOOM libc shim. M_MakeDirectory calls mkdir(path, 0755). */
#ifndef _OSDEV_SYS_STAT_H
#define _OSDEV_SYS_STAT_H

#include <sys/types.h>

struct stat {
    off_t  st_size;
    mode_t st_mode;
};

int mkdir(const char *path, mode_t mode);
int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);

#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)

#endif /* _OSDEV_SYS_STAT_H */
