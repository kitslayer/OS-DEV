/* kheap.h — the kernel's dynamic allocator (malloc/free for kernel code). */
#pragma once
#include <stddef.h>

void  kheap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);   /* kmalloc + zero */
void  kfree(void *ptr);
