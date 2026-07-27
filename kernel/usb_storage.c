/*
 * usb_storage.c — USB mass-storage over the UHCI host controller (kernel/usb.c).
 * Drives a USB flash disk: enumerate it, then read/write sectors with SCSI
 * READ(10)/WRITE(10) carried inside USB Bulk-Only Transport.
 *
 * Since M1889 the BOT/SCSI protocol itself is NOT here — it lives once, shared
 * by every host controller (UHCI here, EHCI, xHCI), in kernel/usbbot.h, which is
 * pure and host-tested (tests/usbbot, ASan/UBSan) against a mock device. What
 * remains in this file is the two halves a transport must supply:
 *
 *   - ENUMERATION (above): walk the UHCI root ports, address the device, find a
 *     Mass-Storage / SCSI-transparent / BOT interface (class 0x08, subclass
 *     0x06, protocol 0x50) and its BULK IN + OUT endpoints.
 *   - THE TRANSPORT (uhci_bot_xfer): move bytes on those two endpoints, keeping
 *     the per-endpoint data toggles BOT requires.
 *
 * --- Safety -----------------------------------------------------------------
 *  - Capacity bounds (lba+count inside the device, 32-bit wrap guarded), the
 *    512-byte block-size requirement, data-phase chunking, and the CBW/CSW
 *    signature + tag + status + residue checks are all enforced by usbbot.h.
 *  - Every USB transfer (control + bulk) has a finite, bounded timeout in
 *    kernel/usb.c; a stalled/absent endpoint returns -1 cleanly.
 *  - DMA buffers come from pmm frames (identity-mapped: phys == virt); the bulk
 *    bounce buffer in usb.c carries the data phase, so the device DMAs into a
 *    physically-addressed frame, then usb.c memcpys into the caller buffer
 *    bounded by the bytes actually transferred.
 *  - No mass-storage interface found => usb_storage_init() returns -1 (clean
 *    no-op); the existing USB tablet path is unaffected.
 */
#include "usb_storage.h"
#include "usb.h"
#include "usbbot.h"    /* the shared BOT/SCSI protocol layer (M1889) */
#include "console.h"
#include "string.h"
#include "timer.h"
#include <stdint.h>

/* Driver state for the one USB mass-storage device we support. Everything above
 * the two bulk endpoints — the CBW/CSW state machine, the SCSI commands, the
 * capacity bounds and the chunking — lives in the shared usbbot.h layer; this
 * driver supplies only the UHCI transport below and the enumeration above. */
static struct {
    int      present;
    uint8_t  addr;        /* USB device address                               */
    uint8_t  ep_in;       /* BULK IN endpoint number                          */
    uint8_t  ep_out;      /* BULK OUT endpoint number                         */
    uint16_t maxp_in;     /* BULK IN max packet                               */
    uint16_t maxp_out;    /* BULK OUT max packet                              */
    int      tog_in;      /* BULK IN data toggle (persists across transfers)  */
    int      tog_out;     /* BULK OUT data toggle                             */
    usb_bot_t bot;        /* the shared BOT/SCSI engine + device geometry     */
} us;

/* --- the UHCI transport for the shared BOT layer ---------------------------
 * usb_bulk_xfer already copies to/from the caller's buffer through its own
 * bounce frame and reports the byte count actually moved (so a short IN stays
 * visible), which is exactly the contract usbbot.h asks for. The per-endpoint
 * data toggles persist in `us` across transfers, as BOT requires. */
static int uhci_bot_xfer(void *ctx, int in, void *buf, int len) {
    (void)ctx;
    int act = 0;
    if (usb_bulk_xfer(us.addr, in ? us.ep_in : us.ep_out,
                      in ? us.maxp_in : us.maxp_out,
                      in ? &us.tog_in : &us.tog_out,
                      buf, len, in, &act) != 0)
        return -1;
    return act;
}
static void uhci_bot_delay(int ms) { timer_wait(ms); }

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

    /* Bind the shared BOT engine to this device over the UHCI transport. */
    us.bot.xfer     = uhci_bot_xfer;
    us.bot.delay_ms = uhci_bot_delay;
    us.bot.ctx      = 0;
    us.bot.max_data = USB_BULK_MAX;      /* one bulk data phase on UHCI */
    us.bot.lun      = 0;

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
    uint8_t inq[36];
    int got = usb_bot_inquiry(&us.bot, inq);
    if (got >= 32) {
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
    int ok = -1;
    for (int p = 0; p < usb_uhci_port_count() && ok != 0; p++) {
        if (usb_uhci_port_claimed(p))
            continue;                             /* the tablet (or another driver) owns it */
        if (!usb_uhci_enable_port(p))
            continue;                             /* nothing connected/enabled */
        if (enumerate_one() == 0) {
            usb_uhci_claim_port(p);               /* ours now — later probes must not reset it */
            ok = 0;
        }
    }
    if (ok != 0) {
        /* No mass-storage interface on the bus — clean no-op (tablet unaffected). */
        return -1;
    }

    /* Some devices need a TEST UNIT READY poke before they answer commands;
     * issue it (ignore failure) so READ CAPACITY is more likely to succeed. */
    (void)usb_bot_test_unit_ready(&us.bot);

    if (usb_bot_read_capacity(&us.bot) != 0) {
        kprintf("[usb-storage] READ CAPACITY failed\n");
        return -1;
    }

    us.present = 1;
    return 0;
}

int      usb_storage_present(void)  { return us.present; }
uint64_t usb_storage_capacity(void) { return us.present ? us.bot.blocks : 0; }

int usb_storage_read(uint32_t lba, uint32_t count, void *buf) {
    if (!us.present) return -1;
    return usb_bot_read10(&us.bot, lba, count, buf);
}

/* WRITE(10) — BOT/SCSI write path (M1728: wired into the block layer, not just
 * the self-test). Bounds + chunking live in the shared layer. */
int usb_storage_write(uint32_t lba, uint32_t count, const void *buf) {
    if (!us.present) return -1;
    return usb_bot_write10(&us.bot, lba, count, buf);
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

    uint64_t bytes_total = us.bot.blocks * (uint64_t)us.bot.block_size;
    kprintf("[ ok ] usb-storage up: READ CAPACITY = %lu blocks x %u bytes "
            "(%lu MiB) via BOT/SCSI over UHCI (boot stays on legacy ATA).\n",
            us.bot.blocks, us.bot.block_size, bytes_total / (1024 * 1024));

    for (uint64_t lba = 0; lba < 3 && lba < us.bot.blocks; lba++) {
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
    if (us.bot.blocks >= 8) {
        uint32_t test_lba = (uint32_t)(us.bot.blocks - 1);
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
