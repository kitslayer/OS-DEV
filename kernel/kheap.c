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

/* KASAN reports go to the kernel console; the host heap test (KHEAP_HOST_TEST)
 * has no console, so make kprintf a no-op there (it checks the counters). */
#ifdef KHEAP_HOST_TEST
#define kprintf(...) ((void)0)
#else
#include "console.h"
#endif

#define KHEAP_BASE        0xFFFF900000000000ull
#define KHEAP_GROW_PAGES  16                       /* grow 64 KiB at a time */

/* The heap's base virtual address. A variable (initialized to the fixed
 * higher-half base) rather than a bare constant only so the host heap test can
 * point the allocator at a real arena it can write to — the kernel never
 * changes it, so kernel behavior is identical to the constant. */
static uint64_t kheap_base = KHEAP_BASE;

#define BLK_MAGIC 0xA110C8EDu   /* "ALLOCED": set on an allocated block, cleared on free, so kfree detects a double-free / bad-pointer free instead of silently corrupting the list */

/* KASAN-lite (M1201): catch the two heap bugs the BLK_MAGIC sentinel can't —
 * buffer OVERFLOW (a write past the requested size) and (partially) USE-AFTER-
 * FREE. Each allocation carries a REDZONE of poison bytes immediately after the
 * user's `req` bytes, verified on free; freed memory is poisoned so a stale read
 * returns obvious garbage. A violation logs to dmesg + bumps a counter
 * (/proc/kasan) rather than panicking, so it's a non-fatal detector. */
#define KASAN_REDZONE   16
#define KASAN_RED_BYTE  0xBEu       /* "BE": the inviolable redzone pattern */
#define KASAN_FREE_BYTE 0xDEu       /* "DE": freed-memory poison */
#define KASAN_POISON_MAX 256        /* cap free-poison cost on large frees */

typedef struct block {
    uint64_t      size;     /* usable payload bytes, excluding this header */
    struct block *next;
    uint32_t      free;
    uint32_t      magic;    /* BLK_MAGIC iff currently allocated */
    uint64_t      req;      /* KASAN: requested user bytes; the redzone starts at payload[req] (M1201) */
} block_t;

static uint64_t kasan_overflows;    /* heap overflows caught at free (M1201) */
static uint64_t kasan_checks;       /* allocations redzone-checked at free */
void kheap_kasan_stats(uint64_t *overflows, uint64_t *checks) {
    if (overflows) *overflows = kasan_overflows;
    if (checks)    *checks    = kasan_checks;
}

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

/* Map [from, to) of heap virtual space to fresh physical frames. Returns 0 on
 * success, -1 on OOM (no frame, or vmm_map couldn't allocate a page table).
 * Without the check, vmm_map would alias the frame-0 sentinel as a heap page —
 * silently corrupting whatever lives at physical 0. On failure we free the frame
 * that didn't map; any pages mapped earlier in this same call leak their frames,
 * an acceptable cost on a path only reached when RAM is already exhausted. */
static int map_range(uint64_t from, uint64_t to) {
    for (uint64_t v = from; v < to; v += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) return -1;
        /* PTE_NX: the kernel heap holds data + task stacks, never executable code
         * (the eBPF JIT / module loader use the .jitexec section, not the heap), so
         * marking every heap page no-execute makes kmalloc'd task stacks NX too —
         * injected bytes on a kernel stack/heap can't be run (W^X). */
        if (vmm_map(v, frame, PTE_WRITABLE | PTE_NX) != 0) { pmm_free_frame(frame); return -1; }
    }
    return 0;
}

void kheap_init(void) {
    uint64_t bytes = KHEAP_GROW_PAGES * PAGE_SIZE;
    map_range(kheap_base, kheap_base + bytes);
    heap_end = kheap_base + bytes;

    head = (block_t *)kheap_base;
    head->size = bytes - sizeof(block_t);
    head->next = NULL;
    head->free = 1;
    head->magic = 0;        /* free block: no live-allocation sentinel */
}

/* Add more mapped pages and append a free block covering them.
 * Returns 0 on success, -1 on OOM (heap left exactly as it was, nothing appended). */
static int grow_heap(uint64_t need_bytes) {
    uint64_t grow = align_page(need_bytes + sizeof(block_t));
    if (grow < KHEAP_GROW_PAGES * PAGE_SIZE)
        grow = KHEAP_GROW_PAGES * PAGE_SIZE;

    if (map_range(heap_end, heap_end + grow) != 0)
        return -1;              /* OOM: leave heap_end + the free list untouched */
    block_t *nb = (block_t *)heap_end;
    nb->size = grow - sizeof(block_t);
    nb->next = NULL;
    nb->free = 1;
    nb->magic = 0;          /* free block */
    heap_end += grow;

    block_t *last = head;
    while (last->next)
        last = last->next;
    last->next = nb;

    /* Merge nb with its PHYSICAL predecessor (the block ending at the old
     * heap_end) if that's free. Scan for it rather than assuming it's the
     * list-tail `last` we just appended to — list order != address order, so the
     * old `last`-only test missed the merge whenever the tail wasn't physically
     * last, leaving a split free pair at the grow boundary (mirrors kfree). */
    for (block_t *p = head; p; p = p->next) {
        if (p->free && p != nb && (uint8_t *)p + sizeof(block_t) + p->size == (uint8_t *)nb) {
            p->size += sizeof(block_t) + nb->size;
            if (p->next == nb) p->next = nb->next;
            else { block_t *lp = head; while (lp && lp->next != nb) lp = lp->next; if (lp) lp->next = nb->next; }
            break;
        }
    }
    return 0;
}

void *kmalloc(size_t size) {
    if (size > (size_t)-1 - sizeof(block_t) - KASAN_REDZONE - 32) return 0;   /* reject a size so large that align16()/the `need + header + redzone` math would wrap -> under-allocation -> heap overflow on use */
    if (size == 0) size = 1;                                  /* malloc(0): hand back a real (min) block, not a 0-usable-byte one whose first write clobbers the next header */
    uint64_t need = align16(size + KASAN_REDZONE);            /* room for the user's bytes + the KASAN redzone */
    uint64_t f = irq_save();

    for (block_t *b = head; b; b = b->next) {
        if (!b->free || b->size < need)
            continue;

        /* Split if there's room for another usable block afterward. */
        if (b->size >= need + sizeof(block_t) + 16) {
            block_t *nb = (block_t *)((uint8_t *)b + sizeof(block_t) + need);
            nb->size = b->size - need - sizeof(block_t);
            nb->free = 1;
            nb->magic = 0;          /* the split-off remainder is a free block */
            nb->next = b->next;
            b->next = nb;
            b->size = need;
        }
        b->free = 0;
        b->magic = BLK_MAGIC;       /* mark live so a double-free / bad-pointer free is caught in kfree */
        b->req = size;              /* KASAN: remember the user size so kfree knows where the redzone is */
        void *p = (uint8_t *)b + sizeof(block_t);
        memset((uint8_t *)p + size, KASAN_RED_BYTE, KASAN_REDZONE);   /* KASAN redzone, just past the user's bytes */
        irq_restore(f);
        return p;
    }

    if (grow_heap(need) != 0) { irq_restore(f); return 0; }   /* OOM: report failure, don't recurse forever */
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
    if (b->magic != BLK_MAGIC) { irq_restore(f); return; }   /* not a live allocation: a double-free or a bad/interior pointer -> ignore it rather than write `free=1` into the middle of a live block / link garbage into the list */

    /* KASAN: verify the redzone just past the user's `req` bytes is intact — a
     * corrupted byte means the code wrote past the end of its allocation. */
    kasan_checks++;
    {
        uint8_t *pay = (uint8_t *)ptr;
        for (uint32_t i = 0; i < KASAN_REDZONE; i++)
            if (pay[b->req + i] != KASAN_RED_BYTE) {
                kasan_overflows++;
                kprintf("[kasan] heap buffer overflow: %lu-byte object at %p written >= %lu byte(s) past end\n",
                        (unsigned long)b->req, ptr, (unsigned long)(i + 1));
                break;
            }
    }

    b->magic = 0;            /* clear so a second free of this same pointer is detected */
    b->free = 1;

    /* KASAN free-poison: stale reads of this freed block now return 0xDE garbage
     * (use-after-free reads become obvious) instead of the last live data.
     * Bounded by KASAN_POISON_MAX so freeing a large block stays cheap. */
    {
        uint64_t pn = b->size < KASAN_POISON_MAX ? b->size : KASAN_POISON_MAX;
        memset(ptr, KASAN_FREE_BYTE, pn);
    }

    /* Coalesce with following free, adjacent blocks. */
    while (b->next && b->next->free &&
           (uint8_t *)b + sizeof(block_t) + b->size == (uint8_t *)b->next) {
        b->size += sizeof(block_t) + b->next->size;
        b->next = b->next->next;
    }

    /* Coalesce BACKWARD too: a free block sitting physically immediately before b
     * should absorb it. The list isn't address-ordered (kfree can splice in any
     * order), so scan for the physical predecessor `p` and splice b out of the
     * list. Without this, freeing N while N-1 is already free left two adjacent
     * free blocks unmerged -> the heap fragments/grows over a long run even with
     * free space available. (b==head has no predecessor, so it's never matched.) */
    for (block_t *p = head; p; p = p->next) {
        if (p->free && (uint8_t *)p + sizeof(block_t) + p->size == (uint8_t *)b) {
            p->size += sizeof(block_t) + b->size;            /* p absorbs b's header + payload */
            if (p->next == b) p->next = b->next;             /* unlink b (common case: p links to b) */
            else { block_t *lp = head; while (lp && lp->next != b) lp = lp->next; if (lp) lp->next = b->next; }
            break;                                           /* at most one physical predecessor */
        }
    }
    irq_restore(f);
}
