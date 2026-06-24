/* kheap.h — the kernel's dynamic allocator (malloc/free for kernel code). */
#pragma once
#include <stddef.h>
#include <stdint.h>

void  kheap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);   /* kmalloc + zero */
void  kfree(void *ptr);

/* KASAN-lite (M1201): how many heap buffer-overflows were caught at free, and
 * how many allocations were redzone-checked. Surfaced at /proc/kasan. */
void  kheap_kasan_stats(uint64_t *overflows, uint64_t *checks);
