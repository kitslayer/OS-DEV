/* usbbot.h — USB Mass-Storage Bulk-Only Transport (BOT) + minimal SCSI (M1889).
 *
 * ONE implementation of the BOT/SCSI protocol, shared by every USB host
 * controller. Before this header the exact same state machine existed three
 * times — kernel/usb_storage.c (UHCI), kernel/ehci.c and kernel/xhci.c — and
 * only the UHCI copy was ever wired into the block layer; the EHCI/xHCI copies
 * were read-only, LBA-0-only, and lived inside their drivers' self-tests. Three
 * hand-copied transcriptions of a protocol with tags, toggles and residues is
 * exactly the thing that rots, so the protocol lives here and the drivers supply
 * only their transport.
 *
 * Pure and self-contained (only <stdint.h>/<stddef.h>, no allocation, no I/O),
 * so the kernel and the off-target unit test (tests/usbbot, ASan/UBSan) share
 * the exact same code. The caller owns every buffer.
 *
 * --- The protocol, briefly --------------------------------------------------
 * A Mass-Storage / SCSI-transparent / Bulk-Only-Transport device (class 0x08,
 * subclass 0x06, protocol 0x50) speaks SCSI over two bulk endpoints. Each
 * command is three phases on those endpoints:
 *
 *   1. COMMAND — a 31-byte Command Block Wrapper (CBW) on BULK OUT:
 *        signature 'USBC' (0x43425355 LE), a tag we echo-check, the declared
 *        data-transfer length, a flags byte (0x80 = data is IN), the LUN, the
 *        SCSI command length, then the SCSI command bytes (padded to 16).
 *   2. DATA    — the data phase of the declared length, IN or OUT (may be absent).
 *   3. STATUS  — a 13-byte Command Status Wrapper (CSW) on BULK IN:
 *        signature 'USBS' (0x53425355 LE), the echoed tag, the residue (bytes
 *        NOT transferred), and a status byte (0 = passed, 1 = failed, 2 = phase
 *        error).
 *
 * --- The transport contract --------------------------------------------------
 * A host controller supplies one function:
 *
 *   int xfer(void *ctx, int in, void *buf, int len)
 *
 * which moves up to `len` bytes on the device's bulk IN (in=1) or bulk OUT
 * (in=0) endpoint, to/from `buf`, and returns the number of bytes ACTUALLY
 * moved (>= 0, so a short IN is visible to us) or -1 on stall/timeout/error.
 * Data toggles (UHCI/EHCI), TRB rings (xHCI) and any bounce-buffer copy are the
 * transport's business, not ours. `max_data` caps one data phase; the SCSI
 * helpers below chunk anything larger.
 *
 * --- Safety ------------------------------------------------------------------
 *  - Every field written into the CBW is bounded: cb_len is validated 1..16 and
 *    the command bytes are copied into a fixed 31-byte stack CBW.
 *  - The data phase is refused if it exceeds `max_data`, so a transport's bounce
 *    buffer can never be overrun; `*got` is clamped to the declared length, so a
 *    device claiming to have sent MORE than we asked for cannot make a caller
 *    read past its own buffer.
 *  - The CSW signature, the echoed tag and the status byte are all verified; the
 *    residue is cross-checked against the bytes the transport reported and the
 *    SMALLER of the two is reported, so neither a lying residue nor a lying
 *    transport can inflate the byte count.
 *  - read10/write10 bounds lba+count against the device capacity (with the
 *    32-bit add guarded against wrap) and refuse a non-512-byte block size.
 */
#ifndef USBBOT_H
#define USBBOT_H
#include <stdint.h>
#include <stddef.h>

#define USB_BOT_CBW_SIG   0x43425355u   /* 'USBC' little-endian */
#define USB_BOT_CSW_SIG   0x53425355u   /* 'USBS' little-endian */
#define USB_BOT_CBW_IN    0x80          /* CBW flags bit 7: data is device->host */
#define USB_BOT_CBW_LEN   31
#define USB_BOT_CSW_LEN   13
#define USB_BOT_SECTOR    512           /* the only block size the block layer takes */

/* SCSI opcodes we issue. */
#define USB_SCSI_TEST_UNIT_READY 0x00
#define USB_SCSI_INQUIRY         0x12
#define USB_SCSI_READ_CAPACITY10 0x25
#define USB_SCSI_READ_10         0x28
#define USB_SCSI_WRITE_10        0x2A

/* Move up to `len` bytes on the bulk IN (in=1) / OUT (in=0) endpoint to/from
 * `buf`. Returns bytes actually moved (>=0), or -1 on error. */
typedef int (*usb_bot_xfer_fn)(void *ctx, int in, void *buf, int len);

typedef struct {
    usb_bot_xfer_fn xfer;        /* transport (required)                        */
    void  (*delay_ms)(int ms);   /* optional inter-retry delay (may be NULL)    */
    void    *ctx;                /* opaque, passed to xfer                      */
    uint32_t max_data;           /* max bytes in ONE data phase (>= 512)        */
    uint8_t  lun;                /* logical unit (0..15)                        */
    uint32_t tag;                /* monotonic CBW tag; bumped per command       */
    /* Geometry, filled in by usb_bot_read_capacity(). */
    uint64_t blocks;             /* total logical blocks (last-LBA + 1)         */
    uint32_t block_size;         /* bytes per block (we require 512 for I/O)    */
} usb_bot_t;

/* --- little/big-endian field helpers (the CBW/CSW are LE, SCSI CDBs are BE) - */
static inline void usb_bot_put32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint32_t usb_bot_get32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void usb_bot_put32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static inline uint32_t usb_bot_get32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* Run one full BOT command: CBW (OUT) -> optional data phase -> CSW (IN).
 * `cb`/`cb_len` is the SCSI command (1..16 bytes). `data`/`data_len` is the data
 * buffer and the host-declared transfer length (0 for none); `data_in` selects
 * IN (device->host) vs OUT. On return *got (optional) holds the bytes actually
 * transferred. Returns 0 if the CSW reported success, -1 on any transport,
 * signature, tag or status fault. */
static inline int usb_bot_command(usb_bot_t *b, const uint8_t *cb, int cb_len,
                                  void *data, uint32_t data_len, int data_in,
                                  uint32_t *got) {
    if (got) *got = 0;
    if (!b || !b->xfer)                 return -1;
    if (cb_len < 1 || cb_len > 16)      return -1;
    if (data_len && !data)              return -1;
    if (data_len > b->max_data)         return -1;   /* caller must chunk */

    /* --- Phase 1: the 31-byte CBW on BULK OUT --- */
    uint8_t cbw[USB_BOT_CBW_LEN];
    for (int i = 0; i < USB_BOT_CBW_LEN; i++) cbw[i] = 0;
    uint32_t tag = ++b->tag;
    usb_bot_put32le(cbw + 0, USB_BOT_CBW_SIG);
    usb_bot_put32le(cbw + 4, tag);
    usb_bot_put32le(cbw + 8, data_len);
    cbw[12] = data_in ? USB_BOT_CBW_IN : 0;
    cbw[13] = (uint8_t)(b->lun & 0x0F);
    cbw[14] = (uint8_t)cb_len;
    for (int i = 0; i < cb_len; i++) cbw[15 + i] = cb[i];

    if (b->xfer(b->ctx, 0, cbw, USB_BOT_CBW_LEN) != USB_BOT_CBW_LEN)
        return -1;

    /* --- Phase 2: the data phase, of the host-declared length --- */
    uint32_t moved = 0;
    if (data_len) {
        int a = b->xfer(b->ctx, data_in ? 1 : 0, data, (int)data_len);
        if (a < 0)
            return -1;                                /* data-phase transport fault */
        moved = (uint32_t)a;
        if (moved > data_len) moved = data_len;       /* never exceed what we declared */
    }

    /* --- Phase 3: read + validate the 13-byte CSW on BULK IN --- */
    uint8_t csw[USB_BOT_CSW_LEN];
    for (int i = 0; i < USB_BOT_CSW_LEN; i++) csw[i] = 0;
    if (b->xfer(b->ctx, 1, csw, USB_BOT_CSW_LEN) != USB_BOT_CSW_LEN)
        return -1;                                    /* short/absent CSW => fail clean */
    if (usb_bot_get32le(csw + 0) != USB_BOT_CSW_SIG)  return -1;   /* bad signature */
    if (usb_bot_get32le(csw + 4) != tag)              return -1;   /* tag mismatch  */
    if (csw[12] != 0)                                 return -1;   /* failed / phase error */

    /* Residue is the bytes the device did NOT transfer, so (declared - residue)
     * is its own report of the byte count. Which side we trust depends on the
     * direction, and the two cases are genuinely different:
     *
     *   IN  — the TRANSPORT filled our buffer, so it bounds what is safe to
     *         read: report min(moved, by_residue). Believing a device that
     *         claims it sent more than the controller actually delivered would
     *         hand the caller uninitialised bytes.
     *   OUT — the DEVICE is the authority on how much it accepted; the
     *         transport's count is only a transmit tally (and controllers vary
     *         in how they report OUT actual-lengths). Report by_residue, so a
     *         device that quietly accepted less than we sent is caught by the
     *         caller's short-transfer check rather than silently losing data.
     */
    if (got) {
        uint32_t residue    = usb_bot_get32le(csw + 8);
        uint32_t by_residue = (residue <= data_len) ? (data_len - residue) : 0;
        if (data_in)
            *got = (moved < by_residue) ? moved : by_residue;
        else
            *got = by_residue;
    }
    return 0;
}

/* As usb_bot_command, but retry once (with the optional delay) — handles a
 * transient stall / CSW fault the way the UHCI driver always has. */
static inline int usb_bot_command_retry(usb_bot_t *b, const uint8_t *cb, int cb_len,
                                        void *data, uint32_t data_len, int data_in,
                                        uint32_t *got) {
    if (usb_bot_command(b, cb, cb_len, data, data_len, data_in, got) == 0)
        return 0;
    if (b && b->delay_ms) b->delay_ms(1);
    return usb_bot_command(b, cb, cb_len, data, data_len, data_in, got);
}

/* TEST UNIT READY — some devices need this poke before they answer commands.
 * Callers treat failure as non-fatal. */
static inline int usb_bot_test_unit_ready(usb_bot_t *b) {
    uint8_t cb[6] = { USB_SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    return usb_bot_command_retry(b, cb, (int)sizeof cb, 0, 0, 0, 0);
}

/* INQUIRY — standard inquiry data into `out` (36 bytes). Returns the bytes read
 * (>=0) or -1. Best-effort: some emulated devices are terse. */
static inline int usb_bot_inquiry(usb_bot_t *b, uint8_t out[36]) {
    uint8_t cb[6] = { USB_SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    for (int i = 0; i < 36; i++) out[i] = 0;
    uint32_t got = 0;
    if (usb_bot_command_retry(b, cb, (int)sizeof cb, out, 36, 1, &got) != 0)
        return -1;
    return (int)got;
}

/* READ CAPACITY(10) — fills b->blocks and b->block_size. Returns 0 on success. */
static inline int usb_bot_read_capacity(usb_bot_t *b) {
    uint8_t cb[10] = { USB_SCSI_READ_CAPACITY10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t cap[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    uint32_t got = 0;
    if (usb_bot_command_retry(b, cb, (int)sizeof cb, cap, (uint32_t)sizeof cap, 1, &got) != 0)
        return -1;
    if (got < 8)
        return -1;
    uint32_t last_lba = usb_bot_get32be(&cap[0]);
    uint32_t blk      = usb_bot_get32be(&cap[4]);
    if (blk == 0)
        return -1;
    b->block_size = blk;
    b->blocks     = (uint64_t)last_lba + 1;
    return 0;
}

/* How many max-packet-sized packets a bulk transfer of `moved` bytes consumed
 * (M1891). A controller that manages a per-endpoint DATA TOGGLE has to advance
 * it once per packet, and the count must come from the bytes ACTUALLY moved, not
 * the bytes requested: USB terminates a transfer early on a short packet, so a
 * short IN consumes fewer packets than were asked for. A zero-length transfer is
 * still one packet (the ZLP).
 *
 * Getting this wrong desynchronises the endpoint's toggle from the device's, and
 * every later transfer on that endpoint is then NAKed or mis-sequenced. Because
 * the toggle is a single bit, only a PARITY difference between the requested and
 * actual packet counts is visible — which is why the bug this replaced survived:
 * the common shortfall (fewer bytes inside the same packet count, e.g. 412 of a
 * requested 512 with maxp 512) is harmless, and QEMU's usb-storage always
 * delivers exactly what was asked. A real device that short-terminates across a
 * packet boundary (e.g. 1024 delivered of a requested 1536 at maxp 512: 2
 * packets vs 3) flips the toggle the wrong way.
 *
 * Returns 0 for a negative maxp (a caller error) so the caller can reject it. */
static inline int usb_bot_packets(int moved, int maxp) {
    if (maxp <= 0) return 0;
    if (moved <= 0) return 1;                       /* zero-length transfer = one ZLP */
    return (moved + maxp - 1) / maxp;
}

/* Blocks we can move in one data phase: max_data/512, capped at the CDB's
 * 16-bit transfer-length field. At least 1 whenever max_data >= 512. */
static inline uint32_t usb_bot_blocks_per_xfer(const usb_bot_t *b) {
    uint32_t per = b->max_data / USB_BOT_SECTOR;
    if (per > 0xFFFFu) per = 0xFFFFu;
    return per;
}

/* Shared READ(10)/WRITE(10) engine: bounds-check against the device capacity,
 * then issue chunked commands so each data phase fits max_data. `write` picks
 * the direction. Returns 0 on success, -1 on any fault (including a short read,
 * which fails clean rather than leaving the caller's buffer half-filled). */
static inline int usb_bot_rw10(usb_bot_t *b, int write, uint32_t lba,
                               uint32_t count, void *buf) {
    if (!b || !b->xfer || !buf || count == 0)          return -1;
    if (b->block_size != USB_BOT_SECTOR)               return -1;  /* 512-byte blocks only */
    /* lba+count must lie inside the device; the subtraction form guards the
     * 32-bit add against wrapping. */
    if (lba >= b->blocks || count > b->blocks - lba)   return -1;

    uint32_t per = usb_bot_blocks_per_xfer(b);
    if (per == 0) return -1;
    uint8_t *p = (uint8_t *)buf;

    while (count > 0) {
        uint32_t n     = (count < per) ? count : per;
        uint32_t bytes = n * (uint32_t)USB_BOT_SECTOR;

        uint8_t cb[10];
        for (int i = 0; i < 10; i++) cb[i] = 0;
        cb[0] = write ? USB_SCSI_WRITE_10 : USB_SCSI_READ_10;
        usb_bot_put32be(&cb[2], lba);          /* logical block address (BE32)      */
        cb[7] = (uint8_t)(n >> 8);             /* transfer length in blocks (BE16)  */
        cb[8] = (uint8_t)n;

        uint32_t got = 0;
        if (usb_bot_command_retry(b, cb, (int)sizeof cb, p, bytes, write ? 0 : 1, &got) != 0)
            return -1;
        if (got != bytes)                      /* short transfer => fail clean */
            return -1;

        lba   += n;
        count -= n;
        p     += bytes;
    }
    return 0;
}

static inline int usb_bot_read10(usb_bot_t *b, uint32_t lba, uint32_t count, void *buf) {
    return usb_bot_rw10(b, 0, lba, count, buf);
}
static inline int usb_bot_write10(usb_bot_t *b, uint32_t lba, uint32_t count, const void *buf) {
    return usb_bot_rw10(b, 1, lba, count, (void *)(uintptr_t)buf);
}

#endif /* USBBOT_H */
