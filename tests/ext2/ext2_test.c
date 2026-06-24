/*
 * ext2_test.c — host-side fuzz test of the ext2 READ path (ASan + UBSan).
 *
 * ext2 is the OS's most-exposed on-disk parser: the kernel auto-mounts ARBITRARY
 * ext2 volumes from any attached disk (/diskN, M1061), ISO/loop images (M1106/
 * M1107) — i.e. untrusted on-disk metadata — yet, unlike fat32 (tests/fs) /
 * x509 / the image decoders, it had no fuzz harness. This is that harness.
 *
 * ext2.c is device-agnostic (blk_read_fn/blk_write_fn callbacks) and uses no
 * kernel heap (stack buffers + local helpers), so it #includes cleanly with only
 * an in-memory disk served by bd_read (out-of-range LBA -> error, so the parser
 * can never read past the image, and a runaway block/inode walk terminates at
 * the disk edge). It builds a real golden image with mke2fs (argv[1]), confirms
 * it probes + lists, then FUZZES corrupted copies of the metadata region
 * (superblock + group descriptors + bitmaps + inode table — which holds the
 * direct/indirect block pointers) and re-runs probe/list/isdir/read.
 *
 * A corrupt superblock/GDT/inode (bad block_size, inode count, block pointers,
 * a cyclic/huge indirect chain, a cyclic directory) must never out-of-bounds or
 * hang. ASan/UBSan catch OOB; ext2.c's bounds/depth caps must catch the rest;
 * the runner's `timeout` catches a hang. Exit 0 = pass.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define SECSZ 512
static uint8_t g_img[2 * 1024 * 1024];      /* the (mutable, fuzzed) working image */
static long    g_img_bytes;

/* blk_read_fn: serve 512-byte sectors from g_img; OOB -> error (the disk edge). */
static int bd_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx;
    if ((lba + count) * SECSZ > (uint64_t)g_img_bytes) return -1;
    memcpy(buf, g_img + lba * SECSZ, (size_t)count * SECSZ);
    return 0;
}
static int bd_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    (void)ctx;
    if ((lba + count) * SECSZ > (uint64_t)g_img_bytes) return -1;
    memcpy(g_img + lba * SECSZ, buf, (size_t)count * SECSZ);
    return 0;
}

#include "ext2.c"                             /* the read paths: ext2_probe/list/read/isdir + the static walkers */

static uint8_t g_golden[2 * 1024 * 1024];
static long    g_golden_bytes;

/* Hammer every read/walk parser over whatever currently sits in g_img. Return
 * values are ignored — the point is that NONE of these crash or hang on garbage.
 * (void) casts silence -Wunused-result. */
static void exercise(void) {
    fatvol_dirent ents[32];
    uint8_t rb[512];
    (void)ext2_probe(bd_read, 0, 0);
    (void)ext2_list_path(bd_read, 0, 0, "/", ents, 32);
    (void)ext2_list_path(bd_read, 0, 0, "/lost+found", ents, 32);
    (void)ext2_isdir_path(bd_read, 0, 0, "/lost+found");
    (void)ext2_isdir_path(bd_read, 0, 0, "/BIG");
    (void)ext2_read_path(bd_read, 0, 0, "/HELLO", rb, sizeof rb);
    (void)ext2_read_path(bd_read, 0, 0, "/BIG", rb, sizeof rb);   /* walks direct + (double-)indirect blocks */
    (void)ext2_read_path(bd_read, 0, 0, "/nope", rb, sizeof rb);
    /* xattr read parsers (M1182): walk the in-inode EA on possibly-corrupt
     * inodes — a bad i_extra_isize/magic/entry must never read past the inode. */
    char xb[256];
    (void)ext2_getxattr(bd_read, 0, 0, "/HELLO", "user.greeting", xb, sizeof xb);
    (void)ext2_getxattr(bd_read, 0, 0, "/BIG", "user.x", xb, sizeof xb);
    (void)ext2_listxattr(bd_read, 0, 0, "/HELLO", xb, sizeof xb);
    (void)ext2_listxattr(bd_read, 0, 0, "/lost+found", xb, sizeof xb);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ext2_test <golden.ext2>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open golden"); return 2; }
    g_golden_bytes = (long)fread(g_golden, 1, sizeof g_golden, f);
    fclose(f);
    if (g_golden_bytes < 2048) { fprintf(stderr, "golden image too small (%ld)\n", g_golden_bytes); return 2; }

    /* 1) the golden image must probe + list correctly (proves the harness is real) */
    memcpy(g_img, g_golden, (size_t)g_golden_bytes); g_img_bytes = g_golden_bytes;
    if (ext2_probe(bd_read, 0, 0) != 0) { fprintf(stderr, "golden ext2_probe failed\n"); return 1; }
    fatvol_dirent ents[32];
    int n = ext2_list_path(bd_read, 0, 0, "/", ents, 32);
    int found = 0;
    for (int i = 0; i < n; i++) if (!strcmp(ents[i].name, "lost+found")) found = 1;
    if (n < 1 || !found) { fprintf(stderr, "golden root listing wrong (n=%d, lost+found=%d)\n", n, found); return 1; }
    uint8_t rb[512];
    long hn = ext2_read_path(bd_read, 0, 0, "/HELLO", rb, sizeof rb);
    if (hn <= 0) { fprintf(stderr, "golden /HELLO read failed (%ld)\n", hn); return 1; }
    printf("golden ext2: probe OK, root has %d entries (incl lost+found), /HELLO reads %ld bytes\n", n, hn);

    /* 2) fuzz the metadata region (superblock + GDT + bitmaps + inode table, which
     *    carries every block pointer), re-running all read parsers each time. */
    unsigned seed = 0x9e3779b9u;
    long meta = g_golden_bytes < 65536 ? g_golden_bytes : 65536;
    const int ITERS = 8000;
    for (int iter = 0; iter < ITERS; iter++) {
        memcpy(g_img, g_golden, (size_t)g_golden_bytes); g_img_bytes = g_golden_bytes;
        int flips = 1 + (int)(seed % 12);
        for (int k = 0; k < flips; k++) {
            seed = seed * 1103515245u + 12345u; long off = (long)(seed % (unsigned)meta);
            seed = seed * 1103515245u + 12345u; g_img[off] ^= (uint8_t)(seed >> 13);
        }
        exercise();
    }
    printf("fuzz: %d corrupt-metadata iterations, no OOB / no hang\n", ITERS);

    /* 3) ext4 extent-mapped reads (M1186): if an extent image is given, its
     *    extent-flag inodes must read byte-exact, and fuzzing its metadata (which
     *    now carries the in-inode extent tree) must stay OOB/hang-safe. */
    if (argc > 2) {
        FILE *xf = fopen(argv[2], "rb");
        if (!xf) { fprintf(stderr, "open extent image failed\n"); return 1; }
        g_golden_bytes = (long)fread(g_golden, 1, sizeof g_golden, xf); fclose(xf);
        memcpy(g_img, g_golden, (size_t)g_golden_bytes); g_img_bytes = g_golden_bytes;
        static uint8_t big[131072];
        long bn = ext2_read_path(bd_read, 0, 0, "/BIG.TXT", big, sizeof big);
        int okb = (bn == 100000);
        for (long i = 0; i < bn && okb; i++) if (big[i] != 'E') okb = 0;
        if (!okb) { fprintf(stderr, "extent /BIG.TXT read wrong (%ld)\n", bn); return 1; }
        long sn = ext2_read_path(bd_read, 0, 0, "/SMALL.TXT", big, sizeof big);
        if (sn != 19 || memcmp(big, "hello extent world\n", 19) != 0) {
            fprintf(stderr, "extent /SMALL.TXT read wrong (%ld)\n", sn); return 1;
        }
        printf("extent: /BIG.TXT %ld bytes + /SMALL.TXT byte-exact via the extent tree\n", bn);
        long emeta = g_golden_bytes < 65536 ? g_golden_bytes : 65536;
        for (int iter = 0; iter < 6000; iter++) {
            memcpy(g_img, g_golden, (size_t)g_golden_bytes); g_img_bytes = g_golden_bytes;
            int flips = 1 + (int)(seed % 12);
            for (int k = 0; k < flips; k++) {
                seed = seed * 1103515245u + 12345u; long off = (long)(seed % (unsigned)emeta);
                seed = seed * 1103515245u + 12345u; g_img[off] ^= (uint8_t)(seed >> 13);
            }
            (void)ext2_read_path(bd_read, 0, 0, "/BIG.TXT", big, sizeof big);   /* walks the (corrupt) extent tree */
            exercise();
        }
        printf("extent fuzz: 6000 corrupt-extent-tree iterations, no OOB / no hang\n");
    }
    printf("PASS\n");
    return 0;
}
