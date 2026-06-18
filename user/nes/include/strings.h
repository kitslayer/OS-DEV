/* strings.h — DOOM libc shim (case-insensitive compares). */
#ifndef _OSDEV_STRINGS_H
#define _OSDEV_STRINGS_H

#include <stddef.h>

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

#endif /* _OSDEV_STRINGS_H */
