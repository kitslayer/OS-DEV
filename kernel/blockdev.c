/*
 * blockdev.c — a generic block-device registry + read-only multi-volume FAT32
 * browsing over EVERY storage driver (see blockdev.h for the why).
 *
 * blockdev_init() registers each present storage device behind one uniform
 * read(ctx,lba,count,buf) vtable; blockdev_enumerate() then reads LBA 0 of each,
 * works out where any FAT32 volumes live (a bare FS at LBA 0, or FAT32 partitions
 * carved out by an MBR/GPT table), and for each one MOUNTS it read-only via the
 * device-agnostic fatvol_list()/fatvol_find() in partition.c and LISTS its root
 * directory. This proves every driver's disk is genuinely browsable.
 *
 * SECURITY — this parses untrusted on-disk MBR/GPT/FAT structures over DMA driver
 * reads, so every field is validated before use:
 *   - Every sector read targets a FIXED 512-byte stack buffer; buffer offsets are
 *     compile-time constants well within 512.
 *   - The MBR/GPT scan only ever COLLECTS candidate start-LBAs; the heavy FAT walk
 *     (fatvol_*) re-validates the whole BPB and is bounded by the cluster count +
 *     a cycle guard, so a corrupt table/FAT can never loop or read unboundedly.
 *   - Candidate start-LBAs are validated against the device capacity where known
 *     and against the 64-bit/addressable range; the GPT entry array is read in
 *     bounded single-sector chunks with the entry count + size capped.
 *   - A device read() returning an error => the volume/device is skipped cleanly.
 *   - The candidate list, the per-volume file listing, and the device registry are
 *     all fixed-size and capped.
 *
 * It is purely READ-ONLY and never touches the boot FAT32 mount (fat32.c/vfs.c).
 */
#include "blockdev.h"
#include "partition.h"
#include "ata.h"
#include "ahci.h"
#include "virtio_blk.h"
#include "nvme.h"
#include "usb_storage.h"
#include "console.h"
#include "string.h"

#define SECSZ 512

/* Little-endian field readers over a byte buffer (on-disk byte order). */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* --- the registry ---------------------------------------------------------- */

static blockdev_t g_dev[BLOCKDEV_MAX];
static int        g_ndev;

/* --- per-driver read adapters (each matches blockdev_t.read / blk_read_fn) --- */

/* ATA: the drive index is packed into ctx (drives 0..3 share one read fn). */
static int ata_bd_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    int drive = (int)(intptr_t)ctx;
    if (lba > 0xFFFFFFFFull) return -1;            /* ata_read_drive takes a u32 LBA */
    return ata_read_drive(drive, (uint32_t)lba, count, buf);
}
/* AHCI: the SATA-disk index is packed into ctx. */
static int ahci_bd_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    return ahci_read((int)(intptr_t)ctx, lba, count, buf);
}
/* virtio-blk: single device, ctx unused. */
static int virtio_bd_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx; return virtio_blk_read(lba, count, buf);
}
/* NVMe: namespace 1, ctx unused. */
static int nvme_bd_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx; return nvme_read(lba, count, buf);
}
/* USB mass-storage: single device, ctx unused; its read takes a u32 LBA. */
static int usb_bd_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx;
    if (lba > 0xFFFFFFFFull) return -1;
    return usb_storage_read((uint32_t)lba, count, buf);
}

static void reg(const char *name, int (*read)(void *, uint64_t, uint32_t, void *),
                uint64_t sectors, void *ctx) {
    if (g_ndev >= BLOCKDEV_MAX) return;
    g_dev[g_ndev].name    = name;
    g_dev[g_ndev].read    = read;
    g_dev[g_ndev].sectors = sectors;
    g_dev[g_ndev].ctx     = ctx;
    g_ndev++;
}

int blockdev_init(void) {
    g_ndev = 0;

    /* ATA drives 0..3 — those that answered IDENTIFY. Static names so the
     * registered pointer stays valid for the lifetime of the kernel. */
    static const char *ata_names[ATA_MAX_DRIVES] = {
        "ata0", "ata1", "ata2", "ata3"
    };
    ata_identify_all();                            /* idempotent; ensure probed */
    for (int d = 0; d < ATA_MAX_DRIVES; d++) {
        const struct ata_drive_info *info = ata_drive(d);
        if (info && info->present)
            reg(ata_names[d], ata_bd_read, info->sectors, (void *)(intptr_t)d);
    }

    /* AHCI SATA disks. ahci.c exposes no capacity query, so register capacity 0
     * (unknown): blockdev_read then trusts the driver's own bounds-checking, and
     * the FAT walk stays bounded by the cluster count regardless. */
    static const char *ahci_names[4] = { "ahci0", "ahci1", "ahci2", "ahci3" };
    int nahci = ahci_disk_count();
    for (int a = 0; a < nahci && a < 4; a++)
        reg(ahci_names[a], ahci_bd_read, 0, (void *)(intptr_t)a);

    /* virtio-blk (single paravirtual disk). */
    if (virtio_blk_present())
        reg("virtio-blk", virtio_bd_read, virtio_blk_capacity(), 0);

    /* NVMe namespace 1. */
    if (nvme_present())
        reg("nvme0n1", nvme_bd_read, nvme_capacity(), 0);

    /* USB mass-storage. */
    if (usb_storage_present())
        reg("usb-storage", usb_bd_read, usb_storage_capacity(), 0);

    return g_ndev;
}

int blockdev_count(void) { return g_ndev; }

blockdev_t *blockdev_get(int i) {
    if (i < 0 || i >= g_ndev) return 0;
    return &g_dev[i];
}

int blockdev_read(int i, uint64_t lba, uint32_t count, void *buf) {
    if (i < 0 || i >= g_ndev || !buf || count == 0) return -1;
    blockdev_t *d = &g_dev[i];
    if (!d->read) return -1;
    /* Range-check against the known capacity (0 = unknown -> defer to the driver). */
    if (d->sectors) {
        if (lba >= d->sectors) return -1;
        if (lba + count < lba) return -1;            /* 64-bit overflow */
        if (lba + count > d->sectors) return -1;
    }
    return d->read(d->ctx, lba, count, buf) < 0 ? -1 : 0;
}

/* --- FAT32 volume discovery over a generic block device -------------------- */

/* A blk_read_fn (for fatvol_*) bound to one registered device by index. The
 * device index is packed into ctx. Reads go through blockdev_read so the device's
 * capacity bound (where known) is enforced. */
static int bd_blk_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    return blockdev_read((int)(intptr_t)ctx, lba, count, buf);
}

/* Collect candidate FAT32-volume start-LBAs on device `i` into `starts` (up to
 * `max`). A device either holds a bare filesystem at LBA 0 (no table) or an
 * MBR/GPT table carving it into partitions; we collect LBA 0 for the bare case
 * and each partition's start-LBA otherwise. fatvol_*() re-validates each as FAT32,
 * so collecting a non-FAT32 candidate is harmless. Returns the candidate count. */
static int collect_fat_starts(int i, uint64_t *starts, int max) {
    if (max <= 0) return 0;
    blockdev_t *d = blockdev_get(i);
    if (!d) return 0;
    uint64_t cap = d->sectors;                       /* 0 = unknown */

    uint8_t lba0[SECSZ];
    if (blockdev_read(i, 0, 1, lba0) < 0) return 0;

    /* No 0x55AA boot signature at LBA 0 => no MBR/GPT and no FAT32 BPB here. */
    if (rd16(lba0 + 510) != 0xAA55) return 0;

    /* Is LBA 0 itself a FAT32 BPB (a bare, unpartitioned volume)? Heuristic only —
     * fatvol_*() does the authoritative validation. The "FAT32" type string sits
     * at offset 82 in a FAT32 BPB; bytes/sector at 11 must be 512. */
    int n = 0;
    if (rd16(lba0 + 11) == SECSZ && memcmp(lba0 + 82, "FAT32", 5) == 0) {
        starts[n++] = 0;
        return n;                                    /* a bare volume has no table */
    }

    /* Otherwise treat LBA 0 as an MBR (or protective MBR for GPT). GPT if any of
     * the four primary entries is the 0xEE protective type. */
    int is_gpt = 0;
    for (int k = 0; k < 4; k++)
        if (lba0[0x1BE + k * 16 + 4] == 0xEE) { is_gpt = 1; break; }

    if (!is_gpt) {
        /* --- classic MBR: four 16-byte primary entries at 0x1BE --- */
        for (int k = 0; k < 4 && n < max; k++) {
            const uint8_t *e = lba0 + 0x1BE + k * 16;
            uint8_t type = e[4];
            if (type == 0x00) continue;              /* empty slot */
            if (type == 0xEE) continue;              /* protective (handled by GPT path) */
            uint64_t start = rd32(e + 8);
            uint64_t count = rd32(e + 12);
            if (count == 0) continue;
            if (start + count < start) continue;     /* overflow */
            if (cap && start >= cap) continue;       /* runs off a known-size disk */
            starts[n++] = start;
        }
        return n;
    }

    /* --- GPT: header at LBA 1, entry array it points at --- */
    uint8_t hdr[SECSZ];
    if (blockdev_read(i, 1, 1, hdr) < 0) return n;
    if (memcmp(hdr, "EFI PART", 8) != 0) return n;   /* protective MBR but no GPT */

    uint64_t arr_lba    = rd64(hdr + 72);
    uint32_t nentries   = rd32(hdr + 80);
    uint32_t entry_size = rd32(hdr + 84);
    if (entry_size < 128 || entry_size > SECSZ) return n;
    if (SECSZ % entry_size != 0) return n;           /* must tile a sector cleanly */
    if (nentries == 0) return n;
    if (nentries > 512) nentries = 512;              /* cap a runaway entry count */
    if (cap && arr_lba >= cap) return n;

    uint32_t per_sec = SECSZ / entry_size;
    uint32_t sectors_needed = (nentries + per_sec - 1) / per_sec;
    for (uint32_t s = 0; s < sectors_needed && n < max; s++) {
        uint64_t lba = arr_lba + s;
        if (cap && lba >= cap) break;
        uint8_t buf[SECSZ];
        if (blockdev_read(i, lba, 1, buf) < 0) break;
        for (uint32_t e = 0; e < per_sec && n < max; e++) {
            uint32_t idx = s * per_sec + e;
            if (idx >= nentries) break;
            const uint8_t *ent = buf + e * entry_size;   /* e*entry_size < SECSZ */
            int used = 0;
            for (int b = 0; b < 16; b++) if (ent[b]) { used = 1; break; }
            if (!used) continue;                     /* all-zero type GUID = unused */
            uint64_t first = rd64(ent + 32);
            uint64_t last  = rd64(ent + 40);
            if (last < first) continue;
            if (cap && first >= cap) continue;
            starts[n++] = first;
        }
    }
    return n;
}

/* --- the headless browsing demo -------------------------------------------- */

void blockdev_enumerate(void) {
    int ndev = blockdev_init();
    kprintf("[ ok ] block devices: %d present (browsable across all storage drivers).\n",
            ndev);

    int total_volumes = 0;
    for (int i = 0; i < ndev; i++) {
        blockdev_t *d = blockdev_get(i);
        if (!d) continue;
        if (d->sectors)
            kprintf("  blockdev %d: %s, %lu sectors (%lu MiB)\n",
                    i, d->name, (unsigned long)d->sectors,
                    (unsigned long)(d->sectors / 2048));
        else
            kprintf("  blockdev %d: %s, capacity unknown\n", i, d->name);

        uint64_t starts[16];
        int nstart = collect_fat_starts(i, starts, 16);
        if (nstart == 0) {
            kprintf("    no FAT32 volume found (not FAT32 / unreadable).\n");
            continue;
        }

        for (int v = 0; v < nstart; v++) {
            uint64_t start = starts[v];
            fatvol_dirent ents[32];
            int n = fatvol_list(bd_blk_read, (void *)(intptr_t)i, start, ents, 32);
            if (n <= 0) {
                /* The candidate wasn't actually a readable FAT32 volume — skip
                 * cleanly (e.g. a non-FAT32 partition, or a read error). */
                continue;
            }
            total_volumes++;
            kprintf("    FAT32 volume mounted (read-only) at start-LBA %lu: %d root entr%s\n",
                    (unsigned long)start, n, n == 1 ? "y" : "ies");
            for (int e = 0; e < n; e++) {
                if (ents[e].is_dir)
                    kprintf("        %s/  (dir)\n", ents[e].name);
                else
                    kprintf("        %s  (%lu bytes)\n",
                            ents[e].name, (unsigned long)ents[e].size);
            }
        }
    }

    kprintf("[ ok ] blockdev browse: %d FAT32 volume(s) listed across %d device(s).\n",
            total_volumes, ndev);
}

/* --- text formatting for the userspace `lsblk` shell command (SYS_lsblk) ----
 * Bounded string/decimal appenders, then the same browse as above but written to
 * a caller buffer instead of the serial log. */
static int sapp(char *b, int p, int max, const char *s) {
    while (*s && p < max - 1) b[p++] = *s++;
    return p;
}
static int sdec(char *b, int p, int max, uint64_t v) {
    char t[24]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v && n < 24) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n && p < max - 1) b[p++] = t[--n];
    return p;
}

int blockdev_format(char *out, int max) {
    if (!out || max < 2) return 0;
    int ndev = blockdev_init();
    int p = 0;
    p = sapp(out, p, max, "block devices: ");
    p = sdec(out, p, max, (uint64_t)ndev);
    p = sapp(out, p, max, " present\n");
    for (int i = 0; i < ndev; i++) {
        blockdev_t *d = blockdev_get(i);
        if (!d) continue;
        p = sapp(out, p, max, "  ");
        p = sapp(out, p, max, d->name);
        if (d->sectors) {
            p = sapp(out, p, max, "  ");
            p = sdec(out, p, max, d->sectors / 2048);
            p = sapp(out, p, max, " MiB");
        } else {
            p = sapp(out, p, max, "  (size unknown)");
        }
        p = sapp(out, p, max, "\n");
        uint64_t starts[16];
        int nstart = collect_fat_starts(i, starts, 16);
        for (int v = 0; v < nstart; v++) {
            fatvol_dirent ents[32];
            int n = fatvol_list(bd_blk_read, (void *)(intptr_t)i, starts[v], ents, 32);
            if (n <= 0) continue;
            p = sapp(out, p, max, "    FAT32 @ LBA ");
            p = sdec(out, p, max, starts[v]);
            p = sapp(out, p, max, ":\n");
            for (int e = 0; e < n; e++) {
                p = sapp(out, p, max, "      ");
                p = sapp(out, p, max, ents[e].name);
                if (ents[e].is_dir) {
                    p = sapp(out, p, max, "/");
                } else {
                    p = sapp(out, p, max, "  (");
                    p = sdec(out, p, max, ents[e].size);
                    p = sapp(out, p, max, " bytes)");
                }
                p = sapp(out, p, max, "\n");
            }
        }
    }
    if (p < max) out[p] = 0;
    return p;
}
