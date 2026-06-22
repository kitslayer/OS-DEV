/* ata.h — read/write sectors from the legacy ATA (IDE) disks (PIO mode). */
#pragma once
#include <stdint.h>

#define SECTOR_SIZE 512

/* The four legacy ATA drives, in index order:
 *   0 = primary master, 1 = primary slave,
 *   2 = secondary master, 3 = secondary slave. */
#define ATA_MAX_DRIVES 4

/* What an IDENTIFY sweep learns about one drive. */
struct ata_drive_info {
    int      present;        /* 1 if a usable PIO ATA disk answered IDENTIFY */
    int      drive;          /* drive index 0..3 */
    uint64_t sectors;        /* reported LBA28/LBA48 sector count */
    int      lba48;          /* 1 if the drive advertised 48-bit addressing */
    char     model[41];      /* model string (trimmed), for logging */
};

/* --- primary-master API (drive 0): unchanged, every existing caller uses it -- */

/* Read `count` 512-byte sectors starting at LBA `lba` into `buf` (drive 0).
 * Returns 0 on success, -1 on error/timeout. (count==0 means 256, the LBA28
 * convention the original driver used.) */
int ata_read(uint32_t lba, uint8_t count, void *buf);

/* Write `count` 512-byte sectors starting at LBA `lba` from `buf` (drive 0). */
int ata_write(uint32_t lba, uint8_t count, const void *buf);

/* --- multi-drive API ------------------------------------------------------- */

/* Read/write `count` sectors at `lba` on drive `drive` (0..3). `count` is a
 * 32-bit count here (chunked internally to the 8-bit LBA28 sector field), so a
 * caller may request more than 256 sectors. Returns 0 on success, -1 on error. */
int ata_read_drive(int drive, uint32_t lba, uint32_t count, void *buf);
int ata_write_drive(int drive, uint32_t lba, uint32_t count, const void *buf);

/* Probe all four legacy drives with IDENTIFY (finite timeouts; an absent drive
 * is detected via the status register, never hangs). Records which are present
 * and their sector counts. Returns the number of present drives. Idempotent. */
int ata_identify_all(void);

/* Info for drive `drive` (runs ata_identify_all() lazily on first use). NULL for
 * an out-of-range index; check ->present for whether a disk is actually there. */
const struct ata_drive_info *ata_drive(int drive);

/* Convenience: reported sector count of drive `drive`, or 0 if absent. */
uint64_t ata_drive_sectors(int drive);
