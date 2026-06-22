/*
 * ata.c — minimal ATA (IDE) disk driver, PIO mode.
 *
 * This is the *block device*: the lowest layer of storage, which reads and
 * writes the disk in fixed 512-byte sectors addressed by a linear number (LBA).
 * The filesystem (FAT32) sits on top and gives those sectors meaning (files and
 * directories).
 *
 * "PIO" (Programmed I/O) means the CPU itself moves every word through the data
 * port — slow, but dead simple and perfect for learning. We talk to the
 * controller through its command-block I/O ports.
 *
 * The legacy PC has up to four ATA disks on two buses:
 *
 *   drive 0  primary master    I/O 0x1F0  ctrl 0x3F6   drive-select 0xE0
 *   drive 1  primary slave     I/O 0x1F0  ctrl 0x3F6   drive-select 0xF0
 *   drive 2  secondary master  I/O 0x170  ctrl 0x376   drive-select 0xE0
 *   drive 3  secondary slave   I/O 0x170  ctrl 0x376   drive-select 0xF0
 *
 * The original driver spoke only to drive 0 (the boot disk). ata_read()/
 * ata_write() still do exactly that — they are now thin wrappers over the
 * drive-parameterised core — so the FAT32 boot mount is untouched. The new
 * ata_read_drive()/ata_identify_all() add the other three drives so the
 * partition layer can enumerate every disk attached to the machine.
 */
#include "ata.h"
#include "io.h"
#include "console.h"

/* Per-bus command-block + control-block base ports. A drive's I/O base depends
 * only on its bus (primary/secondary); master vs slave is the DRV bit in the
 * drive-select register (0xA0 base, |0x10 = slave). */
#define PRIMARY_IO    0x1F0
#define PRIMARY_CTRL  0x3F6
#define SECONDARY_IO  0x170
#define SECONDARY_CTRL 0x376

/* Command-block register offsets from the I/O base. */
#define REG_DATA      0      /* r/w  16-bit data port */
#define REG_ERROR     1      /* r    error */
#define REG_FEATURES  1      /* w    features */
#define REG_SECCOUNT  2      /* r/w  sector count */
#define REG_LBA0      3      /* r/w  LBA bits 0..7   (also sector number) */
#define REG_LBA1      4      /* r/w  LBA bits 8..15  (also cylinder low) */
#define REG_LBA2      5      /* r/w  LBA bits 16..23 (also cylinder high) */
#define REG_DRIVE     6      /* r/w  drive/head select */
#define REG_STATUS    7      /* r    status */
#define REG_COMMAND   7      /* w    command */

#define ST_BSY 0x80          /* busy */
#define ST_DRDY 0x40         /* device ready */
#define ST_DRQ 0x08          /* data request ready */
#define ST_ERR 0x01

#define CMD_READ_SECTORS  0x20
#define CMD_WRITE_SECTORS 0x30
#define CMD_FLUSH         0xE7
#define CMD_IDENTIFY      0xEC

/* The four legacy drives, in index order. */
static const struct { uint16_t io, ctrl; uint8_t slave; } ATA_DRIVES[ATA_MAX_DRIVES] = {
    { PRIMARY_IO,   PRIMARY_CTRL,   0 },   /* 0: primary master   */
    { PRIMARY_IO,   PRIMARY_CTRL,   1 },   /* 1: primary slave    */
    { SECONDARY_IO, SECONDARY_CTRL, 0 },   /* 2: secondary master */
    { SECONDARY_IO, SECONDARY_CTRL, 1 },   /* 3: secondary slave  */
};

/* Discovered drives, filled lazily on first IDENTIFY (ata_identify_all). */
static struct ata_drive_info g_drives[ATA_MAX_DRIVES];
static int g_probed;          /* have we run the IDENTIFY sweep yet? */

static int drive_ok(int drive) { return drive >= 0 && drive < ATA_MAX_DRIVES; }

/* --- low-level busy/DRQ polling, scoped to a given I/O base ---------------- */

static int wait_busy_clear(uint16_t io) {
    for (int i = 0; i < 100000; i++)
        if (!(inb(io + REG_STATUS) & ST_BSY))
            return 0;
    return -1;
}

static int wait_drq(uint16_t io) {
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(io + REG_STATUS);
        if (s & ST_ERR) return -1;
        if (!(s & ST_BSY) && (s & ST_DRQ)) return 0;
    }
    return -1;
}

/* Read `words` 16-bit words from the data port into `buf`. */
static void read_data(uint16_t io, void *buf, int words) {
    __asm__ volatile("rep insw"
                     : "+D"(buf), "+c"(words)
                     : "d"((uint16_t)(io + REG_DATA))
                     : "memory");
}

static void write_data(uint16_t io, const void *buf, int words) {
    __asm__ volatile("rep outsw"
                     : "+S"(buf), "+c"(words)
                     : "d"((uint16_t)(io + REG_DATA))
                     : "memory");
}

/* Select a drive + program an LBA28 access. `slave` picks master/slave; the top
 * LBA nibble goes in the low 4 bits of the drive register. */
static void select_lba(uint16_t io, uint8_t slave, uint32_t lba, uint8_t count) {
    outb(io + REG_DRIVE, 0xE0 | (slave ? 0x10 : 0) | ((lba >> 24) & 0x0F));
    outb(io + REG_SECCOUNT, count);
    outb(io + REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(io + REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(io + REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
}

/* --- drive-parameterised read/write core ---------------------------------- */

int ata_read_drive(int drive, uint32_t lba, uint32_t count, void *buf) {
    if (!drive_ok(drive) || count == 0) return -1;
    uint16_t io = ATA_DRIVES[drive].io;
    uint8_t slave = ATA_DRIVES[drive].slave;

    /* LBA28 sector count is an 8-bit field (0 means 256). Loop in chunks so a
     * caller can request more than 256 sectors in one call. */
    uint8_t *p = buf;
    while (count > 0) {
        uint32_t chunk = count > 256 ? 256 : count;
        if (wait_busy_clear(io) < 0)
            return -1;
        select_lba(io, slave, lba, (uint8_t)(chunk & 0xFF));   /* 256 -> 0 in the register */
        outb(io + REG_COMMAND, CMD_READ_SECTORS);
        for (uint32_t s = 0; s < chunk; s++) {
            if (wait_drq(io) < 0)
                return -1;
            read_data(io, p, SECTOR_SIZE / 2);                 /* 256 words = 512 bytes */
            p += SECTOR_SIZE;
        }
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

int ata_write_drive(int drive, uint32_t lba, uint32_t count, const void *buf) {
    if (!drive_ok(drive) || count == 0) return -1;
    uint16_t io = ATA_DRIVES[drive].io;
    uint8_t slave = ATA_DRIVES[drive].slave;

    const uint8_t *p = buf;
    while (count > 0) {
        uint32_t chunk = count > 256 ? 256 : count;
        if (wait_busy_clear(io) < 0)
            return -1;
        select_lba(io, slave, lba, (uint8_t)(chunk & 0xFF));
        outb(io + REG_COMMAND, CMD_WRITE_SECTORS);
        for (uint32_t s = 0; s < chunk; s++) {
            if (wait_drq(io) < 0)
                return -1;
            write_data(io, p, SECTOR_SIZE / 2);
            p += SECTOR_SIZE;
        }
        outb(io + REG_COMMAND, CMD_FLUSH);        /* flush the write cache */
        wait_busy_clear(io);
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

/* --- the original primary-master API, unchanged for every existing caller --- */

int ata_read(uint32_t lba, uint8_t count, void *buf) {
    /* count==0 meant "256 sectors" in the old LBA28 sense; preserve that. */
    return ata_read_drive(0, lba, count ? count : 256, buf);
}

int ata_write(uint32_t lba, uint8_t count, const void *buf) {
    return ata_write_drive(0, lba, count ? count : 256, buf);
}

/* --- IDENTIFY-based enumeration -------------------------------------------- */

/*
 * Probe one drive with IDENTIFY DEVICE. Returns 1 + fills *out if a usable ATA
 * disk answered, 0 if the slot is empty / not an ATA disk.
 *
 * Robustness is the whole point here: an absent drive on a "floating" bus must
 * NOT hang the kernel. We follow the standard sequence with finite spins —
 * select the drive, issue IDENTIFY, and if the status register reads 0 (no
 * device on this bus at all) bail immediately; otherwise wait for BSY to clear
 * within a bounded loop and require DRQ. A device that signals it is ATAPI/SATA
 * (the LBA1/LBA2 signature bytes become non-zero after IDENTIFY) is treated as
 * "no PIO ATA disk here" and skipped — we are an LBA28/48 PIO driver only.
 */
static int identify_drive(int drive, struct ata_drive_info *out) {
    uint16_t io = ATA_DRIVES[drive].io;
    uint8_t slave = ATA_DRIVES[drive].slave;

    /* Select the drive and give it a moment (400ns ~= 4 status reads). */
    outb(io + REG_DRIVE, 0xA0 | (slave ? 0x10 : 0));
    for (int i = 0; i < 4; i++) (void)inb(io + REG_STATUS);

    /* Status 0xFF (or 0x00) => nothing on this bus: a floating bus reads all-ones
     * (open bus) or all-zeros. Bail before issuing any command so we never spin
     * waiting on a device that isn't there. */
    uint8_t st = inb(io + REG_STATUS);
    if (st == 0xFF || st == 0x00)
        return 0;

    /* Zero the addressing registers, then IDENTIFY. */
    outb(io + REG_SECCOUNT, 0);
    outb(io + REG_LBA0, 0);
    outb(io + REG_LBA1, 0);
    outb(io + REG_LBA2, 0);
    outb(io + REG_COMMAND, CMD_IDENTIFY);

    /* IDENTIFY of an absent device leaves status 0 -> no device. */
    st = inb(io + REG_STATUS);
    if (st == 0x00)
        return 0;

    /* Wait (bounded) for BSY to clear. */
    int spun = 0;
    while (inb(io + REG_STATUS) & ST_BSY) {
        if (++spun > 100000)
            return 0;                  /* stuck busy: treat as absent, don't hang */
    }

    /* Non-zero LBA-mid/high after IDENTIFY = an ATAPI/SATA signature, not a plain
     * ATA disk our PIO read path can serve. Skip it. */
    uint8_t lo = inb(io + REG_LBA1);
    uint8_t hi = inb(io + REG_LBA2);
    if (lo != 0 || hi != 0)
        return 0;

    /* Wait (bounded) for DRQ or ERR. */
    spun = 0;
    for (;;) {
        st = inb(io + REG_STATUS);
        if (st & ST_ERR) return 0;
        if (st & ST_DRQ) break;
        if (++spun > 100000) return 0;
    }

    /* Read the 256-word IDENTIFY block. */
    uint16_t id[256];
    read_data(io, id, 256);

    /* Sector count: prefer the LBA48 64-bit field (words 100..103) if the drive
     * supports 48-bit addressing (word 83 bit 10), else the LBA28 32-bit count
     * (words 60..61). */
    uint64_t sectors = 0;
    if (id[83] & (1u << 10)) {
        sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                  ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    }
    if (sectors == 0)
        sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);   /* LBA28 */
    if (sectors == 0)
        return 0;                       /* a present device that reports no capacity: ignore */

    out->present = 1;
    out->drive = drive;
    out->sectors = sectors;
    out->lba48 = (id[83] & (1u << 10)) ? 1 : 0;

    /* Pull the model string (words 27..46, byte-swapped ATA convention) for the
     * log. Trim trailing spaces. */
    int n = 0;
    for (int w = 27; w <= 46 && n < (int)sizeof(out->model) - 1; w++) {
        out->model[n++] = (char)(id[w] >> 8);
        out->model[n++] = (char)(id[w] & 0xFF);
    }
    while (n > 0 && (out->model[n - 1] == ' ' || out->model[n - 1] == '\0')) n--;
    out->model[n] = '\0';
    return 1;
}

int ata_identify_all(void) {
    int found = 0;
    for (int d = 0; d < ATA_MAX_DRIVES; d++) {
        g_drives[d].present = 0;
        g_drives[d].drive = d;
        g_drives[d].sectors = 0;
        g_drives[d].lba48 = 0;
        g_drives[d].model[0] = '\0';
        if (identify_drive(d, &g_drives[d]))
            found++;
    }
    g_probed = 1;
    return found;
}

const struct ata_drive_info *ata_drive(int drive) {
    if (!drive_ok(drive)) return 0;
    if (!g_probed) ata_identify_all();
    return &g_drives[drive];
}

uint64_t ata_drive_sectors(int drive) {
    const struct ata_drive_info *info = ata_drive(drive);
    return (info && info->present) ? info->sectors : 0;
}
