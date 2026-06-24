/*
 * iso9660_test.c — host-side fuzz test of the ISO 9660 read path (ASan + UBSan).
 *
 * iso9660.c is an untrusted-input parser: the kernel mounts arbitrary .iso
 * images (M1106/M1131) and walks their on-disk Primary Volume Descriptor + the
 * chain of directory records — attacker-controllable structures. ext2 (tests/
 * ext2) got a fuzz harness in M1180; this is the matching one for ISO 9660, the
 * last untrusted on-disk parser without host fuzz coverage.
 *
 * iso9660.c is device-agnostic (blk_read_fn) and uses only stack buffers, so it
 * #includes cleanly with an in-memory disk served by bd_read (out-of-range LBA
 * -> error, so the parser can never read past the image, and a runaway dir/extent
 * walk terminates at the disk edge). No host ISO builder (genisoimage/mkisofs)
 * is needed: build_iso() lays down a minimal VALID image by hand (PVD at logical
 * sector 16, a root directory with "."/".."/"F.TXT;1", a file extent), the test
 * confirms it probes/lists/reads, then FUZZES corrupted copies of the metadata
 * region (the PVD + root directory — which carry every extent LBA/length and the
 * record lengths) and re-runs probe/list/isdir/read.
 *
 * A corrupt PVD / bad root extent / overlong record length / huge data length /
 * a directory pointing at itself must never out-of-bounds or hang. ASan/UBSan
 * catch OOB; iso9660.c's bounds (lendr>=34, off+lendr<=2048, off+33+fil<=2048,
 * rd_sec range-checked) must catch the rest; the runner's `timeout` catches a
 * hang. Exit 0 = pass.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define DSEC 512
#define ISO_LOG   2048
#define IMG_SECS  64                              /* 64 logical sectors = 128 KB */
static uint8_t g_img[IMG_SECS * ISO_LOG];
static long    g_img_bytes = sizeof g_img;

/* blk_read_fn: serve 512-byte device sectors from g_img; OOB -> error (disk edge). */
static int bd_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx;
    if ((lba + count) * DSEC > (uint64_t)g_img_bytes) return -1;
    memcpy(buf, g_img + lba * DSEC, (size_t)count * DSEC);
    return 0;
}

#include "iso9660.c"                              /* probe/list/read/isdir + the static walkers */

static uint8_t g_golden[sizeof g_img];

static void wr32le(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

/* Write one ISO directory record at img+byteoff. Returns its (even-padded) length. */
static int dir_rec(uint8_t *r, uint32_t extent, uint32_t len, int is_dir,
                   const char *name, int namelen) {
    int lendr = 33 + namelen; if (lendr & 1) lendr++;     /* pad to even */
    memset(r, 0, lendr);
    r[0] = (uint8_t)lendr;
    wr32le(r + 2, extent);                                /* extent LBA (LE32) */
    wr32le(r + 10, len);                                  /* data length (LE32) */
    r[25] = is_dir ? 0x02 : 0x00;                         /* file flags */
    r[32] = (uint8_t)namelen;                             /* file id length */
    for (int i = 0; i < namelen; i++) r[33 + i] = (uint8_t)name[i];
    return lendr;
}

/* Lay down a minimal valid ISO 9660 image into g_img. */
static void build_iso(void) {
    memset(g_img, 0, sizeof g_img);
    uint8_t *pvd = g_img + 16u * ISO_LOG;                 /* logical sector 16 */
    pvd[0] = 1;                                           /* Primary Volume Descriptor */
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;                                           /* version */
    /* root directory record at PVD+156: extent sector 18, length one sector */
    { uint8_t one = 0; dir_rec(pvd + 156, 18, ISO_LOG, 1, (char *)&one, 1); }

    uint8_t *term = g_img + 17u * ISO_LOG;                /* sector 17: VD set terminator */
    term[0] = 255; memcpy(term + 1, "CD001", 5); term[6] = 1;

    uint8_t *root = g_img + 18u * ISO_LOG;                /* sector 18: root directory */
    int off = 0; char dot = 0, dotdot = 1;
    off += dir_rec(root + off, 18, ISO_LOG, 1, &dot, 1);          /* "." */
    off += dir_rec(root + off, 18, ISO_LOG, 1, &dotdot, 1);      /* ".." */
    off += dir_rec(root + off, 20, 27, 0, "F.TXT;1", 7);         /* a real file */
    /* a leftover terminator (lendr 0) is already there from memset */

    uint8_t *file = g_img + 20u * ISO_LOG;               /* sector 20: file data */
    memcpy(file, "hello iso9660 fuzz harness\n", 27);
}

/* Hammer every read/walk path over whatever currently sits in g_img — none may
 * crash or hang on garbage. (void) casts silence -Wunused-result. */
static void exercise(void) {
    fatvol_dirent ents[32];
    uint8_t rb[4096];
    (void)iso9660_probe(bd_read, 0, 0);
    (void)iso9660_list_path(bd_read, 0, 0, "/", ents, 32);
    (void)iso9660_list_path(bd_read, 0, 0, "/F.TXT", ents, 32);
    (void)iso9660_isdir_path(bd_read, 0, 0, "/");
    (void)iso9660_isdir_path(bd_read, 0, 0, "/F.TXT");
    (void)iso9660_read_path(bd_read, 0, 0, "/F.TXT", rb, sizeof rb);
    (void)iso9660_read_path(bd_read, 0, 0, "/F.TXT;1", rb, sizeof rb);
    (void)iso9660_read_path(bd_read, 0, 0, "/nope", rb, sizeof rb);
    (void)iso9660_read_path(bd_read, 0, 0, "/a/b/c/d/e", rb, sizeof rb);
}

int main(void) {
    build_iso();
    memcpy(g_golden, g_img, sizeof g_img);

    /* 1) the golden image must probe + list + read correctly (proves it's real) */
    if (iso9660_probe(bd_read, 0, 0) != 0) { fprintf(stderr, "golden probe failed\n"); return 1; }
    fatvol_dirent ents[32];
    int n = iso9660_list_path(bd_read, 0, 0, "/", ents, 32);
    if (n < 1) { fprintf(stderr, "golden list failed (%d)\n", n); return 1; }
    uint8_t rb[4096];
    long r = iso9660_read_path(bd_read, 0, 0, "/F.TXT", rb, sizeof rb);
    if (r != 27 || memcmp(rb, "hello iso9660 fuzz harness\n", 27) != 0) {
        fprintf(stderr, "golden read failed (%ld)\n", r); return 1;
    }
    printf("golden ISO 9660 image OK (probe + list %d entries + read %ld bytes)\n", n, r);

    /* 2) fuzz: corrupt the metadata region (PVD sector 16 .. file sector 20),
     * which carries the volume descriptor, root extent, and all record lengths. */
    srand(1234);
    const long meta_lo = 16L * ISO_LOG, meta_hi = 21L * ISO_LOG;   /* sectors 16..20 */
    for (int iter = 0; iter < 12000; iter++) {
        memcpy(g_img, g_golden, sizeof g_img);
        int flips = 1 + rand() % 12;
        for (int f = 0; f < flips; f++) {
            long pos = meta_lo + rand() % (meta_hi - meta_lo);
            /* mix of bit flips, zeroing, and adversarial 0xFF / large-length bytes */
            int mode = rand() % 4;
            if      (mode == 0) g_img[pos] ^= (uint8_t)(1 << (rand() % 8));
            else if (mode == 1) g_img[pos]  = 0xFF;
            else if (mode == 2) g_img[pos]  = 0x00;
            else                g_img[pos]  = (uint8_t)(rand() & 0xFF);
        }
        exercise();
    }

    /* 3) a few rounds that scribble the WHOLE image (probe must reject, not crash) */
    for (int iter = 0; iter < 3000; iter++) {
        for (size_t i = 0; i < sizeof g_img; i += (size_t)(1 + rand() % 64))
            g_img[i] = (uint8_t)(rand() & 0xFF);
        exercise();
    }

    printf("iso9660 fuzz: 15000 iterations, no OOB / no hang\n");
    return 0;
}
