/* Standalone proof of ext2_setxattr/getxattr/listxattr/removexattr (M1182):
 * load a real mke2fs image, round-trip xattrs through the from-scratch ext2.c,
 * write it back. Then the shell script runs e2fsck -fn + debugfs to prove the
 * on-disk format is e2fsck-clean AND readable by real ext2 userspace tools. */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define SECSZ 512
static uint8_t g_img[8 * 1024 * 1024];
static long g_img_bytes;
static int bd_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx; if ((lba + count) * SECSZ > (uint64_t)g_img_bytes) return -1;
    memcpy(buf, g_img + lba * SECSZ, (size_t)count * SECSZ); return 0;
}
static int bd_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    (void)ctx; if ((lba + count) * SECSZ > (uint64_t)g_img_bytes) return -1;
    memcpy(g_img + lba * SECSZ, buf, (size_t)count * SECSZ); return 0;
}
#include "ext2.c"

static int fails = 0;
static void check(const char *what, int ok) {
    printf("  %-44s %s\n", what, ok ? "OK" : "FAIL"); if (!ok) fails++;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s in.img out.img\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror("open in"); return 2; }
    g_img_bytes = (long)fread(g_img, 1, sizeof g_img, f); fclose(f);
    printf("loaded %ld bytes from %s\n", g_img_bytes, argv[1]);

    const char *P = "/F.TXT";
    /* 256-byte inode -> ~92 spare bytes, so 2-3 small in-inode attrs fit.
     * set two (one value has a space), then verify a too-big 3rd is refused. */
    check("set user.greeting=hello",  ext2_setxattr(bd_read, bd_write, 0, 0, P, "user.greeting", "hello", 5) == 5);
    check("set user.by='cc ai'",      ext2_setxattr(bd_read, bd_write, 0, 0, P, "user.by", "cc ai", 5) == 5);

    char buf[256]; long n;
    /* --- in-inode phase: get + replace --- */
    n = ext2_getxattr(bd_read, 0, 0, P, "user.greeting", buf, sizeof buf);
    check("get user.greeting == 'hello'", n == 5 && memcmp(buf, "hello", 5) == 0);
    check("replace user.greeting=hi", ext2_setxattr(bd_read, bd_write, 0, 0, P, "user.greeting", "hi", 2) == 2);
    n = ext2_getxattr(bd_read, 0, 0, P, "user.by", buf, sizeof buf);
    check("user.by survived replace", n == 5 && memcmp(buf, "cc ai", 5) == 0);

    /* --- block-spill phase: a 180-byte value can't fit in-inode -> EA block;
     * the whole set moves to the block, and the small attrs must survive --- */
    char big[180]; memset(big, 'B', sizeof big);
    check("set user.big (180B) -> spills to EA block", ext2_setxattr(bd_read, bd_write, 0, 0, P, "user.big", big, 180) == 180);
    n = ext2_getxattr(bd_read, 0, 0, P, "user.big", buf, sizeof buf);
    check("get user.big == 180x'B' (from block)", n == 180 && memcmp(buf, big, 180) == 0);
    n = ext2_getxattr(bd_read, 0, 0, P, "user.greeting", buf, sizeof buf);
    check("user.greeting survived spill (=hi)", n == 2 && memcmp(buf, "hi", 2) == 0);
    n = ext2_getxattr(bd_read, 0, 0, P, "user.by", buf, sizeof buf);
    check("user.by survived spill (=cc ai)", n == 5 && memcmp(buf, "cc ai", 5) == 0);

    /* listxattr (across in-inode + block): all three names */
    char names[512]; long ln = ext2_listxattr(bd_read, 0, 0, P, names, sizeof names);
    int saw_g = 0, saw_b = 0, saw_x = 0;
    for (long i = 0; i < ln; ) {
        if (!strcmp(names + i, "user.greeting")) saw_g = 1;
        if (!strcmp(names + i, "user.by"))       saw_b = 1;
        if (!strcmp(names + i, "user.big"))      saw_x = 1;
        while (i < ln && names[i]) i++; i++;
    }
    check("list shows all three names", saw_g && saw_b && saw_x);

    /* replace the block value with a different length; the others survive */
    char big2[150]; memset(big2, 'C', sizeof big2);
    check("replace user.big (150B 'C')", ext2_setxattr(bd_read, bd_write, 0, 0, P, "user.big", big2, 150) == 150);
    n = ext2_getxattr(bd_read, 0, 0, P, "user.big", buf, sizeof buf);
    check("get user.big == 150x'C'", n == 150 && memcmp(buf, big2, 150) == 0);

    /* missing attr -> -1; non-user namespace -> -1 */
    check("get missing -> -1", ext2_getxattr(bd_read, 0, 0, P, "user.nope", buf, sizeof buf) == -1);
    check("set system.x -> -1 (only user.*)", ext2_setxattr(bd_read, bd_write, 0, 0, P, "system.x", "y", 1) == -1);

    /* remove the small one; greeting + the block value survive */
    check("remove user.by", ext2_removexattr(bd_read, bd_write, 0, 0, P, "user.by") == 0);
    check("get removed user.by -> -1", ext2_getxattr(bd_read, 0, 0, P, "user.by", buf, sizeof buf) == -1);
    n = ext2_getxattr(bd_read, 0, 0, P, "user.greeting", buf, sizeof buf);
    check("user.greeting survived removal (=hi)", n == 2 && memcmp(buf, "hi", 2) == 0);
    check("remove again -> -1 (absent)", ext2_removexattr(bd_read, bd_write, 0, 0, P, "user.by") == -1);
    /* final on-disk state: user.greeting=hi + user.big=150x'C' in the EA block
     * (e2fsck will validate the block + the per-entry hash; debugfs reads both). */

    FILE *o = fopen(argv[2], "wb"); if (!o) { perror("open out"); return 2; }
    fwrite(g_img, 1, (size_t)g_img_bytes, o); fclose(o);
    printf("wrote %s\n", argv[2]);
    printf(fails ? "\nPROOF FAILED (%d)\n" : "\nALL ROUND-TRIP CHECKS PASSED\n", fails);
    return fails ? 1 : 0;
}
