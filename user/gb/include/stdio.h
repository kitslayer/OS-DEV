/* stdio.h — DOOM libc shim.
 *
 * FILE is an in-memory buffer. Read mode slurps the whole file via
 * sys_readfile; write mode accumulates into a growable buffer flushed to
 * sys_writefile on fclose/fflush. See dlibc.c for the implementation. */
#ifndef _OSDEV_STDIO_H
#define _OSDEV_STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct _FILE {
    unsigned char *buf;   /* backing data (malloc'd) */
    long           pos;   /* read/write cursor */
    long           len;   /* valid bytes in buf */
    long           cap;   /* allocated capacity of buf */
    int            mode;  /* 0 = read, 1 = write */
    int            eof;   /* EOF flag */
    int            err;   /* error flag */
    int            is_std;/* 1 = stdout, 2 = stderr (no backing buffer) */
    char          *name;  /* filename (for flush on write), malloc'd */
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#ifndef EOF
#define EOF (-1)
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#ifndef BUFSIZ
#define BUFSIZ 8192
#endif

FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int   fseek(FILE *f, long offset, int whence);
long  ftell(FILE *f);
void  rewind(FILE *f);
int   fflush(FILE *f);
int   feof(FILE *f);
int   ferror(FILE *f);
int   fgetc(FILE *f);
int   getc(FILE *f);
char *fgets(char *s, int size, FILE *f);
int   fputc(int c, FILE *f);
int   fputs(const char *s, FILE *f);
int   putc(int c, FILE *f);
int   putchar(int c);
int   puts(const char *s);
int   fileno(FILE *f);
int   remove(const char *path);
int   rename(const char *oldp, const char *newp);
void  setbuf(FILE *f, char *buf);
int   setvbuf(FILE *f, char *buf, int mode, size_t size);

int   printf(const char *fmt, ...);
int   fprintf(FILE *f, const char *fmt, ...);
int   sprintf(char *str, const char *fmt, ...);
int   snprintf(char *str, size_t size, const char *fmt, ...);
int   vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
int   vsprintf(char *str, const char *fmt, va_list ap);
int   vprintf(const char *fmt, va_list ap);
int   vfprintf(FILE *f, const char *fmt, va_list ap);
int   sscanf(const char *str, const char *fmt, ...);

#endif /* _OSDEV_STDIO_H */
