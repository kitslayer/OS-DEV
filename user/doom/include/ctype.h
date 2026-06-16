/* ctype.h — DOOM libc shim. Implemented as functions in dlibc.c. */
#ifndef _OSDEV_CTYPE_H
#define _OSDEV_CTYPE_H

int isalpha(int c);
int isdigit(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int iscntrl(int c);
int isprint(int c);
int ispunct(int c);
int isxdigit(int c);
int isgraph(int c);
int toupper(int c);
int tolower(int c);

#endif /* _OSDEV_CTYPE_H */
