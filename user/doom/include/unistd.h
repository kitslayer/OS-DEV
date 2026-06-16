/* unistd.h — DOOM libc shim (POSIX bits DOOM references; mostly stubbed). */
#ifndef _OSDEV_UNISTD_H
#define _OSDEV_UNISTD_H

#include <stddef.h>

int   access(const char *path, int mode);
int   unlink(const char *path);
int   isatty(int fd);
int   close(int fd);
long  read(int fd, void *buf, unsigned long count);
long  write(int fd, const void *buf, unsigned long count);
long  lseek(int fd, long offset, int whence);
int   usleep(unsigned long usec);
unsigned int sleep(unsigned int seconds);
char *getcwd(char *buf, size_t size);
int   chdir(const char *path);

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#endif /* _OSDEV_UNISTD_H */
