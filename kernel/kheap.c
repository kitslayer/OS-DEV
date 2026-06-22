/*
 * kheap.c — a simple but real kernel heap (kmalloc/kfree).
 *
 * The PMM hands out whole 4 KiB frames; the VMM maps them. But kernel code
 * wants to allocate a 40-byte struct, or a 300-byte buffer. The heap bridges
 * that gap: it grabs pages from the VMM and carves them into variable-sized
 * blocks.
 *
 * Design: a singly linked list of contiguous blocks, each with a small header
 * { size, free, next }. Allocation is first-fit with splitting; freeing
 * coalesces with the following block. When nothing fits, we grow the heap by
 * mapping more pages at its end. Not the fastest design, but easy to read and
 * entirely sufficient for a kernel.
 *
 * The heap lives in its own higher-half virtual region, backed by frames the
 * VMM maps on demand.
 */
#include "kheap.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include <stdint.h>

#define KHEAP_BASE        0xFFFF900000000000ull
#define KHEAP_GROW_PAGES  16                       /* grow 64 KiB at a time */

/* The heap's base virtual address. A variable (initialized to the fixed
 * higher-half base) rather than a bare constant only so the host heap test can
 * point the allocator at a real arena it can write to — the kernel never
 * changes it, so kernel behavior is identical to the constant. */
static uint64_t kheap_base = KHEAP_BASE;

typedef struct block {
    uint64_t      size;     /* usable payload bytes, excluding this header */
    struct block *next;
    uint32_t      free;
} block_t;

static block_t  *head;
static uint64_t  heap_end;  /* first virtual address not yet mapped */

static inline uint64_t align16(uint64_t x) { return (x + 15) & ~15ull; }
static inline uint64_t align_page(uint64_t x) {
    return (x + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

/* The free list is shared mutable state, so every allocation/free must be
 * atomic against preemption: a timer IRQ that switched tasks mid-edit could let
 * another task walk a half-linked list (e.g. the WM frees an app's stack with
 * interrupts on while an app's syscall allocates). These save RFLAGS + disable
 * interrupts, then restore the caller's prior state — so they nest correctly and
 * are safe whether the caller already had interrupts off (syscalls) or on. */
static inline uint64_t irq_save(void) {
    uint64_t fl;
#ifdef KHEAP_HOST_TEST
    __asm__ volatile("pushfq; pop %0" : "=r"(fl) :: "memory");  /* host: cli is privileged (ring 3) */
#else
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
#endif
    return fl;
}
static inline void irq_restore(uint64_t fl) {
    __asm__ volatile("push %0; popfq" : : "r"(fl) : "memory", "cc");
}

/* Map [from, to) of heap virtual space to fresh physical frames. */
static void map_range(uint64_t from, uint64_t to) {
    for (uint64_t v = from; v < to; v += PAGE_SIZE)
        vmm_map(v, pmm_alloc_frame(), PTE_WRITABLE);
}

void kheap_init(void) {
    uint64_t bytes = KHEAP_GROW_PAGES * PAGE_SIZE;
    map_range(kheap_base, kheap_base + bytes);
    heap_end = kheap_base + bytes;

    head = (block_t *)kheap_base;
    head->size = bytes - sizeof(block_t);
    head->next = NULL;
    head->free = 1;
}

/* Add more mapped pages and append a free block covering them. */
static void grow_heap(uint64_t need_bytes) {
    uint64_t grow = align_page(need_bytes + sizeof(block_t));
    if (grow < KHEAP_GROW_PAGES * PAGE_SIZE)
        grow = KHEAP_GROW_PAGES * PAGE_SIZE;

    map_range(heap_end, heap_end + grow);
    block_t *nb = (block_t *)heap_end;
    nb->size = grow - sizeof(block_t);
    nb->next = NULL;
    nb->free = 1;
    heap_end += grow;

    block_t *last = head;
    while (last->next)
        last = last->next;
    last->next = nb;

    /* If the previous last block was free and physically adjacent, merge. */
    if (last->free &&
        (uint8_t *)last + sizeof(block_t) + last->size == (uint8_t *)nb) {
        last->size += sizeof(block_t) + nb->size;
        last->next = nb->next;
    }
}

void *kmalloc(size_t size) {
    if (size > (size_t)-1 - sizeof(block_t) - 32) return 0;   /* reject a size so large that align16()/the `need + header` math would wrap -> under-allocation -> heap overflow on use */
    if (size == 0) size = 1;                                  /* malloc(0): hand back a real (min) block, not a 0-usable-byte one whose first write clobbers the next header */
    uint64_t need = align16(size);
    uint64_t f = irq_save();

    for (block_t *b = head; b; b = b->next) {
        if (!b->free || b->size < need)
            continue;

        /* Split if there's room for another usable block afterward. */
        if (b->size >= need + sizeof(block_t) + 16) {
            block_t *nb = (block_t *)((uint8_t *)b + sizeof(block_t) + need);
            nb->size = b->size - need - sizeof(block_t);
            nb->free = 1;
            nb->next = b->next;
            b->next = nb;
            b->size = need;
        }
        b->free = 0;
        void *p = (uint8_t *)b + sizeof(block_t);
        irq_restore(f);
        return p;
    }

    grow_heap(need);
    void *p = kmalloc(size);   /* one retry; the new block will fit (nested
                                * irq_save is a no-op while we hold IF off) */
    irq_restore(f);
    return p;
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *ptr) {
    if (!ptr)
        return;
    uint64_t f = irq_save();
    block_t *b = (block_t *)((uint8_t *)ptr - sizeof(block_t));
    b->free = 1;

    /* Coalesce with following free, adjacent blocks. */
    while (b->next && b->next->free &&
           (uint8_t *)b + sizeof(block_t) + b->size == (uint8_t *)b->next) {
        b->size += sizeof(block_t) + b->next->size;
        b->next = b->next->next;
    }
    irq_restore(f);
}
