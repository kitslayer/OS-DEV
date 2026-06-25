/*
 * dm.h — device-mapper-lite virtual block devices (M1157). A RAID-1 mirror
 * composes two registered blockdev_t children (by index) into a redundant
 * volume. See dm.c.
 */
#pragma once
#include <stdint.h>

/* A RAID-1 mirror over child blockdev indices a, b. *_failed marks a member as
 * down so reads skip it and writes don't fan to it. */
typedef struct { int a, b, a_failed, b_failed; } dm_mirror_t;

int  dm_mirror_read(dm_mirror_t *m, uint64_t lba, uint32_t count, void *buf);
int  dm_mirror_write(dm_mirror_t *m, uint64_t lba, uint32_t count, const void *buf);

#define DM_MAXDEV 8

/* RAID-0 stripe set over `n` child blockdev indices, 1-sector chunks round-robin:
 * logical sector L lives on member (L % n) at that member's sector (L / n). No
 * redundancy — pure capacity/throughput aggregation. */
typedef struct { int devs[DM_MAXDEV]; int n; } dm_stripe_t;
int dm_stripe_read(dm_stripe_t *s, uint64_t lba, uint32_t count, void *buf);
int dm_stripe_write(dm_stripe_t *s, uint64_t lba, uint32_t count, const void *buf);

/* RAID-5 set over `n` (>=3) child blockdev indices: each stripe is (n-1) data
 * sectors + 1 ROTATING XOR-parity sector, so the volume survives ANY single
 * member failure (the lost sector is reconstructed as the XOR of the others).
 * `failed[m]` marks a member down. */
typedef struct { int devs[DM_MAXDEV]; int n; int failed[DM_MAXDEV]; } dm_raid5_t;
int dm_raid5_read(dm_raid5_t *r, uint64_t lba, uint32_t count, void *buf);
int dm_raid5_write(dm_raid5_t *r, uint64_t lba, uint32_t count, const void *buf);

/* Linear logical volume (LVM-lite): concatenate `n` members into ONE address
 * space — member 0 holds logical sectors [0, sectors[0]), member 1 the next
 * sectors[1], and so on. Grows capacity past any single disk; no redundancy or
 * striping. `sectors[m]` is each member's size in sectors. */
typedef struct { int devs[DM_MAXDEV]; uint64_t sectors[DM_MAXDEV]; int n; } dm_linear_t;
int dm_linear_read(dm_linear_t *l, uint64_t lba, uint32_t count, void *buf);
int dm_linear_write(dm_linear_t *l, uint64_t lba, uint32_t count, const void *buf);

void dm_selftest(void);   /* boot-time RAID-1/0/5 + linear-LV self-test (no-op unless >=2 (>=3 for RAID-5) non-boot disks) */
