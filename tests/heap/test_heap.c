/*
 * test_heap.c — host-side regression test for the userspace allocator
 * (user/umalloc.c), exercised over a mock sbrk under ASan + UBSan.
 *
 * umalloc.c is compiled with -Dsbrk=test_sbrk -Dmalloc=t_malloc -Dfree=t_free
 * -Dcalloc=t_calloc -Drealloc=t_realloc, so we drive the real allocator while
 * leaving libc's malloc untouched for the test harness itself. The mock sbrk
 * grows a single page-aligned arena contiguously, exactly like the kernel's
 * SYS_sbrk, so the allocator's adjacency/coalescing assumptions are tested.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

void *t_malloc(unsigned long);
void  t_free(void *);
void *t_calloc(unsigned long, unsigned long);
void *t_realloc(void *, unsigned long);

#define ARENA (64ul * 1024 * 1024)
_Alignas(4096) static unsigned char arena[ARENA];
static unsigned long brk_off;
unsigned long g_sbrk_total;

void *test_sbrk(long inc) {
    if (inc <= 0) return arena + brk_off;
    if (brk_off + (unsigned long)inc > ARENA) return (void *)-1;   /* OOM */
    void *old = arena + brk_off;
    brk_off += (unsigned long)inc;
    g_sbrk_total = brk_off;
    return old;
}

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* fill a block with a pattern keyed by `seed`, and verify it later */
static void fill(unsigned char *p, unsigned long n, unsigned seed) {
    for (unsigned long i = 0; i < n; i++) p[i] = (unsigned char)(seed * 31u + i);
}
static int verify(const unsigned char *p, unsigned long n, unsigned seed) {
    for (unsigned long i = 0; i < n; i++)
        if (p[i] != (unsigned char)(seed * 31u + i)) return 0;
    return 1;
}

int main(void) {
    /* 1. alignment + basic round-trip */
    for (unsigned long s = 1; s <= 5000; s += 7) {
        unsigned char *p = t_malloc(s);
        CHECK(p != NULL);
        CHECK(((uintptr_t)p & 15u) == 0);     /* 16-aligned payloads */
        fill(p, s, (unsigned)s);
        CHECK(verify(p, s, (unsigned)s));
        t_free(p);
    }

    /* 2. calloc zeroes; overflow guarded */
    unsigned char *z = t_calloc(1000, 8);
    CHECK(z != NULL);
    for (int i = 0; i < 8000; i++) CHECK(z[i] == 0);
    t_free(z);
    CHECK(t_calloc((unsigned long)-1, 2) == NULL);   /* multiply overflow -> NULL */

    /* 3. realloc grows and preserves contents */
    unsigned char *r = t_malloc(64);
    fill(r, 64, 12345);
    r = t_realloc(r, 4096);
    CHECK(r != NULL);
    CHECK(verify(r, 64, 12345));              /* old bytes survive the grow */
    t_free(r);
    CHECK(t_realloc(NULL, 32) != NULL);       /* realloc(NULL,n) == malloc(n) */

    /* 4. reuse: alloc+free in a tight loop must NOT grow the heap unboundedly.
     * (Sections 1-3 have freed everything, so the free list can satisfy these.) */
    unsigned long before = g_sbrk_total;
    for (int i = 0; i < 200000; i++) {
        unsigned char *p = t_malloc(1 + (unsigned)(i % 4000));
        CHECK(p != NULL);
        p[0] = 1; p[(i % 4000)] = 2;
        t_free(p);
    }
    CHECK(g_sbrk_total - before < 1024 * 1024);   /* freed memory is reused, not re-sbrk'd */

    /* 5. coalescing: many small blocks freed should merge into space a big one fits */
    enum { N = 2000 };
    static unsigned char *ptr[N];
    for (int i = 0; i < N; i++) { ptr[i] = t_malloc(512); CHECK(ptr[i]); fill(ptr[i], 512, (unsigned)i); }
    for (int i = 0; i < N; i++) CHECK(verify(ptr[i], 512, (unsigned)i));   /* no overlap/corruption */
    unsigned long peak = g_sbrk_total;
    for (int i = 0; i < N; i++) t_free(ptr[i]);
    unsigned char *big = t_malloc(N * 256);    /* ~half the freed bytes, as one block */
    CHECK(big != NULL);
    CHECK(g_sbrk_total <= peak + 256 * 1024);  /* fit by coalescing, not by growing much */
    t_free(big);

    /* 6. pseudo-random churn with live-set pattern verification (ASan/UBSan watch for UB) */
    static unsigned char *live[512];
    static unsigned long  lsz[512];
    static unsigned       lseed[512];
    unsigned rng = 0xC0FFEEu;
    for (int it = 0; it < 300000; it++) {
        rng = rng * 1664525u + 1013904223u;
        int slot = (rng >> 8) % 512;
        if (live[slot]) {
            CHECK(verify(live[slot], lsz[slot], lseed[slot]));
            t_free(live[slot]);
            live[slot] = NULL;
        } else {
            unsigned long n = 1 + ((rng >> 4) % 8192);
            unsigned char *p = t_malloc(n);
            CHECK(p != NULL);
            live[slot] = p; lsz[slot] = n; lseed[slot] = rng;
            fill(p, n, rng);
        }
    }
    for (int i = 0; i < 512; i++)
        if (live[i]) { CHECK(verify(live[i], lsz[i], lseed[i])); t_free(live[i]); }

    if (fails) { printf("heaptest: %d FAILURES\n", fails); return 1; }
    printf("heaptest: all checks passed\n");
    return 0;
}
