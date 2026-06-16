/*
 * tar_test.c — host round-trip + corrupt-input fuzz for the ustar extractor.
 * The runner (run-tar-tests.sh) generates /tmp/osdev_t.tar with known files via
 * python tarfile; we extract it, assert names + bytes match, then fuzz truncated
 * and corrupted variants under ASan/UBSan. Exit 0 = pass.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int tar_extract(const uint8_t *, int, void (*)(void *, const char *, int, const uint8_t *, int), void *);

static uint8_t tarbuf[2000000];

struct rec { char name[80]; const uint8_t *data; int len; };
struct ctx { struct rec r[64]; int n; };

static void emit(void *vc, const char *name, int namelen, const uint8_t *data, int datalen) {
    struct ctx *c = (struct ctx *)vc;
    /* touch every byte so ASan validates the emitted pointers/lengths */
    volatile uint8_t s = 0;
    for (int i = 0; i < namelen; i++) s ^= (uint8_t)name[i];
    for (int i = 0; i < datalen; i++) s ^= data[i];
    (void)s;
    if (c->n < 64) {
        int nl = namelen < 79 ? namelen : 79;
        memcpy(c->r[c->n].name, name, (size_t)nl); c->r[c->n].name[nl] = 0;
        c->r[c->n].data = data; c->r[c->n].len = datalen; c->n++;
    }
}

static int find(struct ctx *c, const char *name, const char *body, int blen) {
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->r[i].name, name) && c->r[i].len == blen &&
            !memcmp(c->r[i].data, body, (size_t)blen))
            return 1;
    return 0;
}

int main(void) {
    FILE *f = fopen("/tmp/osdev_t.tar", "rb");
    if (!f) { printf("FAIL: cannot open /tmp/osdev_t.tar\n"); return 1; }
    int n = (int)fread(tarbuf, 1, sizeof tarbuf, f);
    fclose(f);

    struct ctx c = { .n = 0 };
    int cnt = tar_extract(tarbuf, n, emit, &c);
    printf("tar extractor: %d-byte archive -> %d files\n", n, cnt);

    static char big[5000]; memset(big, 'X', sizeof big);
    if (!find(&c, "hello.txt", "hello tar\n", 10)) { printf("FAIL: hello.txt\n"); return 1; }
    if (!find(&c, "big.txt", big, 5000))           { printf("FAIL: big.txt\n");   return 1; }
    if (!find(&c, "sub/y.txt", "nested tar file", 15)) { printf("FAIL: sub/y.txt\n"); return 1; }
    printf("  ok: hello.txt / big.txt / sub/y.txt all match\n");

    /* fuzz: every truncated prefix must not crash / over-read */
    for (int t = 0; t <= n; t++) { struct ctx c2 = { .n = 0 }; tar_extract(tarbuf, t, emit, &c2); }
    /* fuzz: single-byte corruptions across the header region */
    for (int p = 0; p < n && p < 4096; p++) {
        uint8_t save = tarbuf[p]; tarbuf[p] = (uint8_t)(save ^ 0xFF);
        struct ctx c3 = { .n = 0 }; tar_extract(tarbuf, n, emit, &c3);
        tarbuf[p] = save;
    }
    /* fuzz: random garbage buffers */
    uint32_t rs = 0xC0FFEEu;
    for (int i = 0; i < 20000; i++) {
        int ln = (int)(rs % 2048u) + 1;
        for (int j = 0; j < ln; j++) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; tarbuf[j] = (uint8_t)rs; }
        struct ctx c4 = { .n = 0 }; tar_extract(tarbuf, ln, emit, &c4);
    }
    printf("  fuzz: truncations + single-byte corruptions + 20000 garbage buffers -> all clean\n");

    printf("PASS: tar extractor (exact extraction, fuzz/corrupt safe, ASan/UBSan clean)\n");
    return 0;
}
