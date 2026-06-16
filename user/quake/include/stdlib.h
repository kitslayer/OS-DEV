/* stdlib.h — DOOM libc shim. */
#ifndef _OSDEV_STDLIB_H
#define _OSDEV_STDLIB_H

#include <stddef.h>

/* malloc family comes from ulib/umalloc (declared here, defined there) */
void *malloc(size_t n);
void  free(void *p);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *p, size_t n);

int    atoi(const char *s);
long   atol(const char *s);
double atof(const char *s);
long   strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);
double strtod(const char *s, char **endptr);

int    abs(int x);
long   labs(long x);

void   qsort(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *));
void  *bsearch(const void *key, void *base, size_t nmemb, size_t size,
               int (*compar)(const void *, const void *));

int    rand(void);
void   srand(unsigned int seed);

char  *getenv(const char *name);
int    system(const char *command);

void   exit(int status);
void   abort(void);
int    atexit(void (*func)(void));

#define RAND_MAX 0x7fffffff
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#endif /* _OSDEV_STDLIB_H */
