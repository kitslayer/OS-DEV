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
void dm_selftest(void);   /* boot-time RAID-1 self-test (no-op unless 2 non-boot writable disks) */
