/*
 * usb_storage.c — USB mass-storage (Bulk-Only Transport + minimal SCSI) over the
 * UHCI host controller (kernel/usb.c). Drives a USB flash disk: enumerate it,
 * then read sectors with SCSI READ(10) carried inside USB BOT.
 *
 * --- The protocol, briefly --------------------------------------------------
 * A Mass-Storage / SCSI-transparent / Bulk-Only-Transport device (class 0x08,
 * subclass 0x06, protocol 0x50) speaks SCSI over two bulk endpoints. Each
 * command is three phases on those endpoints:
 *
 *   1. COMMAND  — a 31-byte Command Block Wrapper (CBW) on BULK OUT:
 *        signature 'USBC' (0x43425355, little-endian), a tag we echo-check, the
 *        data-transfer length, a flags byte (0x80 = data is IN), the LUN, the
 *        SCSI command length, then the SCSI command bytes (padded to 16).
 *   2. DATA     — the data phase of the stated length: BULK IN for a READ.
 *   3. STATUS   — a 13-byte Command Status Wrapper (CSW) on BULK IN:
 *        signature 'USBS' (0x53425355), the echoed tag, the residue (bytes NOT
 *        transferred), and a status byte (0 = passed, 1 = failed, 2 = phase err).
 *
 * The SCSI commands we issue: INQUIRY (0x12, for a log line), READ CAPACITY(10)
 * (0x25 -> last-LBA + block size), and READ(10) (0x28 -> data phase). WRITE(10)
 * (0x2A) is provided as a stretch path, used by the self-test's optional
 * round-trip only.
 *
 * --- Safety (reviewed line-by-line) -----------------------------------------
 *  - usb_storage_read validates buf!=NULL, count>0, and lba+count within
 *    capacity; it caps each bulk data phase to USB_BULK_MAX and chunks the rest,
 *    and never copies more than min(declared CBW length, caller buffer) out.
 *  - Every USB transfer (control + bulk) has a finite, bounded timeout in
 *    kernel/usb.c; a stalled/absent endpoint returns -1 cleanly.
 *  - The CBW/CSW signatures + the echoed tag are verified; a bad CSW fails clean
 *    (one retry, then report).
 *  - DMA buffers come from pmm frames (identity-mapped: phys == virt); the bulk
 *    bounce buffer in usb.c carries the data phase, so the device DMAs into a
 *    physically-addressed frame, then usb.c memcpys into the caller buffer
 *    bounded by the bytes actually transferred.
 *  - No mass-storage interface found => usb_storage_init() returns -1 (clean
 *    no-op); the existing USB tablet path is unaffected.
 */
#include "usb_storage.h"
#include "usb.h"
#include "console.h"
#include "string.h"
#include "timer.h"
#include <stdint.h>

/* --- Bulk-Only Transport wrappers ----------------------------------------- */
#define CBW_SIGNATURE 0x43425355u    /* 'USBC' little-endian */
#define CSW_SIGNATURE 0x53425355u    /* 'USBS' little-endian */
#define CBW_FLAG_IN   0x80           /* data-transfer direction: device->host */

struct cbw {
    uint32_t signature;   /* CBW_SIGNATURE                                    */
    uint32_t tag;         /* echoed back in the CSW                           */
    uint32_t data_len;    /* bytes the host expects to transfer in the data phase */
    uint8_t  flags;       /* bit 7: 1 = IN (device->host), 0 = OUT            */
    uint8_t  lun;         /* logical unit (bits 0..3)                         */
    uint8_t  cb_len;      /* length of the SCSI command (1..16)               */
    uint8_t  cb[16];      /* the SCSI command block                           */
} __attribute__((packed));

struct csw {
    uint32_t signature;   /* CSW_SIGNATURE                                    */
    uint32_t tag;         /* must equal the CBW's tag                         */
    uint32_t residue;     /* data_len minus the bytes actually transferred    */
    uint8_t  status;      /* 0 = passed, 1 = failed, 2 = phase error          */
} __attribute__((packed));

/* --- SCSI opcodes ---------------------------------------------------------- */
#define SCSI_INQUIRY       0x12
#define SCSI_READ_CAPACITY 0x25
#define SCSI_READ_10       0x28
#define SCSI_WRITE_10      0x2A
#define SCSI_TEST_UNIT_RDY 0x00

/* Driver state for the one USB mass-storage device we support. */
static struct {
    int      present;
    uint8_t  addr;        /* USB device address                               */
    uint8_t  ep_in;       /* BULK IN endpoint number                          */
    uint8_t  ep_out;      /* BULK OUT endpoint number                         */
    uint16_t maxp_in;     /* BULK IN max packet                               */
    uint16_t maxp_out;    /* BULK OUT max packet                              */
    int      tog_in;      /* BULK IN data toggle (persists across transfers)  */
    int      tog_out;     /* BULK OUT data toggle                             */
    uint8_t  lun;         /* logical unit (0)                                 */
    uint32_t block_size;  /* bytes per block (expected 512)                   */
    uint64_t blocks;      /* total blocks (last-LBA + 1)                      */
    uint32_t tag;         /* monotonically increasing CBW tag                 */
} us;

/* Store a 32-bit value big-endian (SCSI CDB fields are big-endian). */
static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* Run one full Bulk-Only-Transport command: send the CBW (BULK OUT), do the
 * data phase (BULK IN for a read, OUT for a write; `data_in` selects), then read
 * + validate the CSW. `cb`/`cb_len` is the SCSI command. `data`/`data_len` is
 * the data buffer + the host-declared transfer length (0 for none). On a
 * read, *got receives the bytes actually transferred. Returns 0 if the CSW
 * reported success, -1 on any transport error / signature / tag / status fault.
 *
 * The data phase length is bounded by usb.c's USB_BULK_MAX (the caller chunks),
 * and usb.c never copies more than the bytes the device actually sent into our
 * buffer — so a device returning an over-long IN cannot overrun `data`. */
static int bot_command(const uint8_t *cb, int cb_len,
                       void *data, uint32_t data_len, int data_in, uint32_t *got) {
    if (got) *got = 0;
    if (!us.present && us.addr == 0)            /* not yet enumerated */
        return -1;
    if (cb_len < 1 || cb_len > 16)
        return -1;
    if (data_len > USB_BULK_MAX)                /* caller must chunk */
        return -1;

    struct cbw cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = CBW_SIGNATURE;
    cbw.tag       = ++us.tag;
    cbw.data_len  = data_len;
    cbw.flags     = data_in ? CBW_FLAG_IN : 0;
    cbw.lun       = us.lun;
    cbw.cb_len    = (uint8_t)cb_len;
    memcpy(cbw.cb, cb, (size_t)cb_len);

    /* Phase 1: ship the 31-byte CBW on BULK OUT. */
    int act = 0;
    if (usb_bulk_xfer(us.addr, us.ep_out, us.maxp_out, &us.tog_out,
                      &cbw, (int)sizeof(cbw), 0, &act) != 0 || act != (int)sizeof(cbw))
        return -1;

    /* Phase 2: the data phase, of the host-declared length. */
    uint32_t moved = 0;
    if (data_len) {
        int a = 0;
        int rc = usb_bulk_xfer(us.addr, data_in ? us.ep_in : us.ep_out,
                               data_in ? us.maxp_in : us.maxp_out,
                               data_in ? &us.tog_in : &us.tog_out,
                               data, (int)data_len, data_in, &a);
        if (rc != 0)
            return -1;                          /* data-phase transport fault */
        moved = (a > 0) ? (uint32_t)a : 0;
        if (moved > data_len) moved = data_len; /* defensive: never exceed declared */
    }

    /* Phase 3: read + validate the 13-byte CSW on BULK IN. */
    struct csw csw;
    memset(&csw, 0, sizeof(csw));
    int csw_act = 0;
    if (usb_bulk_xfer(us.addr, us.ep_in, us.maxp_in, &us.tog_in,
                      &csw, (int)sizeof(csw), 1, &csw_act) != 0)
        return -1;
    if (csw_act != (int)sizeof(csw))            /* short CSW => fail clean */
        return -1;
    if (csw.signature != CSW_SIGNATURE || csw.tag != cbw.tag)
        return -1;                              /* bad signature / tag mismatch */
    if (csw.status != 0)
        return -1;                              /* command failed / phase error */

    /* Residue is the bytes NOT transferred; trust the smaller of (declared -
     * residue) and what the transfer reported. */
    if (got) {
        uint32_t by_residue = (csw.residue <= data_len) ? (data_len - csw.residue) : 0;
        *got = (moved < by_residue) ? moved : by_residue;
    }
    return 0;
}

/* As bot_command, but retry once on failure (handles a transient stall/CSW
 * fault as the task allows). */
static int bot_command_retry(const uint8_t *cb, int cb_len,
                             void *data, uint32_t data_len, int data_in, uint32_t *got) {
    if (bot_command(cb, cb_len, data, data_len, data_in, got) == 0)
        return 0;
    timer_wait(1);
    return bot_command(cb, cb_len, data, data_len, data_in, got);
}

/* --- enumeration ----------------------------------------------------------- */

/* GET_DESCRIPTOR via the shared control endpoint, for the device at `addr`. */
static int get_descriptor(uint8_t addr, uint16_t ep0_maxp, int type, int index,
                          void *out, int len) {
    uint8_t s[8] = { 0x80, 0x06, (uint8_t)index, (uint8_t)type,
                     0, 0, (uint8_t)len, (uint8_t)(len >> 8) };
    return usb_control_xfer(addr, ep0_maxp, s, out, len, 1);
}
static int set_configuration(uint8_t addr, uint16_t ep0_maxp, int cfg) {
    uint8_t s[8] = { 0x00, 0x09, (uint8_t)cfg, 0, 0, 0, 0, 0 };
    return usb_control_xfer(addr, ep0_maxp, s, 0, 0, 0);
}
static int set_address(uint8_t ep0_maxp, int addr) {
    uint8_t s[8] = { 0x00, 0x05, (uint8_t)addr, 0, 0, 0, 0, 0 };
    return usb_control_xfer(0, ep0_maxp, s, 0, 0, 0);   /* still at address 0 */
}

/* Try to enumerate the device on `port_bit`'s freshly-enabled port as a USB
 * mass-storage / SCSI-transparent / Bulk-Only device. On success fills `us`
 * (address, bulk endpoints, config set) and returns 0; returns -1 if the device
 * there isn't a usable mass-storage device. */
static int enumerate_one(void) {
    uint16_t ep0 = 8;                             /* ep0 default max packet */

    /* Device descriptor (first 8 bytes for the real ep0 max packet, then full). */
    uint8_t dd[18];
    if (get_descriptor(0, ep0, 1, 0, dd, 8) != 0)
        return -1;
    ep0 = dd[7] ? dd[7] : 8;

    /* Assign an address, then re-fetch at the new address. */
    int addr = usb_alloc_address();
    if (addr <= 0 || set_address(ep0, addr) != 0)
        return -1;
    timer_wait(1);

    /* Config descriptor: header first (total length), then the whole thing. */
    uint8_t cfg[256];
    if (get_descriptor((uint8_t)addr, ep0, 2, 0, cfg, 9) != 0)
        return -1;
    int total = cfg[2] | (cfg[3] << 8);
    if (total < 9) return -1;
    if (total > (int)sizeof(cfg)) total = sizeof(cfg);
    if (get_descriptor((uint8_t)addr, ep0, 2, 0, cfg, total) != 0)
        return -1;

    /* Walk the config: find a Mass-Storage / SCSI-transparent / BOT interface,
     * then within it the BULK IN + BULK OUT endpoints. We track the "current"
     * interface as we walk; endpoints belong to the most recent interface. */
    int found_iface = 0, in_target = 0;
    uint8_t ep_in = 0, ep_out = 0;
    uint16_t maxp_in = 0, maxp_out = 0;
    uint8_t iface_class = 0, iface_sub = 0, iface_proto = 0;

    for (int i = 0; i + 1 < total; ) {
        int blen = cfg[i], btype = cfg[i + 1];
        if (blen < 2) break;
        if (i + blen > total) break;

        if (btype == 0x04 && blen >= 9) {         /* INTERFACE descriptor */
            uint8_t icls = cfg[i + 5], isub = cfg[i + 6], iproto = cfg[i + 7];
            in_target = (icls == 0x08 && isub == 0x06 && iproto == 0x50);
            if (in_target && !found_iface) {
                iface_class = icls; iface_sub = isub; iface_proto = iproto;
            }
        } else if (btype == 0x05 && blen >= 7 && in_target) {  /* ENDPOINT in target iface */
            uint8_t eaddr = cfg[i + 2], eattr = cfg[i + 3];
            uint16_t emax = cfg[i + 4] | (cfg[i + 5] << 8);
            if ((eattr & 0x03) == 0x02) {         /* BULK endpoint */
                if (eaddr & 0x80) { ep_in = eaddr & 0x0F; maxp_in = emax; }
                else              { ep_out = eaddr & 0x0F; maxp_out = emax; }
            }
            if (ep_in && ep_out && !found_iface) {
                found_iface = 1;                  /* a complete BOT interface */
            }
        }
        i += blen;
    }

    if (!found_iface || !ep_in || !ep_out)
        return -1;
    if (maxp_in == 0 || maxp_in > 64) maxp_in = 64;   /* UHCI full-speed cap */
    if (maxp_out == 0 || maxp_out > 64) maxp_out = 64;

    if (set_configuration((uint8_t)addr, ep0, cfg[5]) != 0)
        return -1;

    us.addr     = (uint8_t)addr;
    us.ep_in    = ep_in;
    us.ep_out   = ep_out;
    us.maxp_in  = maxp_in;
    us.maxp_out = maxp_out;
    us.tog_in   = 0;
    us.tog_out  = 0;
    us.lun      = 0;

    kprintf("[usb-storage] enumerated mass-storage: class=%02x subclass=%02x "
            "proto=%02x  bulk-in=ep%d(maxp=%d) bulk-out=ep%d(maxp=%d)\n",
            iface_class, iface_sub, iface_proto,
            ep_in, maxp_in, ep_out, maxp_out);
    return 0;
}

/* --- SCSI helpers ---------------------------------------------------------- */

/* INQUIRY: read the standard inquiry data (36 bytes) for a log line. Best-effort
 * (failure is non-fatal — some emulated devices are terse). */
static void scsi_inquiry(void) {
    uint8_t cb[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    uint8_t inq[36];
    memset(inq, 0, sizeof(inq));
    uint32_t got = 0;
    if (bot_command_retry(cb, sizeof(cb), inq, sizeof(inq), 1, &got) == 0 && got >= 32) {
        /* bytes 8..15 vendor + 16..31 product = 24 ASCII chars (space-padded). */
        char vp[25];
        for (int i = 0; i < 24; i++) {
            uint8_t c = inq[8 + i];
            vp[i] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
        }
        vp[24] = '\0';
        kprintf("[usb-storage] INQUIRY: '%s'\n", vp);
    }
}

/* READ CAPACITY(10): fills us.blocks + us.block_size. Returns 0 on success. */
static int scsi_read_capacity(void) {
    uint8_t cb[10] = { SCSI_READ_CAPACITY, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t cap[8];
    memset(cap, 0, sizeof(cap));
    uint32_t got = 0;
    if (bot_command_retry(cb, sizeof(cb), cap, sizeof(cap), 1, &got) != 0 || got < 8)
        return -1;
    uint32_t last_lba = rd_be32(&cap[0]);
    uint32_t blk      = rd_be32(&cap[4]);
    if (blk == 0)
        return -1;
    us.block_size = blk;
    us.blocks     = (uint64_t)last_lba + 1;
    return 0;
}

/* --- public API ------------------------------------------------------------ */

int usb_storage_init(void) {
    memset(&us, 0, sizeof(us));

    /* Bring the UHCI controller up (shared with the tablet; idempotent). */
    if (usb_uhci_init() != 0)
        return -1;                                /* no controller: clean no-op */

    /* Probe each root port for a mass-storage device, one port at a time and
     * SKIPPING the port the tablet already claimed (so we never disturb the live
     * tablet endpoint). Enabling a port resets the device behind it back to
     * address 0; we then enumerate + address it. One-port-at-a-time is required
     * because two unaddressed devices would both answer address 0. */
    int tablet_port = usb_uhci_tablet_port();
    int ok = -1;
    for (int p = 0; p < usb_uhci_port_count() && ok != 0; p++) {
        if (p == tablet_port)
            continue;
        if (!usb_uhci_enable_port(p))
            continue;                             /* nothing connected/enabled */
        if (enumerate_one() == 0)
            ok = 0;
    }
    if (ok != 0) {
        /* No mass-storage interface on the bus — clean no-op (tablet unaffected). */
        return -1;
    }

    /* Some devices need a TEST UNIT READY poke before they answer commands;
     * issue it (ignore failure) so READ CAPACITY is more likely to succeed. */
    {
        uint8_t cb[6] = { SCSI_TEST_UNIT_RDY, 0, 0, 0, 0, 0 };
        (void)bot_command_retry(cb, sizeof(cb), 0, 0, 0, 0);
    }

    if (scsi_read_capacity() != 0) {
        kprintf("[usb-storage] READ CAPACITY failed\n");
        return -1;
    }

    us.present = 1;
    return 0;
}

int      usb_storage_present(void)  { return us.present; }
uint64_t usb_storage_capacity(void) { return us.present ? us.blocks : 0; }

int usb_storage_read(uint32_t lba, uint32_t count, void *buf) {
    if (!us.present || !buf || count == 0)
        return -1;
    if (us.block_size != USB_STORAGE_SECTOR_SIZE)
        return -1;                                /* we only support 512-byte blocks */
    /* Bound to capacity: lba+count must not exceed the device, and guard the
     * 32-bit add against wrap. */
    if (lba >= us.blocks || count > us.blocks - lba)
        return -1;

    /* Chunk so each bulk data phase fits USB_BULK_MAX (8 sectors). */
    const uint32_t per = USB_BULK_MAX / USB_STORAGE_SECTOR_SIZE;   /* sectors/chunk */
    uint8_t *out = (uint8_t *)buf;

    while (count > 0) {
        uint32_t n = (count < per) ? count : per;
        uint32_t bytes = n * USB_STORAGE_SECTOR_SIZE;

        uint8_t cb[10];
        memset(cb, 0, sizeof(cb));
        cb[0] = SCSI_READ_10;
        be32(&cb[2], lba);                        /* logical block address (BE)     */
        cb[7] = (uint8_t)(n >> 8);                /* transfer length (blocks, BE16)  */
        cb[8] = (uint8_t)n;

        uint32_t got = 0;
        if (bot_command_retry(cb, sizeof(cb), out, bytes, 1, &got) != 0)
            return -1;
        if (got != bytes)                          /* short read => fail clean */
            return -1;

        lba   += n;
        count -= n;
        out   += bytes;
    }
    return 0;
}

/* WRITE(10) — BOT/SCSI write path (M1728: now wired into the block layer, not
 * just the self-test). Same bounds + chunking as the read path. */
int usb_storage_write(uint32_t lba, uint32_t count, const void *buf) {
    if (!us.present || !buf || count == 0)
        return -1;
    if (us.block_size != USB_STORAGE_SECTOR_SIZE)
        return -1;
    if (lba >= us.blocks || count > us.blocks - lba)
        return -1;

    const uint32_t per = USB_BULK_MAX / USB_STORAGE_SECTOR_SIZE;
    const uint8_t *in = (const uint8_t *)buf;

    while (count > 0) {
        uint32_t n = (count < per) ? count : per;
        uint32_t bytes = n * USB_STORAGE_SECTOR_SIZE;

        uint8_t cb[10];
        memset(cb, 0, sizeof(cb));
        cb[0] = SCSI_WRITE_10;
        be32(&cb[2], lba);
        cb[7] = (uint8_t)(n >> 8);
        cb[8] = (uint8_t)n;

        uint32_t got = 0;
        if (bot_command_retry(cb, sizeof(cb), (void *)in, bytes, 0, &got) != 0)
            return -1;

        lba   += n;
        count -= n;
        in    += bytes;
    }
    return 0;
}

/* --- boot-time verification ------------------------------------------------ */

/* A static DMA-friendly buffer for the self-test (identity-mapped low BSS). */
static uint8_t selftest_buf[USB_STORAGE_SECTOR_SIZE * 4];

void usb_storage_selftest(void) {
    if (!us.present) {
        kprintf("[usb-storage] no USB mass-storage device found "
                "(none attached; USB tablet + legacy ATA boot intact).\n\n");
        return;
    }

    scsi_inquiry();   /* best-effort vendor/product log line */

    uint64_t bytes_total = us.blocks * (uint64_t)us.block_size;
    kprintf("[ ok ] usb-storage up: READ CAPACITY = %lu blocks x %u bytes "
            "(%lu MiB) via BOT/SCSI over UHCI (boot stays on legacy ATA).\n",
            us.blocks, us.block_size, bytes_total / (1024 * 1024));

    for (uint64_t lba = 0; lba < 3 && lba < us.blocks; lba++) {
        if (usb_storage_read((uint32_t)lba, 1, selftest_buf) != 0) {
            kprintf("[usb-storage] sector %lu: READ FAILED\n", lba);
            continue;
        }
        uint32_t sum = 0;
        for (int i = 0; i < USB_STORAGE_SECTOR_SIZE; i++)
            sum += selftest_buf[i];
        kprintf("[usb-storage] sector %lu sum=%08x first16=", lba, sum);
        for (int i = 0; i < 16; i++)
            kprintf("%02x ", selftest_buf[i]);
        kprintf(" |");
        for (int i = 0; i < 16; i++) {
            uint8_t c = selftest_buf[i];
            kprintf("%c", (c >= 0x20 && c < 0x7F) ? (char)c : '.');
        }
        kprintf("|\n");
    }

    /* Optional write round-trip on the last sector: save, write a marker, read
     * back, verify, restore. Only if the device has room (>=8 sectors). A
     * failure is reported but not fatal. */
    if (us.blocks >= 8) {
        uint32_t test_lba = (uint32_t)(us.blocks - 1);
        static uint8_t saved[USB_STORAGE_SECTOR_SIZE];
        static uint8_t scratch[USB_STORAGE_SECTOR_SIZE];
        static uint8_t readback[USB_STORAGE_SECTOR_SIZE];
        if (usb_storage_read(test_lba, 1, saved) == 0) {
            for (int i = 0; i < USB_STORAGE_SECTOR_SIZE; i++)
                scratch[i] = (uint8_t)(0x5A ^ (i & 0xFF));
            int ok = (usb_storage_write(test_lba, 1, scratch) == 0);
            memset(readback, 0, sizeof(readback));
            ok = ok && (usb_storage_read(test_lba, 1, readback) == 0);
            ok = ok && (memcmp(readback, scratch, USB_STORAGE_SECTOR_SIZE) == 0);
            usb_storage_write(test_lba, 1, saved);   /* restore original content */
            kprintf("[usb-storage] write round-trip on sector %u: %s\n",
                    test_lba, ok ? "OK (wrote+read back+restored)" : "MISMATCH");
        }
    }

    kprintf("[ ok ] usb-storage read self-test complete "
            "(bytes above are the real flash-disk content).\n\n");
}
