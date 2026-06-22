/*
 * partition.h — parse MBR and GPT partition tables on an ATA drive.
 *
 * A drive's first sector (LBA 0) either holds a bare filesystem (no partition
 * table — our boot disk is like this) or a partition table that carves the
 * drive into volumes. We support both classic schemes:
 *
 *   MBR  — a 0x55AA signature at offset 510 and four 16-byte primary entries at
 *          offset 0x1BE. Each entry has a type byte, a 32-bit start-LBA and a
 *          32-bit sector count.
 *   GPT  — modern, unbounded. The MBR is then "protective" (one type-0xEE entry
 *          covering the disk); the real table is a GPT header at LBA 1 ("EFI
 *          PART") pointing at an array of 128-byte entries, each with a type
 *          GUID and 64-bit first/last LBA.
 *
 * partition_scan() reads the table on a drive and returns each non-empty
 * partition as a (drive, start-LBA, sector-count, type) tuple, every field
 * validated against the drive's reported size. It never writes the disk.
 */
#pragma once
#include <stdint.h>

/* How a partition table was encoded (for logging / callers that care). */
#define PART_SCHEME_NONE 0   /* no recognizable table at LBA 0 */
#define PART_SCHEME_MBR  1
#define PART_SCHEME_GPT  2

/* One discovered partition. `type` is the MBR type byte (e.g. 0x0C = FAT32 LBA)
 * for MBR partitions, or PART_TYPE_GPT for a GPT partition (whose real type is a
 * 16-byte GUID, not a single byte). */
#define PART_TYPE_GPT 0xEE   /* also the protective-MBR type byte that signals GPT */

typedef struct {
    int      drive;          /* ATA drive index 0..3 this partition lives on */
    uint64_t start_lba;      /* first sector of the partition */
    uint64_t sectors;        /* partition length in sectors */
    int      type;           /* MBR type byte, or PART_TYPE_GPT for a GPT entry */
    int      scheme;         /* PART_SCHEME_MBR / PART_SCHEME_GPT */
} partition_t;

/* Cap on partitions returned, so a malformed/huge GPT can never make us read or
 * emit an unbounded array. Callers pass their own `max` too; the smaller wins. */
#define PART_MAX 64

/* Scan drive `drive`'s partition table into `out` (up to `max` entries).
 * Returns the number of partitions found (>=0), or -1 if the drive is absent or
 * unreadable. A drive with no table (bare filesystem) returns 0 — not an error.
 * Every emitted partition is bounds-checked: start_lba + sectors fits within the
 * drive's reported sector count, and we never read past a sector buffer. */
int partition_scan(int drive, partition_t *out, int max);

/* Detect the scheme at LBA 0 without enumerating (PART_SCHEME_*). */
int partition_scheme(int drive);

/* Read-only FAT32 probe at a partition's start-LBA on any drive: validate the
 * BPB is FAT32, then find an 8.3 file (`name83`, 11 bytes, space-padded) in the
 * root directory. Returns 1 + sets *out_size if found; 0 otherwise. This proves
 * a partition's filesystem is readable from its offset, without touching the
 * boot-disk FAT32 mount's global state. */
int partition_fat32_find(int drive, uint64_t start_lba, const char name83[11],
                         uint32_t *out_size);

/* Probe all four ATA drives and log each present drive + its partition table
 * (scheme, and per-partition drive/type/start/size). The headless self-test. */
void partition_enumerate(void);
