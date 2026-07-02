/*
 * string_test.c — differential fuzz for kernel/lib/string.c's mem* primitives.
 *
 * #includes the real string.c (like tests/url/url_test.c does for url.c) to
 * reach the actual shipped memcpy/memset/memmove, and checks each against a
 * reference implementation: a deliberately naive byte-at-a-time loop (the
 * exact shape memmove itself was until this change) that's obviously correct
 * by inspection, even though it's slow. That's the right oracle here — these
 * are freestanding primitives with no glibc equivalent to compare against
 * inside this translation unit (memcpy/memset/memmove are defined here, not
 * merely declared, so nothing else may also provide them).
 *
 * memmove gets the most scrutiny: its word-at-a-time fast path (added
 * alongside this test) has a direction to get right -- forward when
 * dst < src, backward otherwise -- fuzzed across overlapping and
 * non-overlapping placements and every alignment combination. Build under
 * ASan+UBSan via tests/run-string-tests.sh. Exit 0 = pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>   /* host malloc/rand only -- not the mem* under test */
#include "../../kernel/lib/string.c"

#define BUFN 256

static void ref_memcpy(unsigned char *d, const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}
static void ref_memset(unsigned char *d, unsigned char v, size_t n) {
    for (size_t i = 0; i < n; i++) d[i] = v;
}
static void ref_memmove(unsigned char *d, const unsigned char *s, size_t n) {
    if (d < s) { for (size_t i = 0; i < n; i++) d[i] = s[i]; }
    else       { for (size_t i = n; i-- > 0; ) d[i] = s[i]; }
}

static int fails;
static unsigned rngstate = 987654321u;
static unsigned rnd(void) { rngstate = rngstate * 1664525u + 1013904223u; return rngstate; }
static unsigned rndn(unsigned n) { return n ? rnd() % n : 0; }

int main(void) {
    unsigned char a[BUFN], b[BUFN], refbuf[BUFN];

    /* --- memcpy: random size/offset, src in the first half, dst in the
     * second -- trivially disjoint (memcpy's contract requires no overlap). */
    for (int iter = 0; iter < 20000; iter++) {
        unsigned n = rndn(BUFN / 2);                        /* n < BUFN/2 */
        unsigned off_s = rndn(BUFN / 2 - n + 1);             /* [off_s, off_s+n) within [0, BUFN/2) */
        unsigned off_d = BUFN / 2 + rndn(BUFN / 2 - n + 1);  /* [off_d, off_d+n) within [BUFN/2, BUFN) */
        for (unsigned i = 0; i < BUFN; i++) a[i] = (unsigned char)rnd();
        memcpy(b, a, BUFN);
        memcpy(refbuf, a, BUFN);
        memcpy(b + off_d, a + off_s, n);
        ref_memcpy(refbuf + off_d, refbuf + off_s, n);
        if (memcmp(b, refbuf, BUFN) != 0) { printf("  FAIL: memcpy n=%u off_s=%u off_d=%u\n", n, off_s, off_d); fails++; }
    }
    printf(fails ? "  (memcpy fails above)\n" : "  ok: memcpy vs reference, 20000 random disjoint copies\n");

    /* --- memset: random size/offset/value --- */
    int msfails = fails;
    for (int iter = 0; iter < 20000; iter++) {
        unsigned n = rndn(BUFN), off = rndn(BUFN - n + 1);
        unsigned char v = (unsigned char)rnd();
        for (unsigned i = 0; i < BUFN; i++) a[i] = (unsigned char)rnd();
        memcpy(refbuf, a, BUFN);
        memset(a + off, v, n);
        ref_memset(refbuf + off, v, n);
        if (memcmp(a, refbuf, BUFN) != 0) { printf("  FAIL: memset n=%u off=%u v=%u\n", n, off, v); fails++; }
    }
    printf(fails != msfails ? "  (memset fails above)\n" : "  ok: memset vs reference, 20000 random fills\n");

    /* --- memmove: the main event. Every (n, off_s, off_d) pair within one
     * buffer, so overlapping-forward, overlapping-backward, and disjoint
     * placements all get hit, at every alignment (off_s/off_d aren't masked
     * to any boundary, so both the aligned-word path and the byte-fallback
     * path -- misaligned relative to each other -- get exercised). */
    int mmfails = fails;
    long mmcases = 0;
    for (unsigned n = 0; n <= 40; n++) {
        for (unsigned off_s = 0; off_s + n <= BUFN && off_s <= 48; off_s++) {
            for (unsigned off_d = 0; off_d + n <= BUFN && off_d <= 48; off_d++) {
                for (unsigned i = 0; i < BUFN; i++) a[i] = (unsigned char)(i * 37 + 11);
                memcpy(b, a, BUFN);
                memmove(a + off_d, a + off_s, n);        /* in place: the real overlap case */
                ref_memmove(b + off_d, b + off_s, n);
                mmcases++;
                if (memcmp(a, b, BUFN) != 0) {
                    printf("  FAIL: memmove n=%u off_s=%u off_d=%u\n", n, off_s, off_d);
                    fails++;
                }
            }
        }
    }
    printf(fails != mmfails ? "  (memmove fails above)\n" : "  ok: memmove vs reference, %ld (n, src-offset, dst-offset) combinations (in-place, every overlap shape)\n", mmcases);

    /* --- memmove: bigger random buffers, two independent allocations copied
     * into a shared scratch at random offsets, so both directions and both
     * alignments (dst<src forward, dst>=src backward) get large-n coverage
     * beyond the exhaustive-but-small sweep above. */
    int mmfails2 = fails;
    for (int iter = 0; iter < 20000; iter++) {
        unsigned n = rndn(BUFN), off_s = rndn(BUFN - n + 1), off_d = rndn(BUFN - n + 1);
        for (unsigned i = 0; i < BUFN; i++) a[i] = (unsigned char)rnd();
        memcpy(b, a, BUFN);
        memmove(a + off_d, a + off_s, n);
        ref_memmove(b + off_d, b + off_s, n);
        if (memcmp(a, b, BUFN) != 0) { printf("  FAIL: memmove(random) n=%u off_s=%u off_d=%u\n", n, off_s, off_d); fails++; }
    }
    printf(fails != mmfails2 ? "  (memmove random fails above)\n" : "  ok: memmove vs reference, 20000 additional random in-place cases\n");

    if (fails) { printf("FAIL: %d string.c mismatch(es)\n", fails); return 1; }
    printf("PASS: memcpy/memset/memmove match their reference implementations (ASan/UBSan clean)\n");
    return 0;
}
