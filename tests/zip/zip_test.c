/*
 * ZIP EXTRACTOR test (host-side, ASan/UBSan).
 *
 * Exercises kernel/zip.c (zip_extract) end to end:
 *   1. Build a REAL .zip with python3's zipfile (ZIP_DEFLATED) containing:
 *        - a small text file               ("hello.txt"      -> "hi")
 *        - a ~64KB repetitive file          ("big.txt"        -> "ABCD"*16384)
 *          (actually DEFLATE-compressed by the zip tool -> method 8)
 *        - an empty file                    ("empty.txt"      -> "")
 *        - a file inside a subdirectory      ("sub/x.txt"      -> "nested")
 *      plus a couple of extra cases (stored file, longer text), read it into
 *      memory, run zip_extract with an emit callback that records (name,data),
 *      and assert every file's name + EXACT bytes match the originals and that
 *      directory entries are not emitted.
 *   2. Fuzz: feed every truncated prefix of the real zip, plus many random and
 *      single-byte-corrupted variants, to zip_extract and assert it always
 *      returns cleanly (-1 or a sane count) with NO ASan/UBSan error and no
 *      hang. The scratch buffer is placed at the end of a heap allocation so any
 *      over-write is immediately flagged by ASan.
 *
 * Build/run: see tests/run-zip-tests.sh, or:
 *   gcc -std=gnu11 -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
 *       -Ikernel/include tests/zip/zip_test.c kernel/zip.c kernel/inflate.c \
 *       -o /tmp/ziptest && /tmp/ziptest
 * Clean exit + "PASS" line = success; any OOB aborts under the sanitizers.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zip.h"

#define SCRATCH_CAP (1 << 20)        /* 1 MiB, comfortably > any test file */

/* ---- emit callback: record each (name,data) pair into a table ---- */
typedef struct {
    char     name[256];
    int      namelen;
    uint8_t *data;
    int      datalen;
} entry_t;

typedef struct {
    entry_t ent[64];
    int     count;
} recorder_t;

/* A volatile sink so the compiler can't elide the validation reads below. */
static volatile uint8_t g_sink;

static void rec_emit(void *ctx, const char *name, int namelen,
                     const uint8_t *data, int datalen) {
    recorder_t *r = (recorder_t *)ctx;
    /* zip_extract's contract: namelen/datalen >= 0 and every one of those bytes
     * is readable. Touch them ALL so ASan/UBSan verifies the pointers really are
     * in bounds (a fuzzed archive can legitimately yield a long but valid name).
     * Negative lengths would be a contract violation -> abort. */
    if (namelen < 0 || datalen < 0) {
        fprintf(stderr, "bad len: namelen=%d datalen=%d\n", namelen, datalen);
        abort();
    }
    uint8_t acc = 0;
    for (int i = 0; i < namelen; i++) acc ^= (uint8_t)name[i];
    for (int i = 0; i < datalen; i++) acc ^= data[i];
    g_sink = acc;

    if (r->count >= (int)(sizeof r->ent / sizeof r->ent[0])) {
        fprintf(stderr, "recorder overflow\n"); abort();
    }
    entry_t *e = &r->ent[r->count++];
    /* Store a NUL-terminated, length-clamped copy for name comparison. A name
     * longer than the buffer simply won't equal any expected (short) name. */
    int keep = namelen;
    if (keep > (int)sizeof e->name - 1) keep = (int)sizeof e->name - 1;
    memcpy(e->name, name, (size_t)keep);
    e->name[keep] = '\0';
    e->namelen = namelen;
    e->datalen = datalen;
    e->data = malloc(datalen ? (size_t)datalen : 1);
    if (datalen) memcpy(e->data, data, (size_t)datalen);
}

static void rec_free(recorder_t *r) {
    for (int i = 0; i < r->count; i++) free(r->ent[i].data);
    r->count = 0;
}

/* Find a recorded entry by name; NULL if absent. */
static const entry_t *rec_find(const recorder_t *r, const char *name) {
    for (int i = 0; i < r->count; i++)
        if (strcmp(r->ent[i].name, name) == 0) return &r->ent[i];
    return NULL;
}

static int fails = 0;

/* Assert a named entry exists and matches exactly. */
static void expect(const recorder_t *r, const char *name,
                   const uint8_t *want, int wantlen) {
    const entry_t *e = rec_find(r, name);
    if (!e) {
        printf("  FAIL: entry '%s' not emitted\n", name); fails++; return;
    }
    if (e->namelen != (int)strlen(name)) {
        printf("  FAIL: '%s' namelen: got %d want %zu\n",
               name, e->namelen, strlen(name)); fails++; return;
    }
    if (e->datalen != wantlen) {
        printf("  FAIL: '%s' length: got %d want %d\n", name, e->datalen, wantlen);
        fails++; return;
    }
    if (wantlen && memcmp(e->data, want, (size_t)wantlen) != 0) {
        printf("  FAIL: '%s' bytes differ\n", name); fails++; return;
    }
    printf("  ok: '%s' (%d bytes) matches\n", name, wantlen);
}

/* Read an entire file into a malloc'd buffer (exact size for ASan red-zones). */
static uint8_t *read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n ? (size_t)n : 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *out_len = n;
    return buf;
}

static uint32_t rs = 0xC0FFEEu;
static uint32_t xr(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(int argc, char **argv) {
    /* Optional argv[1] reseeds the fuzz RNG so re-runs cover different mutation
     * sets (the single-byte pass is exhaustive regardless of seed). */
    if (argc > 1) { rs = (uint32_t)strtoul(argv[1], NULL, 0); if (!rs) rs = 1; }
    printf("zip extractor test (fuzz seed 0x%08x)\n", rs);

    /* --- Build the test archive with python3 (ZIP_DEFLATED + one stored). The
     * big repetitive file compresses well, exercising the method-8 path; the
     * stored file exercises method 0; sub/x.txt exercises a subdir path; and an
     * explicit directory entry confirms we skip it. */
    const char *zip_path = "/tmp/osdev_ziptest.zip";
    char cmd[2048];
    snprintf(cmd, sizeof cmd,
        "python3 - <<'PY'\n"
        "import zipfile\n"
        "z=zipfile.ZipFile('%s','w')\n"
        "z.writestr(zipfile.ZipInfo('hello.txt'), b'hi', zipfile.ZIP_DEFLATED)\n"
        "z.writestr(zipfile.ZipInfo('big.txt'),   b'ABCD'*16384, zipfile.ZIP_DEFLATED)\n"
        "z.writestr(zipfile.ZipInfo('empty.txt'), b'', zipfile.ZIP_DEFLATED)\n"
        "z.writestr(zipfile.ZipInfo('sub/x.txt'), b'nested', zipfile.ZIP_DEFLATED)\n"
        "z.writestr(zipfile.ZipInfo('stored.bin'),bytes(range(256))*4, zipfile.ZIP_STORED)\n"
        "z.writestr(zipfile.ZipInfo('lorem.txt'), (b'the quick brown fox jumps over the lazy dog. ')*200, zipfile.ZIP_DEFLATED)\n"
        "zi=zipfile.ZipInfo('sub/')\n"           /* explicit directory entry */
        "zi.external_attr=0o40755<<16\n"
        "z.writestr(zi, b'')\n"
        "z.close()\n"
        "PY\n", zip_path);
    if (system(cmd) != 0) { printf("  FAIL: could not build test zip\n"); return 1; }

    long ziplen = 0;
    uint8_t *zip = read_file(zip_path, &ziplen);
    if (!zip) { printf("  FAIL: could not read %s\n", zip_path); return 1; }
    printf("  built %s (%ld bytes)\n", zip_path, ziplen);

    /* scratch at END of a heap block: any write past scratchcap => ASan OOB. */
    uint8_t *scratch = malloc(SCRATCH_CAP);

    /* --- 1. Exact extraction. --- */
    recorder_t rec = {0};
    int n = zip_extract(zip, (int)ziplen, rec_emit, &rec, scratch, SCRATCH_CAP);
    printf("  zip_extract returned %d entries\n", n);
    if (n < 0) { printf("  FAIL: zip_extract failed on a valid archive\n"); return 1; }

    /* Build expected big.txt = "ABCD"*16384 for the comparison. */
    static uint8_t big[4 * 16384];
    for (int i = 0; i < 16384; i++) memcpy(big + i * 4, "ABCD", 4);
    static uint8_t stored[256 * 4];
    for (int b = 0; b < 4; b++) for (int i = 0; i < 256; i++) stored[b*256+i] = (uint8_t)i;
    static uint8_t lorem[45 * 200];
    for (int i = 0; i < 200; i++)
        memcpy(lorem + i * 45, "the quick brown fox jumps over the lazy dog. ", 45);

    expect(&rec, "hello.txt",  (const uint8_t *)"hi", 2);
    expect(&rec, "big.txt",    big, (int)sizeof big);
    expect(&rec, "empty.txt",  (const uint8_t *)"", 0);
    expect(&rec, "sub/x.txt",  (const uint8_t *)"nested", 6);
    expect(&rec, "stored.bin", stored, (int)sizeof stored);
    expect(&rec, "lorem.txt",  lorem, (int)sizeof lorem);

    /* The explicit "sub/" directory entry must NOT be emitted. */
    if (rec_find(&rec, "sub/")) { printf("  FAIL: directory 'sub/' was emitted\n"); fails++; }
    else printf("  ok: directory entry 'sub/' skipped\n");

    /* Expect exactly the 6 files (not the directory). */
    if (n != 6) { printf("  FAIL: expected 6 files, got %d\n", n); fails++; }

    rec_free(&rec);

    /* --- 1b. Interop with the SYSTEM `zip` tool, which lays the archive out
     * differently from python (it appends an archive COMMENT after the EOCD —
     * exercising the backward scan over a non-empty comment — and writes an
     * explicit 'sub/' directory entry). Skipped cleanly if `zip` is absent. */
    if (system("command -v zip >/dev/null 2>&1") == 0) {
        const char *tp = "/tmp/osdev_ziptool.zip";
        char c2[2048];
        snprintf(c2, sizeof c2,
            "set -e; d=$(mktemp -d); mkdir -p \"$d/sub\"; "
            "printf 'hi' > \"$d/hello.txt\"; "
            "python3 -c \"open('$d/big.txt','wb').write(b'ABCD'*16384)\"; "
            "printf '' > \"$d/empty.txt\"; printf 'nested' > \"$d/sub/x.txt\"; "
            "rm -f %s; (cd \"$d\" && zip -q -r %s .); "
            "printf 'archive comment to test EOCD backward scan' | zip -q -z %s; "
            "rm -rf \"$d\"", tp, tp, tp);
        if (system(c2) != 0) { printf("  FAIL: could not build system-zip archive\n"); fails++; }
        else {
            long tl = 0; uint8_t *tz = read_file(tp, &tl);
            if (!tz) { printf("  FAIL: could not read %s\n", tp); fails++; }
            else {
                printf("  built %s via system zip (%ld bytes, w/ comment)\n", tp, tl);
                recorder_t tr = {0};
                int tn = zip_extract(tz, (int)tl, rec_emit, &tr, scratch, SCRATCH_CAP);
                printf("  system-zip: zip_extract returned %d entries\n", tn);
                expect(&tr, "hello.txt", (const uint8_t *)"hi", 2);
                expect(&tr, "big.txt",   big, (int)sizeof big);
                expect(&tr, "empty.txt", (const uint8_t *)"", 0);
                expect(&tr, "sub/x.txt", (const uint8_t *)"nested", 6);
                if (rec_find(&tr, "sub/")) { printf("  FAIL: 'sub/' emitted\n"); fails++; }
                else printf("  ok: directory 'sub/' skipped (system zip)\n");
                if (tn != 4) { printf("  FAIL: system-zip expected 4 files, got %d\n", tn); fails++; }
                rec_free(&tr);
                free(tz);
            }
        }
    } else {
        printf("  skip: system 'zip' not installed, interop omitted\n");
    }

    /* --- 2a. Truncation fuzz: every prefix length 0..ziplen must be safe. We
     * copy each prefix into its OWN exact-size buffer so ASan red-zones sit
     * right after the last valid byte (catches any over-read past the prefix).*/
    printf("  fuzz: %ld truncated prefixes...\n", ziplen + 1);
    for (long L = 0; L <= ziplen; L++) {
        uint8_t *pref = malloc(L ? (size_t)L : 1);
        memcpy(pref, zip, (size_t)L);
        recorder_t fr = {0};
        int r = zip_extract(pref, (int)L, rec_emit, &fr, scratch, SCRATCH_CAP);
        if (r < -1) { printf("  FAIL: prefix %ld returned %d (<-1)\n", L, r); fails++; }
        rec_free(&fr);
        free(pref);
    }
    printf("  ok: all truncated prefixes returned cleanly\n");

    /* --- 2b. Single-byte corruption fuzz across the whole file. --- */
    printf("  fuzz: single-byte corruptions...\n");
    for (long pos = 0; pos < ziplen; pos++) {
        uint8_t *cp = malloc((size_t)ziplen);
        memcpy(cp, zip, (size_t)ziplen);
        cp[pos] ^= 0xFF;
        recorder_t fr = {0};
        int r = zip_extract(cp, (int)ziplen, rec_emit, &fr, scratch, SCRATCH_CAP);
        if (r < -1) { printf("  FAIL: corruption @%ld returned %d\n", pos, r); fails++; }
        rec_free(&fr);
        free(cp);
    }
    printf("  ok: all single-byte corruptions returned cleanly\n");

    /* --- 2b'. Multi-byte random mutations of the real archive: flip 1..4 byte
     * runs to random values. This is the path most likely to forge a
     * self-consistent-but-hostile size/offset field (e.g. a huge compressed or
     * uncompressed size, a local-header offset pointing mid-data) and so to
     * exercise the bounds checks guarding the inflate()/copy paths. */
    printf("  fuzz: multi-byte mutations (80000 trials)...\n");
    for (int t = 0; t < 80000; t++) {
        uint8_t *cp = malloc((size_t)ziplen);
        memcpy(cp, zip, (size_t)ziplen);
        int nmut = 1 + (int)(xr() % 4);
        for (int m = 0; m < nmut; m++) {
            long o = (long)(xr() % (uint32_t)ziplen);
            cp[o] = (uint8_t)xr();
        }
        recorder_t fr = {0};
        int r = zip_extract(cp, (int)ziplen, rec_emit, &fr, scratch, SCRATCH_CAP);
        if (r < -1) { printf("  FAIL: mutation t=%d returned %d\n", t, r); fails++; }
        rec_free(&fr);
        free(cp);
    }
    printf("  ok: all multi-byte mutations returned cleanly\n");

    /* --- 2c. Random garbage of assorted sizes (no valid structure). --- */
    printf("  fuzz: random garbage buffers...\n");
    for (int t = 0; t < 20000; t++) {
        int L = (int)(xr() % 600);
        uint8_t *g = malloc(L ? (size_t)L : 1);
        for (int i = 0; i < L; i++) g[i] = (uint8_t)xr();
        /* Occasionally sprinkle real signatures to drive deeper into parsing. */
        if (L >= 4 && (t & 3) == 0) {
            uint32_t sigs[3] = {0x06054b50u, 0x02014b50u, 0x04034b50u};
            uint32_t s = sigs[xr() % 3];
            int o = (int)(xr() % (uint32_t)(L - 3));
            g[o] = s & 0xff; g[o+1] = (s>>8)&0xff; g[o+2] = (s>>16)&0xff; g[o+3] = (s>>24)&0xff;
        }
        recorder_t fr = {0};
        int r = zip_extract(g, L, rec_emit, &fr, scratch, SCRATCH_CAP);
        if (r < -1) { printf("  FAIL: random t=%d returned %d\n", t, r); fails++; }
        rec_free(&fr);
        free(g);
    }
    printf("  ok: all random buffers returned cleanly\n");

    /* --- 2d. Tiny scratch buffer: extraction must refuse to overflow it. With
     * a 1-byte scratch, every method-0/8 entry should make zip_extract return
     * -1 (or skip), never write OOB (ASan would abort). */
    {
        uint8_t *tiny = malloc(1);
        recorder_t fr = {0};
        int r = zip_extract(zip, (int)ziplen, rec_emit, &fr, tiny, 1);
        printf("  ok: 1-byte scratch returned %d (no overflow)\n", r);
        rec_free(&fr);
        free(tiny);
    }

    free(scratch);
    free(zip);

    if (fails) { printf("\nFAILED: %d check(s) failed\n", fails); return 1; }
    printf("\nPASS: exact extraction + directory skip, fuzz/corrupt safe, ASan/UBSan clean\n");
    return 0;
}
