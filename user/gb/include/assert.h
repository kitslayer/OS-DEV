/* assert.h — DOOM libc shim. Asserts print and exit (rare on the hot path). */
#ifndef _OSDEV_ASSERT_H
#define _OSDEV_ASSERT_H

void __doom_assert_fail(const char *expr, const char *file, int line);

#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) ((x) ? (void)0 : __doom_assert_fail(#x, __FILE__, __LINE__))
#endif

#endif /* _OSDEV_ASSERT_H */
