/*
 * atapi.c — ATAPI CD-ROM read driver (SCSI PACKET commands over the ATA bus),
 * PIO / polled (M1852). Additive: ata.c is the plain-ATA disk driver and
 * explicitly SKIPS ATAPI-signature devices; this module claims those instead,
 * so the boot ATA path is byte-for-byte untouched. Read-only (CDs are), 2048-byte
 * logical sectors via READ(10); capacity via READ CAPACITY(10).
 *
 * The one shared caveat: the ATA command-block ports are a single hardware
 * resource. This driver's self-test runs at boot (single-threaded, before any
 * concurrent disk I/O) and a CD is a distinct device, so no locking is needed
 * for the self-test; a general concurrent CD+disk workload would need ata.c's
 * shared lock (out of scope — the self-test proves the transport).
 *
 * Verify: attach a CD image in QEMU (`-cdrom foo.iso`) in a SEPARATE invocation
 * (default boot stays on ATA+e1000 so make check is unaffected); the self-test
 * reads the ISO 9660 Primary Volume Descriptor (logical sector 16) and checks
 * its "CD001" signature. See tests/run-atapi-tests.sh.
 */
#include "atapi.h"
#include "io.h"
#include "console.h"
#include "string.h"

/* command-block register offsets (same layout as ata.c) */
#define REG_DATA      0
#define REG_FEATURES  1
#define REG_SECCOUNT  2     /* ATAPI: "interrupt reason" on read */
#define REG_LBA0      3
#define REG_LBA1      4     /* ATAPI: byte-count low */
#define REG_LBA2      5     /* ATAPI: byte-count high */
#define REG_DRIVE     6
#define REG_STATUS    7
#define REG_COMMAND   7

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define CMD_IDENTIFY        0xEC   /* IDENTIFY DEVICE (an ATAPI device aborts + sets its signature) */
#define CMD_PACKET          0xA0   /* send a SCSI command packet */
#define ATAPI_SECSZ         2048

/* the four legacy ATA slots: primary/secondary channel, master/slave */
static const struct { uint16_t io, ctrl; uint8_t slave; } CH[ATAPI_MAX] = {
    { 0x1F0, 0x3F6, 0 }, { 0x1F0, 0x3F6, 1 },
    { 0x170, 0x376, 0 }, { 0x170, 0x376, 1 },
};

static struct { int present; uint32_t last_lba; uint32_t block_size; } g_cd[ATAPI_MAX];
static int g_probed;

static void adelay(uint16_t io) { for (int i = 0; i < 4; i++) (void)inb(io + REG_STATUS); }

/* wait (bounded) until BSY clears; returns the last status, or 0xFF on timeout. */
static uint8_t wait_not_busy(uint16_t io) {
    int spun = 0;
    for (;;) {
        uint8_t s = inb(io + REG_STATUS);
        if (!(s & ST_BSY)) return s;
        if (++spun > 5000000) return 0xFF;
    }
}

/* Probe slot for an ATAPI device: IDENTIFY DEVICE, which an ATAPI unit aborts,
 * leaving the ATAPI signature (LBA1=0x14, LBA2=0xEB) in the cylinder registers. */
static int atapi_detect(int slot) {
    uint16_t io = CH[slot].io; uint8_t slave = CH[slot].slave;
    outb(io + REG_DRIVE, 0xA0 | (slave ? 0x10 : 0));
    adelay(io);
    uint8_t st = inb(io + REG_STATUS);
    if (st == 0xFF || st == 0x00) return 0;             /* floating bus / empty */
    outb(io + REG_SECCOUNT, 0); outb(io + REG_LBA0, 0);
    outb(io + REG_LBA1, 0); outb(io + REG_LBA2, 0);
    outb(io + REG_COMMAND, CMD_IDENTIFY);
    st = inb(io + REG_STATUS);
    if (st == 0x00) return 0;
    if (wait_not_busy(io) == 0xFF) return 0;
    uint8_t lo = inb(io + REG_LBA1), hi = inb(io + REG_LBA2);
    return (lo == 0x14 && hi == 0xEB);                  /* the ATAPI signature */
}

/* Run one SCSI command packet (`cdb`, 12 bytes) expecting up to `buflen` bytes of
 * input data into `buf`. Returns bytes read (>=0), or -1 on error/timeout. */
static int atapi_packet(int slot, const uint8_t cdb[12], uint8_t *buf, int buflen) {
    uint16_t io = CH[slot].io; uint8_t slave = CH[slot].slave;
    outb(io + REG_DRIVE, 0xA0 | (slave ? 0x10 : 0));
    adelay(io);
    outb(io + REG_FEATURES, 0);                          /* PIO, no DMA/overlap */
    int limit = buflen > 0xFFFE ? 0xFFFE : buflen;       /* per-DRQ byte-count ceiling (16-bit) */
    outb(io + REG_LBA1, (uint8_t)(limit & 0xFF));
    outb(io + REG_LBA2, (uint8_t)(limit >> 8));
    outb(io + REG_COMMAND, CMD_PACKET);

    /* wait for the device to request the command packet (DRQ), bounded */
    uint8_t st = wait_not_busy(io);
    if (st == 0xFF || (st & ST_ERR) || !(st & ST_DRQ)) return -1;
    for (int i = 0; i < 6; i++)                           /* write the 12-byte CDB as 6 words */
        outw(io + REG_DATA, (uint16_t)(cdb[2*i] | (cdb[2*i+1] << 8)));

    int got = 0;                                         /* read data in DRQ bursts */
    for (;;) {
        st = wait_not_busy(io);
        if (st == 0xFF || (st & ST_ERR)) return -1;
        if (!(st & ST_DRQ)) break;                       /* command complete (no more data) */
        int cnt = inb(io + REG_LBA1) | (inb(io + REG_LBA2) << 8);   /* bytes available this burst */
        if (cnt <= 0) break;
        for (int i = 0; i < cnt; i += 2) {
            uint16_t w = inw(io + REG_DATA);
            if (got < buflen) buf[got++] = (uint8_t)w;
            if (got < buflen) buf[got++] = (uint8_t)(w >> 8);
        }
    }
    return got;
}

int atapi_read_capacity(int slot, uint32_t *last_lba, uint32_t *block_size) {
    if (slot < 0 || slot >= ATAPI_MAX || !g_cd[slot].present) return -1;
    uint8_t cdb[12] = { 0x25, 0,0,0,0,0,0,0,0,0,0,0 };   /* READ CAPACITY(10) */
    uint8_t cap[8];
    if (atapi_packet(slot, cdb, cap, 8) < 8) return -1;
    uint32_t ll = ((uint32_t)cap[0]<<24)|((uint32_t)cap[1]<<16)|((uint32_t)cap[2]<<8)|cap[3];
    uint32_t bs = ((uint32_t)cap[4]<<24)|((uint32_t)cap[5]<<16)|((uint32_t)cap[6]<<8)|cap[7];
    if (last_lba) *last_lba = ll;
    if (block_size) *block_size = bs;
    return 0;
}

/* Read `count` 2048-byte logical sectors starting at `lba` into `buf`. Returns
 * bytes read, or -1. */
int atapi_read10(int slot, uint32_t lba, uint16_t count, uint8_t *buf, int buflen) {
    if (slot < 0 || slot >= ATAPI_MAX || !g_cd[slot].present || count == 0) return -1;
    uint8_t cdb[12] = { 0x28, 0,
        (uint8_t)(lba>>24), (uint8_t)(lba>>16), (uint8_t)(lba>>8), (uint8_t)lba,
        0, (uint8_t)(count>>8), (uint8_t)count, 0, 0, 0 };
    int want = (int)count * ATAPI_SECSZ; if (want > buflen) want = buflen;
    return atapi_packet(slot, cdb, buf, want);
}

int atapi_present(int slot) { return slot >= 0 && slot < ATAPI_MAX && g_cd[slot].present; }

/* Capacity in 512-byte units (2048-byte media exposed as 4x 512-byte sectors), for
 * the block layer. 0 if absent. */
uint32_t atapi_capacity512(int slot) {
    if (slot < 0 || slot >= ATAPI_MAX || !g_cd[slot].present) return 0;
    return (g_cd[slot].last_lba + 1) * 4;
}

void atapi_init(void) {
    if (g_probed) return;
    g_probed = 1;
    for (int s = 0; s < ATAPI_MAX; s++) {
        if (atapi_detect(s)) {
            g_cd[s].present = 1;
            /* Detection only proves an ATAPI-signature DEVICE answered; it does
             * NOT prove a readable disc. A CD-ROM drive with no media — and
             * QEMU's PIIX3 secondary channel, which presents a phantom ATAPI stub
             * even with no -cdrom — answers IDENTIFY with the signature but has no
             * volume: READ CAPACITY fails or reports a zero block size / zero
             * size. Presenting such a drive only spams read failures (and used to
             * fail idedmatest via a stray "PVD read FAILED") and pollutes the
             * block layer with an unreadable device, so require a valid capacity
             * before treating it as a usable CD. (M1888) */
            if (atapi_read_capacity(s, &g_cd[s].last_lba, &g_cd[s].block_size) != 0
                || g_cd[s].block_size == 0 || g_cd[s].last_lba == 0) {
                g_cd[s].present = 0; g_cd[s].last_lba = 0; g_cd[s].block_size = 0;
            }
        }
    }
}

/* Boot self-test: no-op unless an ATAPI CD is attached. Reads the ISO 9660
 * Primary Volume Descriptor (logical sector 16) and checks its "CD001" magic —
 * proving detection + the PACKET transport + READ(10) at a non-zero LBA. */
void atapi_selftest(void) {
    atapi_init();
    for (int s = 0; s < ATAPI_MAX; s++) {
        if (!g_cd[s].present) continue;
        kprintf("[atapi] slot %d: ATAPI CD-ROM, %u sectors of %u bytes\n",
                s, (unsigned)(g_cd[s].last_lba + 1), (unsigned)g_cd[s].block_size);
        static uint8_t sec[ATAPI_SECSZ];
        int n = atapi_read10(s, 16, 1, sec, sizeof sec);      /* the ISO 9660 PVD */
        if (n < 6) { kprintf("[atapi] slot %d: PVD read FAILED (n=%d)\n", s, n); continue; }
        if (sec[0] == 1 && memcmp(sec + 1, "CD001", 5) == 0)
            kprintf("[atapi] slot %d: read PVD ok, CD001 found (ISO 9660 volume)\n", s);
        else
            kprintf("[atapi] slot %d: PVD read but no CD001 magic (byte0=%d)\n", s, sec[0]);
        return;                                                /* one CD is enough for the self-test */
    }
}
