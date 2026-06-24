/*
 * dm.c — device-mapper-lite: a virtual block device composed from registered
 * blockdev_t children (M1157). A RAID-1 MIRROR writes every sector to both
 * members and reads from either, so the volume survives a member failure. It
 * works on ANY storage driver because it fans its read/write out through the
 * uniform blockdev_read/blockdev_write (kernel/blockdev.c) — the block layer is
 * already a vtable, so a mirror is just another consumer of it.
 *
 * dm_selftest() runs at boot IFF two non-boot writable disks are present (e.g.
 * osdrive --disk2 --disk3); with fewer it is a clean no-op, so the normal boot
 * + every make-check suite is unaffected.
 */
#include "dm.h"
#include "blockdev.h"
#include "console.h"
#include <stdint.h>

#define DM_SECSZ 512

/* RAID-1 read: serve from member A, falling back to B (e.g. if A has been
 * marked failed or its read errors) — the redundancy guarantee. */
int dm_mirror_read(dm_mirror_t *m, uint64_t lba, uint32_t count, void *buf) {
    if (!m->a_failed && blockdev_read(m->a, lba, count, buf) == 0) return 0;
    if (!m->b_failed && blockdev_read(m->b, lba, count, buf) == 0) return 0;
    return -1;
}

/* RAID-1 write: fan out to BOTH members (skipping a failed one). Succeeds only
 * if every live member took the write. */
int dm_mirror_write(dm_mirror_t *m, uint64_t lba, uint32_t count, const void *buf) {
    int ra = m->a_failed ? 0 : blockdev_write(m->a, lba, count, buf);
    int rb = m->b_failed ? 0 : blockdev_write(m->b, lba, count, buf);
    return (ra == 0 && rb == 0) ? 0 : -1;
}

/* Boot self-test: build a mirror over the first two non-boot writable disks and
 * prove (a) write fans out to both members, (b) reads survive a member failure.
 * Touches one scratch sector per member and restores it. Logs to dmesg. */
void dm_selftest(void) {
    int devs[2], nd = 0, n = blockdev_count();
    for (int i = 0; i < n && nd < 2; i++) {
        blockdev_t *d = blockdev_get(i);
        if (!d || !d->write || !d->name) continue;
        if (d->name[0]=='a' && d->name[1]=='t' && d->name[2]=='a') continue;   /* never the boot disk */
        devs[nd++] = i;
    }
    if (nd < 2) { kprintf("[dm] RAID-1 self-test skipped (need 2 non-boot writable disks)\n"); return; }

    dm_mirror_t m = { devs[0], devs[1], 0, 0 };
    uint64_t lba = 64;                              /* a scratch sector well clear of any superblock */
    static uint8_t save_a[DM_SECSZ], save_b[DM_SECSZ], pat[DM_SECSZ], back[DM_SECSZ];
    if (blockdev_read(devs[0], lba, 1, save_a) < 0 || blockdev_read(devs[1], lba, 1, save_b) < 0) {
        kprintf("[dm] RAID-1 self-test: scratch read failed\n"); return;
    }
    for (int b = 0; b < DM_SECSZ; b++) pat[b] = (uint8_t)(b * 13 + 0x37);

    int wr = (dm_mirror_write(&m, lba, 1, pat) == 0);
    int rt = wr && dm_mirror_read(&m, lba, 1, back) == 0;
    for (int b = 0; rt && b < DM_SECSZ; b++) if (back[b] != pat[b]) rt = 0;
    kprintf("[dm] RAID-1 (%s+%s): write+read-back %s\n",
            blockdev_get(devs[0])->name, blockdev_get(devs[1])->name, rt ? "OK" : "FAILED");

    int fan = blockdev_read(devs[1], lba, 1, back) == 0;   /* read member B DIRECTLY: did the write reach it? */
    for (int b = 0; fan && b < DM_SECSZ; b++) if (back[b] != pat[b]) fan = 0;
    kprintf("[dm] RAID-1 fan-out (both members hold the data): %s\n", fan ? "OK" : "FAILED");

    m.a_failed = 1;                                        /* simulate member A dying */
    int red = dm_mirror_read(&m, lba, 1, back) == 0;       /* the mirror must now serve from B */
    for (int b = 0; red && b < DM_SECSZ; b++) if (back[b] != pat[b]) red = 0;
    kprintf("[dm] RAID-1 redundancy (read survives member-A failure): %s\n", red ? "OK" : "FAILED");
    m.a_failed = 0;

    blockdev_write(devs[0], lba, 1, save_a);               /* restore both scratch sectors */
    blockdev_write(devs[1], lba, 1, save_b);
    kprintf("[dm] RAID-1 self-test: %s\n", (rt && fan && red) ? "ALL OK" : "FAILURES");
}
