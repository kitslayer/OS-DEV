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

    /* ext2_pread positioned read (M1196): a slice read at an offset must match the
     * same bytes from a full read — across block boundaries + a partial first block. */
    {
        static uint8_t full[400000];
        long bn = ext2_read_path(bd_read, 0, 0, "/BIG", full, sizeof full);   /* the 300 KB golden file */
        if (bn < 200000) { fprintf(stderr, "golden /BIG too small (%ld)\n", bn); return 1; }
        unsigned long offs[] = { 0, 1, 1000, 1023, 1024, 4096, 4097, 100000, 250000 };
        for (unsigned t = 0; t < sizeof offs / sizeof offs[0]; t++) {
            unsigned long o = offs[t]; if ((long)o >= bn) continue;
            uint8_t slice[777];
            long sn = ext2_pread(bd_read, 0, 0, "/BIG", slice, sizeof slice, o);
            long want = bn - (long)o; if (want > (long)sizeof slice) want = sizeof slice;
            if (sn != want || memcmp(slice, full + o, (size_t)sn) != 0) {
                fprintf(stderr, "ext2_pread @%lu wrong (got %ld, want %ld)\n", o, sn, want); return 1;
            }
        }
        if (ext2_pread(bd_read, 0, 0, "/BIG", rb, sizeof rb, (unsigned long)bn + 10) != 0) {
            fprintf(stderr, "ext2_pread past EOF should be 0\n"); return 1;
        }
        printf("ext2_pread: positioned reads byte-exact vs a full read (9 offsets + past-EOF)\n");
    }

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
        /* ext4 extent WRITE (M1189): the driver creates a file as a single contiguous
         * extent; read it back byte-exact, and dump the image (argv[3]) for the runner
         * to e2fsck (which validates the extent layout + the alloc_run bitmap/counts). */
        {
            memcpy(g_img, g_golden, (size_t)g_golden_bytes); g_img_bytes = g_golden_bytes;
            static uint8_t wd[50000]; for (int i = 0; i < 50000; i++) wd[i] = (uint8_t)('A' + (i % 26));
            long w = ext2_write_path(bd_read, bd_write, 0, 0, "/EXTW.TXT", wd, 50000);
            static uint8_t wr[50000]; long rr = (w == 50000) ? ext2_read_path(bd_read, 0, 0, "/EXTW.TXT", wr, sizeof wr) : -1;
            if (w != 50000 || rr != 50000 || memcmp(wr, wd, 50000) != 0) {
                fprintf(stderr, "extent WRITE/readback wrong (w=%ld r=%ld)\n", w, rr); return 1;
            }
            printf("extent write: created /EXTW.TXT (50000 bytes) as an extent, read back byte-exact\n");
            /* hard link (M1207): a 2nd name -> the same inode; both read identically,
             * and unlinking one leaves the other working (links_count inc/dec). Uses
             * a dedicated file so /EXTW.TXT (separately checked extent-mapped) is
             * untouched. The final image (dumped below) must stay e2fsck-clean. */
            {
                static uint8_t hd[1000]; for (int i = 0; i < 1000; i++) hd[i] = (uint8_t)(i * 7 + 1);
                if (ext2_write_path(bd_read, bd_write, 0, 0, "/HL1.TXT", hd, 1000) != 1000) { fprintf(stderr, "hard-link setup write failed\n"); return 1; }
                if (ext2_link_path(bd_read, bd_write, 0, 0, "/HL1.TXT", "/HL2.TXT") != 0) { fprintf(stderr, "ext2_link_path failed\n"); return 1; }
                static uint8_t lr[1000];
                long l1 = ext2_read_path(bd_read, 0, 0, "/HL2.TXT", lr, sizeof lr);
                if (l1 != 1000 || memcmp(lr, hd, 1000) != 0) { fprintf(stderr, "hard-link readback wrong (%ld)\n", l1); return 1; }
                ext2_unlink_path(bd_read, bd_write, 0, 0, "/HL1.TXT");            /* drop the original name */
                long l2 = ext2_read_path(bd_read, 0, 0, "/HL2.TXT", lr, sizeof lr);
                if (l2 != 1000 || memcmp(lr, hd, 1000) != 0) { fprintf(stderr, "hard link did not survive unlink (%ld)\n", l2); return 1; }
                if (ext2_read_path(bd_read, 0, 0, "/HL1.TXT", lr, sizeof lr) > 0) { fprintf(stderr, "unlinked original still readable\n"); return 1; }
                printf("hard link: /HL1.TXT -> /HL2.TXT shares the inode; after unlinking the original the link still reads 1000 bytes\n");
            }
            /* rename (M1213): relocate a directory entry, preserving the inode.
             * (a) same-dir file rename, (b) cross-dir file move, (c) directory move
             * across parents (fixes ".." + both parents' link counts), (d) a move
             * into the directory's own subtree is refused. The final image (dumped
             * below) must stay e2fsck-clean — the gold check of link counts + dirents. */
            {
                static uint8_t rd[1500]; for (int i = 0; i < 1500; i++) rd[i] = (uint8_t)(i * 3 + 5);
                if (ext2_write_path(bd_read, bd_write, 0, 0, "/RN1.TXT", rd, 1500) != 1500) { fprintf(stderr, "rename setup write failed\n"); return 1; }
                static uint8_t rr2[1500];
                /* (a) same-directory rename */
                if (ext2_rename_path(bd_read, bd_write, 0, 0, "/RN1.TXT", "/RN2.TXT") != 0) { fprintf(stderr, "rename same-dir failed\n"); return 1; }
                if (ext2_read_path(bd_read, 0, 0, "/RN2.TXT", rr2, sizeof rr2) != 1500 || memcmp(rr2, rd, 1500) != 0) { fprintf(stderr, "rename same-dir readback wrong\n"); return 1; }
                if (ext2_read_path(bd_read, 0, 0, "/RN1.TXT", rr2, sizeof rr2) > 0) { fprintf(stderr, "old name still readable after rename\n"); return 1; }
                /* (b) cross-directory file move into a fresh subdir */
                if (ext2_mkdir_path(bd_read, bd_write, 0, 0, "/RNDIR") != 0) { fprintf(stderr, "rename mkdir failed\n"); return 1; }
                if (ext2_rename_path(bd_read, bd_write, 0, 0, "/RN2.TXT", "/RNDIR/RN3.TXT") != 0) { fprintf(stderr, "rename cross-dir failed\n"); return 1; }
                if (ext2_read_path(bd_read, 0, 0, "/RNDIR/RN3.TXT", rr2, sizeof rr2) != 1500 || memcmp(rr2, rd, 1500) != 0) { fprintf(stderr, "rename cross-dir readback wrong\n"); return 1; }
                /* (c) directory move across parents (".." repoint + parent link counts) */
                if (ext2_mkdir_path(bd_read, bd_write, 0, 0, "/RNSUB") != 0) { fprintf(stderr, "rename mkdir2 failed\n"); return 1; }
                if (ext2_mkdir_path(bd_read, bd_write, 0, 0, "/RNSUB/INNER") != 0) { fprintf(stderr, "rename mkdir3 failed\n"); return 1; }
                if (ext2_rename_path(bd_read, bd_write, 0, 0, "/RNSUB/INNER", "/RNDIR/INNER") != 0) { fprintf(stderr, "rename dir-move failed\n"); return 1; }
                if (ext2_isdir_path(bd_read, 0, 0, "/RNDIR/INNER") != 1) { fprintf(stderr, "moved dir absent at new path\n"); return 1; }
                if (ext2_isdir_path(bd_read, 0, 0, "/RNSUB/INNER") == 1) { fprintf(stderr, "moved dir still at old path\n"); return 1; }
                /* (d) refuse moving a directory into its own subtree (would orphan it) */
                if (ext2_rename_path(bd_read, bd_write, 0, 0, "/RNDIR", "/RNDIR/INNER/LOOP") == 0) { fprintf(stderr, "rename allowed a dir into its own subtree\n"); return 1; }
                printf("rename: same-dir + cross-dir file move + directory move across parents (\"..\" + link counts), subtree-loop refused\n");
            }
            /* truncate (M1228): shrink frees blocks beyond newlen + lowers i_size;
             * grow is sparse (the new tail reads back as zeros). The dumped image
             * must stay e2fsck-clean. */
            {
                static uint8_t td[3000]; for (int i = 0; i < 3000; i++) td[i] = (uint8_t)(i * 5 + 3);
                if (ext2_write_path(bd_read, bd_write, 0, 0, "/TR.TXT", td, 3000) != 3000) { fprintf(stderr, "truncate setup write failed\n"); return 1; }
                static uint8_t tr[4096];
                if (ext2_truncate_path(bd_read, bd_write, 0, 0, "/TR.TXT", 500) != 0) { fprintf(stderr, "truncate shrink failed\n"); return 1; }
                long r = ext2_read_path(bd_read, 0, 0, "/TR.TXT", tr, sizeof tr);
                if (r != 500 || memcmp(tr, td, 500) != 0) { fprintf(stderr, "truncate shrink readback wrong (%ld)\n", r); return 1; }
                if (ext2_truncate_path(bd_read, bd_write, 0, 0, "/TR.TXT", 1500) != 0) { fprintf(stderr, "truncate grow failed\n"); return 1; }
                long r2 = ext2_read_path(bd_read, 0, 0, "/TR.TXT", tr, sizeof tr);
                int zeros_ok = 1; for (int i = 500; i < 1500; i++) if (tr[i] != 0) { zeros_ok = 0; break; }
                if (r2 != 1500 || memcmp(tr, td, 500) != 0 || !zeros_ok) { fprintf(stderr, "truncate grow readback wrong (%ld z=%d)\n", r2, zeros_ok); return 1; }
                printf("truncate: shrink 3000->500 (frees blocks), grow 500->1500 sparse (zero-filled tail)\n");
            }
            /* SEEK_HOLE / SEEK_DATA (M1229): build a file with a real middle hole
             * ([data][hole][data]) and assert the boundaries. Overwrite converts
             * the extent inode to block-mapped so punch_hole can carve the hole. */
            {
                static uint8_t sd[5000]; for (int i = 0; i < 5000; i++) sd[i] = (uint8_t)(i | 1);
                if (ext2_write_path(bd_read, bd_write, 0, 0, "/SH.TXT", sd, 5000) != 5000) { fprintf(stderr, "seek setup write failed\n"); return 1; }
                if (ext2_write_path(bd_read, bd_write, 0, 0, "/SH.TXT", sd, 5000) != 5000) { fprintf(stderr, "seek overwrite failed\n"); return 1; }
                if (ext2_punch_hole(bd_read, bd_write, 0, 0, "/SH.TXT", 1024, 2048) < 0) { fprintf(stderr, "seek punch_hole failed\n"); return 1; }  /* returns #blocks freed */
                /* now: [0,1024) data, [1024,3072) hole, [3072,5000) data */
                long e = 0;
                if (ext2_seek_data_hole(bd_read, 0, 0, "/SH.TXT", 0,    0) != 0)    e = __LINE__;   /* DATA at 0 -> 0 */
                if (ext2_seek_data_hole(bd_read, 0, 0, "/SH.TXT", 0,    1) != 1024) e = __LINE__;   /* HOLE after 0 -> 1024 */
                if (ext2_seek_data_hole(bd_read, 0, 0, "/SH.TXT", 1024, 0) != 3072) e = __LINE__;   /* DATA after the hole -> 3072 */
                if (ext2_seek_data_hole(bd_read, 0, 0, "/SH.TXT", 1024, 1) != 1024) e = __LINE__;   /* HOLE at 1024 -> 1024 */
                if (ext2_seek_data_hole(bd_read, 0, 0, "/SH.TXT", 3072, 1) != 5000) e = __LINE__;   /* HOLE after last data -> EOF */
                if (ext2_seek_data_hole(bd_read, 0, 0, "/SH.TXT", 5000, 0) != -1)   e = __LINE__;   /* DATA at EOF -> ENXIO */
                if (e) { fprintf(stderr, "seek_data_hole wrong (line %ld)\n", e); return 1; }
                printf("seek: SEEK_DATA/SEEK_HOLE on a [data|hole|data] file -- boundaries at 0/1024/3072/5000 + ENXIO at EOF\n");
            }
            if (argc > 3) { FILE *wf = fopen(argv[3], "wb"); if (wf) { fwrite(g_img, 1, (size_t)g_img_bytes, wf); fclose(wf); } }
        }
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
