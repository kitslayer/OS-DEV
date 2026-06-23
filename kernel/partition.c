/*
 * partition.c — MBR + GPT partition-table parsing over the ATA driver.
 *
 * Given a drive index, read LBA 0 and decode whichever partition scheme is
 * present, exposing each partition as a (drive, start-LBA, sector-count, type)
 * volume. This is purely additive: it reads the disk read-only and the bare
 * boot FAT32 on drive 0 (which has no partition table) is unaffected.
 *
 * SECURITY: every field is treated as untrusted on-disk input.
 *   - Reads only ever target fixed 512-byte sector buffers; offsets into those
 *     buffers are compile-time constants well within 512.
 *   - A partition is emitted only if start_lba + sectors fits within the drive's
 *     reported sector count (rejecting overlap with metadata is not our job, but
 *     a partition that runs off the end of the disk is rejected).
 *   - The GPT entry array is read in bounded chunks (one sector at a time) and
 *     the entry count + entry size are capped, so a malformed header claiming a
 *     billion 4 KB entries can never make us allocate or read an absurd amount.
 *   - The output count is capped at min(max, PART_MAX).
 */
#include "partition.h"
#include "ata.h"
#include "console.h"
#include "string.h"

#define SECSZ 512

/* Little-endian field readers over a byte buffer (the on-disk byte order). */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* Our PIO read path uses LBA28: the on-the-wire LBA field is 28 bits, so any
 * sector we can actually address is < 2^28. A partition whose start (or any
 * sector) falls past that can't be read by ata_read_drive without the LBA being
 * truncated to a *different, wrong* in-range sector — so reject it up front
 * rather than silently read the wrong place. (2^28 sectors = 128 GiB, far past
 * any image we attach; this is a defensive bound on untrusted on-disk values.) */
#define LBA28_MAX 0x10000000ull   /* 2^28 */

/* True if [start, start+count) lies wholly within a disk of `disk_sectors`
 * sectors, with count > 0, no 64-bit overflow, and inside the LBA28-addressable
 * range. disk_sectors==0 means the size is unknown (drive reported nothing) —
 * then we only sanity-check count>0, no overflow, and the LBA28 bound, since we
 * can't compare against an end we don't know. */
static int range_in_disk(uint64_t start, uint64_t count, uint64_t disk_sectors) {
    if (count == 0) return 0;
    if (start + count < start) return 0;             /* 64-bit overflow */
    if (start >= LBA28_MAX || start + count > LBA28_MAX) return 0;   /* PIO-addressable */
    if (disk_sectors == 0) return 1;                 /* size unknown: accept (count>0, in range) */
    if (start >= disk_sectors) return 0;
    if (start + count > disk_sectors) return 0;
    return 1;
}

/* Read LBA 0; return 1 + fill `sec` if it carries a 0x55AA boot signature. */
static int read_lba0(int drive, uint8_t sec[SECSZ]) {
    if (ata_read_drive(drive, 0, 1, sec) < 0) return 0;
    return rd16(sec + 510) == 0xAA55;
}

int partition_scheme(int drive) {
    const struct ata_drive_info *info = ata_drive(drive);
    if (!info || !info->present) return PART_SCHEME_NONE;

    uint8_t sec[SECSZ];
    if (!read_lba0(drive, sec))
        return PART_SCHEME_NONE;                     /* no 0x55AA: a bare FS, not a table */

    /* A protective-MBR (any of the four entries is type 0xEE) means GPT. */
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = sec + 0x1BE + i * 16;
        if (e[4] == PART_TYPE_GPT)
            return PART_SCHEME_GPT;
    }
    return PART_SCHEME_MBR;
}

/* --- GPT --------------------------------------------------------------------
 * Header (LBA 1):
 *   off 0   "EFI PART" signature (8 bytes)
 *   off 72  partition-entry-array starting LBA (u64)
 *   off 80  number of partition entries (u32)
 *   off 84  size of a partition entry, bytes (u32)
 * Each entry:
 *   off 0   type GUID (16 bytes; all-zero = unused slot)
 *   off 32  first LBA (u64)
 *   off 40  last  LBA (u64, inclusive)
 */
static int scan_gpt(int drive, uint64_t disk_sectors, partition_t *out, int max) {
    uint8_t hdr[SECSZ];
    if (ata_read_drive(drive, 1, 1, hdr) < 0)
        return 0;
    if (memcmp(hdr, "EFI PART", 8) != 0)
        return 0;                                    /* protective MBR but no valid GPT header */

    uint64_t arr_lba    = rd64(hdr + 72);
    uint32_t nentries   = rd32(hdr + 80);
    uint32_t entry_size = rd32(hdr + 84);

    /* Sanity-cap the geometry so a corrupt header can't drive an enormous read.
     * A real GPT entry is 128 bytes; the spec allows larger but always a power of
     * two >= 128, and never bigger than a sector here. Cap the entry count too. */
    if (entry_size < 128 || entry_size > SECSZ)
        return 0;
    if (SECSZ % entry_size != 0)
        return 0;                                    /* must tile a sector cleanly for our reader */
    if (nentries == 0)
        return 0;
    if (nentries > 512)
        nentries = 512;                              /* cap: never read a runaway entry array */
    /* The entry array must sit on the disk we can actually read, within the
     * LBA28-addressable range (so its sector reads can't truncate to a wrong LBA). */
    if (arr_lba >= LBA28_MAX)
        return 0;
    if (disk_sectors && arr_lba >= disk_sectors)
        return 0;

    int n = 0;
    uint32_t per_sec = SECSZ / entry_size;           /* entries packed per sector */
    uint32_t sectors_needed = (nentries + per_sec - 1) / per_sec;

    for (uint32_t s = 0; s < sectors_needed && n < max; s++) {
        uint64_t lba = arr_lba + s;
        if (lba >= LBA28_MAX)
            break;
        if (disk_sectors && lba >= disk_sectors)
            break;
        uint8_t buf[SECSZ];
        if (ata_read_drive(drive, (uint32_t)lba, 1, buf) < 0)
            break;
        for (uint32_t k = 0; k < per_sec && n < max; k++) {
            uint32_t idx = s * per_sec + k;
            if (idx >= nentries) break;
            const uint8_t *e = buf + k * entry_size;   /* k*entry_size < SECSZ by construction */

            /* All-zero type GUID = unused slot. */
            int used = 0;
            for (int b = 0; b < 16; b++) if (e[b]) { used = 1; break; }
            if (!used) continue;

            uint64_t first = rd64(e + 32);
            uint64_t last  = rd64(e + 40);
            if (last < first) continue;                /* malformed entry */
            uint64_t count = last - first + 1;
            if (!range_in_disk(first, count, disk_sectors)) continue;

            out[n].drive     = drive;
            out[n].start_lba = first;
            out[n].sectors   = count;
            out[n].type      = PART_TYPE_GPT;
            out[n].scheme    = PART_SCHEME_GPT;
            n++;
        }
    }
    return n;
}

/* --- MBR --------------------------------------------------------------------
 * Four 16-byte primary entries at offset 0x1BE:
 *   off 0   status / boot flag
 *   off 4   type byte (0 = empty)
 *   off 8   start LBA (u32)
 *   off 12  sector count (u32)
 */
static int scan_mbr(int drive, uint64_t disk_sectors, const uint8_t sec[SECSZ],
                    partition_t *out, int max) {
    int n = 0;
    for (int i = 0; i < 4 && n < max; i++) {
        const uint8_t *e = sec + 0x1BE + i * 16;
        uint8_t type = e[4];
        if (type == 0x00)               continue;    /* empty slot */
        if (type == PART_TYPE_GPT)      continue;    /* protective entry, handled by the GPT path */

        uint64_t start = rd32(e + 8);
        uint64_t count = rd32(e + 12);
        if (!range_in_disk(start, count, disk_sectors)) continue;

        out[n].drive     = drive;
        out[n].start_lba = start;
        out[n].sectors   = count;
        out[n].type      = type;
        out[n].scheme    = PART_SCHEME_MBR;
        n++;
    }
    return n;
}

int partition_scan(int drive, partition_t *out, int max) {
    if (!out || max <= 0) return -1;
    if (max > PART_MAX) max = PART_MAX;

    const struct ata_drive_info *info = ata_drive(drive);
    if (!info || !info->present) return -1;          /* no disk in this slot */
    uint64_t disk_sectors = info->sectors;

    uint8_t sec[SECSZ];
    if (!read_lba0(drive, sec))
        return 0;                                    /* bare filesystem / no table — not an error */

    /* GPT if any primary entry is the 0xEE protective type. */
    for (int i = 0; i < 4; i++) {
        if (sec[0x1BE + i * 16 + 4] == PART_TYPE_GPT)
            return scan_gpt(drive, disk_sectors, out, max);
    }
    return scan_mbr(drive, disk_sectors, sec, out, max);
}

/* --- read a FAT32 volume located at a partition's start-LBA -----------------
 *
 * This is the "multi-volume" proof: rather than rewiring the whole fat32.c
 * driver (which keeps one set of module-global BPB fields for the boot mount
 * and reads via the bare ata_read on drive 0), we do a small, self-contained,
 * read-only FAT32 walk *relative to a volume's start-LBA*. It reads the volume's
 * BPB, validates FAT32, then walks the root directory — proving the volume was
 * located correctly AND that its filesystem is genuinely readable, without
 * touching the boot mount's state. Every read targets a fixed 512-byte buffer
 * and the cluster walk is bounded by the cluster count, so a corrupt FS cannot
 * run away.
 *
 * The walk reads through a caller-supplied block-read callback (read, ctx) so the
 * SAME logic works over ANY storage driver via kernel/blockdev.c, not just ATA.
 * partition_fat32_find() (below) is the original ATA-only signature, kept as a
 * thin wrapper so every existing ATA caller is unchanged.
 */

/* Parsed, validated FAT32 geometry — the fields the root-dir walk needs. */
struct fatvol {
    uint64_t fat_start;        /* absolute LBA of FAT #1 */
    uint64_t data_start;       /* absolute LBA of cluster 2 */
    uint32_t spc;              /* sectors per cluster (1..128) */
    uint32_t root_clus;        /* first cluster of the root directory (>=2) */
    uint32_t total_clusters;   /* data-cluster count (chain-walk bound; 0=unknown) */
};

/* Read the boot sector at absolute LBA `start_lba` via (read,ctx) and, if it is a
 * valid FAT32 BPB, fill *v and return 1; else return 0. Every field is validated
 * before use exactly as the boot mount (fat32.c) and the old ATA walk did. */
static int fatvol_parse(blk_read_fn read, void *ctx, uint64_t start_lba,
                        struct fatvol *v) {
    uint8_t bs[SECSZ];
    if (read(ctx, start_lba, 1, bs) < 0)     return 0;
    if (rd16(bs + 510) != 0xAA55)            return 0;   /* boot signature */
    if (rd16(bs + 11) != SECSZ)              return 0;   /* bytes/sector */
    uint32_t spc = bs[13];
    if (spc == 0 || spc > 128)               return 0;   /* sectors/cluster */
    uint32_t reserved    = rd16(bs + 14);
    uint32_t num_fats    = bs[16];
    uint32_t fat_sectors = rd32(bs + 36);                /* FAT size 32 */
    uint32_t root_clus   = rd32(bs + 44);
    if (fat_sectors == 0 || num_fats == 0)   return 0;   /* not FAT32 */
    if (root_clus < 2)                       return 0;

    v->fat_start  = start_lba + reserved;
    v->data_start = v->fat_start + (uint64_t)num_fats * fat_sectors;
    v->spc        = spc;
    v->root_clus  = root_clus;

    /* Bound the cluster walk: derive the volume's cluster count from the BPB,
     * capped by what the FAT can actually index. */
    uint32_t tot_sec = rd16(bs + 19) ? rd16(bs + 19) : rd32(bs + 32);
    uint32_t meta = reserved + num_fats * fat_sectors;
    uint32_t total_clusters = (tot_sec > meta) ? (tot_sec - meta) / spc : 0;
    uint32_t fatcap = fat_sectors * (SECSZ / 4);
    if (fatcap > 2 && total_clusters > fatcap - 2) total_clusters = fatcap - 2;
    v->total_clusters = total_clusters;
    return 1;
}

/* Step the root-dir cluster chain: read FAT[cl] via (read,ctx) and return the
 * next cluster (masked to 28 bits), or EOC (0x0FFFFFF8) on a read error. */
static uint32_t fatvol_next(blk_read_fn read, void *ctx, const struct fatvol *v,
                            uint32_t cl) {
    uint32_t fo = cl * 4;
    uint8_t fsec[SECSZ];
    if (read(ctx, v->fat_start + fo / SECSZ, 1, fsec) < 0) return 0x0FFFFFF8;
    return rd32(fsec + (fo % SECSZ)) & 0x0FFFFFFF;
}

/* Turn an 11-byte 8.3 directory name (space-padded) into "NAME.EXT" in `out`
 * (needs 13 bytes). Mirrors fat32.c's format_83 — for fatvol_list's output. */
static void fatvol_name(const uint8_t *raw, char *out) {
    int n = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) out[n++] = (char)raw[i];
    if (raw[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) out[n++] = (char)raw[i];
    }
    out[n] = '\0';
}

int fatvol_find(blk_read_fn read, void *ctx, uint64_t start_lba,
                const char name83[11], uint32_t *out_size) {
    struct fatvol v;
    if (!read || !fatvol_parse(read, ctx, start_lba, &v)) return 0;

    uint32_t cl = v.root_clus, steps = 0;
    while (cl >= 2 && cl < 0x0FFFFFF8 && (!v.total_clusters || cl < v.total_clusters + 2)) {
        uint64_t first = v.data_start + (uint64_t)(cl - 2) * v.spc;
        for (uint32_t s = 0; s < v.spc; s++) {
            uint8_t dir[SECSZ];
            if (read(ctx, first + s, 1, dir) < 0) return 0;
            for (int off = 0; off < SECSZ; off += 32) {
                const uint8_t *e = dir + off;
                if (e[0] == 0x00) return 0;          /* end of directory */
                if (e[0] == 0xE5) continue;          /* deleted */
                if (e[11] == 0x0F) continue;         /* LFN */
                if (e[11] & 0x08) continue;          /* volume label */
                int eq = 1;
                for (int i = 0; i < 11; i++) if (e[i] != (uint8_t)name83[i]) { eq = 0; break; }
                if (eq) { if (out_size) *out_size = rd32(e + 28); return 1; }
            }
        }
        cl = fatvol_next(read, ctx, &v, cl);
        if (v.total_clusters && ++steps > v.total_clusters + 2) break;   /* cycle guard */
    }
    return 0;
}

int fatvol_list(blk_read_fn read, void *ctx, uint64_t start_lba,
                fatvol_dirent *out, int max) {
    if (!read || !out || max <= 0) return 0;
    struct fatvol v;
    if (!fatvol_parse(read, ctx, start_lba, &v)) return 0;

    int n = 0;
    uint32_t cl = v.root_clus, steps = 0;
    while (cl >= 2 && cl < 0x0FFFFFF8 && (!v.total_clusters || cl < v.total_clusters + 2)
           && n < max) {
        uint64_t first = v.data_start + (uint64_t)(cl - 2) * v.spc;
        for (uint32_t s = 0; s < v.spc && n < max; s++) {
            uint8_t dir[SECSZ];
            if (read(ctx, first + s, 1, dir) < 0) return n;
            for (int off = 0; off < SECSZ && n < max; off += 32) {
                const uint8_t *e = dir + off;
                if (e[0] == 0x00) return n;           /* end of directory */
                if (e[0] == 0xE5) continue;           /* deleted */
                if (e[11] == 0x0F) continue;          /* LFN */
                if (e[11] & 0x08) continue;           /* volume label */
                char name[13];
                fatvol_name(e, name);
                /* Skip "." and ".." (only meaningful in subdirs, but be safe). */
                if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
                    continue;
                for (int i = 0; i < 13; i++) out[n].name[i] = name[i];
                out[n].is_dir = (e[11] & 0x10) ? 1 : 0;
                out[n].size   = out[n].is_dir ? 0 : rd32(e + 28);
                n++;
            }
        }
        cl = fatvol_next(read, ctx, &v, cl);
        if (v.total_clusters && ++steps > v.total_clusters + 2) break;   /* cycle guard */
    }
    return n;
}

/* ATA read adapter: a blk_read_fn that reads via ata_read_drive, with the drive
 * index packed into the ctx pointer (so partition_fat32_find keeps its old ATA-
 * only signature while sharing the generalized walk above). */
static int ata_blk_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    int drive = (int)(intptr_t)ctx;
    if (lba >= LBA28_MAX) return -1;             /* PIO is LBA28-addressable */
    return ata_read_drive(drive, (uint32_t)lba, count, buf);
}

int partition_fat32_find(int drive, uint64_t start_lba, const char name83[11],
                         uint32_t *out_size) {
    return fatvol_find(ata_blk_read, (void *)(intptr_t)drive, start_lba,
                       name83, out_size);
}

/* --- the headless self-test ------------------------------------------------- */

static const char *scheme_name(int s) {
    switch (s) {
        case PART_SCHEME_MBR: return "MBR";
        case PART_SCHEME_GPT: return "GPT";
        default:              return "none";
    }
}

void partition_enumerate(void) {
    int present = ata_identify_all();
    kprintf("[ ok ] ATA drives: %d present (probed all 4 legacy slots).\n", present);

    const char *names[ATA_MAX_DRIVES] = {
        "primary master", "primary slave", "secondary master", "secondary slave"
    };

    for (int d = 0; d < ATA_MAX_DRIVES; d++) {
        const struct ata_drive_info *info = ata_drive(d);
        if (!info || !info->present)
            continue;
        kprintf("  ata%d (%s): %lu sectors (%lu MiB)%s, model \"%s\"\n",
                d, names[d], (unsigned long)info->sectors,
                (unsigned long)(info->sectors / 2048), info->lba48 ? " [LBA48]" : "",
                info->model);

        int sch = partition_scheme(d);
        partition_t parts[16];
        int n = partition_scan(d, parts, 16);
        if (n < 0) {
            kprintf("    partition scan failed.\n");
            continue;
        }
        kprintf("    partition table: %s, %d partition(s)\n", scheme_name(sch), n);
        for (int i = 0; i < n; i++) {
            kprintf("      part %d: %s drive %d type 0x%02x start-LBA %lu sectors %lu (%lu MiB)\n",
                    i, scheme_name(parts[i].scheme), parts[i].drive, parts[i].type & 0xFF,
                    (unsigned long)parts[i].start_lba, (unsigned long)parts[i].sectors,
                    (unsigned long)(parts[i].sectors / 2048));

            /* Multi-volume proof: try to read the partition's filesystem from its
             * offset and find a known file. The probe validates the BPB itself, so
             * it cleanly reports "not FAT32" on a non-FAT partition. We look for
             * "HELLO   TXT" (8.3, space-padded) — the file the test image plants. */
            uint32_t fsize = 0;
            if (partition_fat32_find(parts[i].drive, parts[i].start_lba, "HELLO   TXT", &fsize)) {
                kprintf("        FAT32 volume at start-LBA %lu: found HELLO.TXT (%lu bytes)\n",
                        (unsigned long)parts[i].start_lba, (unsigned long)fsize);
            } else {
                /* Fallback proof: confirm a FAT32 boot signature sits at the
                 * partition's first sector (i.e. we located the volume), even if
                 * it isn't FAT32 or has no such file. */
                uint8_t bs[SECSZ];
                if (ata_read_drive(parts[i].drive, (uint32_t)parts[i].start_lba, 1, bs) == 0
                    && rd16(bs + 510) == 0xAA55) {
                    int isfat32 = (memcmp(bs + 82, "FAT32", 5) == 0);
                    kprintf("        partition first sector has 0x55AA boot signature%s\n",
                            isfat32 ? " (FAT32 BPB)" : "");
                }
            }
        }
    }
}
