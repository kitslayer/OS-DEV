/*
 * umalloc.c — malloc/free/calloc/realloc for userspace, over the SYS_sbrk heap.
 *
 * A first-fit free list of address-ordered chunks; each chunk has a 32-byte
 * header (which keeps payloads 16-aligned). free() coalesces with the adjacent
 * chunks — SYS_sbrk grows the heap contiguously, so neighbouring chunks are
 * always physically adjacent and safe to merge, which keeps fragmentation in
 * check across malloc/free churn (DOOM's zone allocator and WAD caching lean on
 * this). One task per program, so no locking is needed.
 *
 * Kept free of ulib.h / syscalls so the host suite (tests/heap) can compile it
 * directly against a mock sbrk under ASan/UBSan. sbrk + the mem primitives live
 * in ulib.c on the target; on the host they come from the test / libc.
 */
extern void *sbrk(long inc);
extern void *memset(void *dst, int c, unsigned long n);
extern void *memcpy(void *dst, const void *src, unsigned long n);

void *malloc(unsigned long n);
void  free(void *p);
void *calloc(unsigned long nmemb, unsigned long size);
void *realloc(void *p, unsigned long n);

typedef struct chunk {
    unsigned long size;     /* total bytes including this header (always a multiple of 16) */
    struct chunk *next;     /* next chunk by address, or NULL for the last */
    unsigned long used;     /* 1 = allocated, 0 = free */
    unsigned long _pad;     /* pads the header to 32 bytes (payloads stay 16-aligned) */
} chunk_t;

#define HDR_SZ      ((unsigned long)sizeof(chunk_t))   /* 32 */
#define ALIGN16(n)  (((n) + 15ul) & ~15ul)
#define MIN_SPLIT   (HDR_SZ + 16ul)                    /* don't split off a chunk smaller than this */

static chunk_t *heap_head;

/* Extend the heap via sbrk and return a free chunk covering >= `need` bytes. */
static chunk_t *heap_grow(unsigned long need) {
    unsigned long amount = need < 65536ul ? 65536ul : need;
    amount = (amount + 4095ul) & ~4095ul;              /* whole pages */
    void *p = sbrk((long)amount);
    if (p == (void *)-1) return 0;
    chunk_t *c = (chunk_t *)p;
    c->size = amount; c->used = 0; c->next = 0;
    if (!heap_head) { heap_head = c; return c; }
    chunk_t *t = heap_head;
    while (t->next) t = t->next;
    if (!t->used && (unsigned char *)t + t->size == (unsigned char *)c) {
        t->size += amount;                             /* the new region abuts a free tail: merge */
        return t;
    }
    t->next = c;
    return c;
}

void *malloc(unsigned long n) {
    if (n == 0) n = 1;
    unsigned long need = HDR_SZ + ALIGN16(n);
    if (need < n) return 0;                             /* size overflow */
    chunk_t *c = heap_head;
    while (c && !(!c->used && c->size >= need)) c = c->next;
    if (!c) { c = heap_grow(need); if (!c) return 0; }
    if (c->size >= need + MIN_SPLIT) {                 /* split the remainder into a new free chunk */
        chunk_t *nc = (chunk_t *)((unsigned char *)c + need);
        nc->size = c->size - need; nc->used = 0; nc->next = c->next;
        c->size = need; c->next = nc;
    }
    c->used = 1;
    return (unsigned char *)c + HDR_SZ;
}

void free(void *p) {
    if (!p) return;
    chunk_t *c = (chunk_t *)((unsigned char *)p - HDR_SZ);
    chunk_t *prev = 0, *it = heap_head;
    while (it && it != c) { prev = it; it = it->next; }
    if (!it) return;                                   /* not one of ours */
    c->used = 0;
    if (c->next && !c->next->used &&
        (unsigned char *)c + c->size == (unsigned char *)c->next) {
        c->size += c->next->size; c->next = c->next->next;   /* merge forward */
    }
    if (prev && !prev->used &&
        (unsigned char *)prev + prev->size == (unsigned char *)c) {
        prev->size += c->size; prev->next = c->next;         /* merge backward */
    }
}

void *calloc(unsigned long nmemb, unsigned long size) {
    unsigned long n = nmemb * size;
    if (nmemb && n / nmemb != size) return 0;          /* overflow */
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void *realloc(void *p, unsigned long n) {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return 0; }
    chunk_t *c = (chunk_t *)((unsigned char *)p - HDR_SZ);
    unsigned long avail = c->size - HDR_SZ;
    if (avail >= n) return p;                          /* the existing payload already fits */
    void *np = malloc(n);
    if (!np) return 0;
    memcpy(np, p, avail);
    free(p);
    return np;
}
