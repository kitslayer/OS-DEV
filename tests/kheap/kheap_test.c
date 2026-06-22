/*
 * kheap_test.c — host-side torture + invariant test of the kernel heap
 * (ASan + UBSan). #includes kheap.c with the VMM/PMM stubbed out and the heap
 * base pointed at a real mmap'd arena (kheap.c keeps a kheap_base variable for
 * exactly this), so kmalloc/kfree/kzalloc run against memory the host can read
 * and write.
 *
 * The kernel heap underlies every kernel allocation, so a split/coalesce/grow
 * bug corrupts arbitrary kernel state. This test:
 *   - runs hundreds of thousands of random alloc/free operations, each live
 *     block filled with a unique byte pattern that is re-verified on every
 *     pass — overlapping or corrupted blocks are caught immediately;
 *   - checks kzalloc returns zeroed memory and respects requested sizes;
 *   - forces many grow_heap() extensions;
 *   - walks the block list afterward and asserts it tiles the heap exactly
 *     (each block adjacent to the next, all within [base, heap_end), no gaps).
 * Exit 0 = pass.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

/* ---- stubs for kheap.c's external deps ----
 * The arena is one big pre-backed mmap, so vmm_map is a no-op and
 * pmm_alloc_frame just hands out distinct dummy physical addresses. */
int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)virt; (void)phys; (void)flags; return 0;
}
static uint64_t g_frame = 0x200000;
uint64_t pmm_alloc_frame(void) { g_frame += 4096; return g_frame; }

#define KHEAP_HOST_TEST   /* neutralize the privileged cli in irq_save on the host */
#include "kheap.c"

#define ARENA_BYTES (64ull * 1024 * 1024)

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", (msg)); fails++; } } while (0)

/* a live allocation we track for corruption */
struct live { uint8_t *p; size_t n; uint8_t pat; };

static void fillpat(uint8_t *p, size_t n, uint8_t pat) { for (size_t i = 0; i < n; i++) p[i] = pat; }
static int  chkpat(const uint8_t *p, size_t n, uint8_t pat) {
    for (size_t i = 0; i < n; i++) if (p[i] != pat) return 0;
    return 1;
}

/* Walk the free-list and confirm it tiles [base, heap_end) with no gaps and
 * every block inside the arena. */
static void check_invariants(void) {
    uint64_t base = kheap_base;
    block_t *b = head;
    uint64_t addr = base;
    int guard = 0;
    while (b) {
        CHECK((uint64_t)b == addr, "block not contiguous with previous");
        CHECK((uint64_t)b >= base && (uint64_t)b + sizeof(block_t) + b->size <= heap_end,
              "block escapes [base, heap_end)");
        addr = (uint64_t)b + sizeof(block_t) + b->size;
        b = b->next;
        if (++guard > 5000000) { CHECK(0, "block list appears cyclic"); return; }
    }
    CHECK(addr == heap_end, "blocks do not tile up to heap_end");
}

int main(void) {
    void *arena = mmap(NULL, ARENA_BYTES, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) { perror("mmap"); return 1; }
    kheap_base = (uint64_t)arena;
    kheap_init();

    /* kzalloc returns zeroed memory */
    for (int t = 0; t < 64; t++) {
        size_t n = (size_t)(t * 37 + 1);
        uint8_t *z = kzalloc(n);
        CHECK(z != NULL, "kzalloc returned NULL");
        CHECK(chkpat(z, n, 0), "kzalloc memory not zeroed");
        kfree(z);
    }

    /* random alloc/free torture with per-block pattern verification */
    #define MAXLIVE 2048
    static struct live live[MAXLIVE];
    int nlive = 0;
    srand(20240617);
    for (int op = 0; op < 400000; op++) {
        int do_alloc = (nlive == 0) || (nlive < MAXLIVE && (rand() & 1));
        if (do_alloc) {
            size_t n = (size_t)(rand() % 1024) + 1;     /* mix of small + split sizes */
            if ((rand() % 50) == 0) n = (size_t)(rand() % 70000) + 1;  /* occasional big -> grow */
            uint8_t *p = kmalloc(n);
            CHECK(p != NULL, "kmalloc returned NULL");
            if (!p) continue;
            CHECK((uint64_t)p >= kheap_base && (uint64_t)p + n <= heap_end,
                  "allocation outside the heap");
            uint8_t pat = (uint8_t)(op ^ (op >> 8) ^ 0xA5);
            fillpat(p, n, pat);
            live[nlive].p = p; live[nlive].n = n; live[nlive].pat = pat;
            nlive++;
        } else {
            int i = rand() % nlive;
            CHECK(chkpat(live[i].p, live[i].n, live[i].pat), "live block corrupted before free");
            kfree(live[i].p);
            live[i] = live[--nlive];
        }
        /* periodically verify EVERY live block is still intact (overlap check) */
        if ((op & 0x3FFF) == 0) {
            for (int i = 0; i < nlive; i++)
                if (!chkpat(live[i].p, live[i].n, live[i].pat)) { CHECK(0, "overlap: live block corrupted"); break; }
            check_invariants();
        }
        if (fails) break;
    }
    /* final verify + free everything, then confirm the heap coalesces back */
    for (int i = 0; i < nlive; i++)
        CHECK(chkpat(live[i].p, live[i].n, live[i].pat), "live block corrupted at end");
    for (int i = 0; i < nlive; i++) kfree(live[i].p);
    check_invariants();

    /* A double-free and a bad/interior-pointer free are detected via BLK_MAGIC
     * and IGNORED — never allowed to write `free=1` into a live block or link
     * garbage into the list (the kheap header-magic, M972). */
    {
        uint8_t *a = kmalloc(100); CHECK(a != NULL, "df: kmalloc a");
        uint8_t *b = kmalloc(100); CHECK(b != NULL, "df: kmalloc b");
        if (b) fillpat(b, 100, 0x5A);
        kfree(a);                              /* legitimate free */
        kfree(a);                              /* DOUBLE free of the same pointer -> must be ignored */
        check_invariants();
        if (b) CHECK(chkpat(b, 100, 0x5A), "df: double-free corrupted a live block");
        uint8_t junk[64] = {0};
        kfree(junk + 32);                      /* a non-heap / interior pointer -> magic mismatch -> ignored */
        check_invariants();
        if (b) CHECK(chkpat(b, 100, 0x5A), "df: bad-pointer free corrupted a live block");
        uint8_t *c = kmalloc(100); CHECK(c != NULL, "df: alloc after the double-free still works");
        if (b) kfree(b);
        if (c) kfree(c);
        check_invariants();
        printf("double-free/bad-pointer free: detected + ignored, heap intact\n");
    }

    printf("torture: 400000 random alloc/free ops, pattern + tiling invariants -> %s\n",
           fails ? "FAILURES" : "all intact");
    if (fails) { printf("FAIL: %d check(s) failed\n", fails); return 1; }
    printf("PASS: kernel heap (split/coalesce/grow torture, ASan/UBSan clean)\n");
    return 0;
}
