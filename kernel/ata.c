/*
 * ata.c — minimal ATA (IDE) disk driver, PIO mode, primary bus / master.
 *
 * This is the *block device*: the lowest layer of storage, which reads and
 * writes the disk in fixed 512-byte sectors addressed by a linear number (LBA).
 * The filesystem (FAT32) sits on top and gives those sectors meaning (files and
 * directories).
 *
 * "PIO" (Programmed I/O) means the CPU itself moves every word through the data
 * port — slow, but dead simple and perfect for learning. We talk to the
 * controller through its command-block I/O ports (0x1F0..0x1F7).
 */
#include "ata.h"
#include "io.h"

#define ATA_DATA     0x1F0
#define ATA_SECCOUNT 0x1F2
#define ATA_LBA_LOW  0x1F3
#define ATA_LBA_MID  0x1F4
#define ATA_LBA_HIGH 0x1F5
#define ATA_DRIVE    0x1F6
#define ATA_STATUS   0x1F7   /* read */
#define ATA_COMMAND  0x1F7   /* write */

#define ST_BSY 0x80          /* busy */
#define ST_DRQ 0x08          /* data request ready */
#define ST_ERR 0x01

#define CMD_READ_SECTORS  0x20
#define CMD_WRITE_SECTORS 0x30
#define CMD_FLUSH         0xE7

static int wait_busy_clear(void) {
    for (int i = 0; i < 100000; i++)
        if (!(inb(ATA_STATUS) & ST_BSY))
            return 0;
    return -1;
}

static int wait_drq(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & ST_ERR) return -1;
        if (!(s & ST_BSY) && (s & ST_DRQ)) return 0;
    }
    return -1;
}

/* Read `count` words from the data port into `buf`. */
static void read_data(void *buf, int words) {
    __asm__ volatile("rep insw"
                     : "+D"(buf), "+c"(words)
                     : "d"(ATA_DATA)
                     : "memory");
}

static void write_data(const void *buf, int words) {
    __asm__ volatile("rep outsw"
                     : "+S"(buf), "+c"(words)
                     : "d"(ATA_DATA)
                     : "memory");
}

static void select_lba(uint32_t lba, uint8_t count) {
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA_LOW,  (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
}

int ata_read(uint32_t lba, uint8_t count, void *buf) {
    if (wait_busy_clear() < 0)
        return -1;
    select_lba(lba, count);
    outb(ATA_COMMAND, CMD_READ_SECTORS);

    uint8_t *p = buf;
    for (int s = 0; s < count; s++) {
        if (wait_drq() < 0)
            return -1;
        read_data(p, SECTOR_SIZE / 2);   /* 256 words = 512 bytes */
        p += SECTOR_SIZE;
    }
    return 0;
}

int ata_write(uint32_t lba, uint8_t count, const void *buf) {
    if (wait_busy_clear() < 0)
        return -1;
    select_lba(lba, count);
    outb(ATA_COMMAND, CMD_WRITE_SECTORS);

    const uint8_t *p = buf;
    for (int s = 0; s < count; s++) {
        if (wait_drq() < 0)
            return -1;
        write_data(p, SECTOR_SIZE / 2);
        p += SECTOR_SIZE;
    }
    outb(ATA_COMMAND, CMD_FLUSH);        /* flush the write cache */
    wait_busy_clear();
    return 0;
}
