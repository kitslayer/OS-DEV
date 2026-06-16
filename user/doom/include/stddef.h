/* stddef.h — minimal freestanding-ish definitions for the DOOM libc shim. */
#ifndef _OSDEV_STDDEF_H
#define _OSDEV_STDDEF_H

#ifndef NULL
#define NULL ((void *)0)
#endif

typedef unsigned long size_t;
typedef long          ssize_t;
typedef long          ptrdiff_t;

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#endif /* _OSDEV_STDDEF_H */
