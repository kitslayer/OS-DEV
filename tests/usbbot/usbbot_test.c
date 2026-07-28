/* usbbot_test.c — host-side regression for the shared USB Mass-Storage
 * Bulk-Only-Transport + SCSI layer (kernel/usbbot.h). Pure, built for the host
 * under ASan+UBSan. Exit 0 = pass. Keep in sync with kernel/usbbot.h.
 *
 * The fixture is a MOCK BOT DEVICE: a little state machine that consumes a
 * 31-byte CBW on OUT, serves/absorbs the data phase, and emits a 13-byte CSW on
 * IN — i.e. the other end of the wire, implemented independently of the code
 * under test. It backs a small RAM disk so READ(10)/WRITE(10) can be checked for
 * real content, and it can inject every fault a real device can produce: a bad
 * CSW signature, a mismatched tag, a non-zero status, a truncated CSW, a short
 * data phase, a non-zero residue, an over-reporting transport, and a hard
 * transport error on any chosen transfer.
 *
 * Coverage: exact on-wire CBW bytes, tag monotonicity + echo checking, the
 * IN/OUT residue-vs-transport trust rules, READ/WRITE(10) round-trips, chunking
 * across the max_data boundary, capacity bounds (including 32-bit LBA wrap),
 * non-512 block sizes, argument validation, and the single retry.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "usbbot.h"

static int fails = 0, checks = 0;
#define OK(cond) do { checks++; if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)
#define EQ(got, want) do { checks++; if ((got) != (want)) { \
    printf("FAIL line %d: %s == %d, wanted %d\n", __LINE__, #got, (int)(got), (int)(want)); fails++; } } while (0)

#define MOCK_BLOCKS 64

typedef struct {
    uint8_t  disk[MOCK_BLOCKS * 512];
    uint32_t nblocks, blk_size;

    /* BOT phase state: 0 = expect CBW, 1 = data phase, 2 = expect CSW read. */
    int      phase;
    uint32_t tag, data_len;
    int      data_in;
    uint8_t  cb[16];
    int      cb_len;
    uint32_t residue;                 /* what we will report in the CSW        */

    /* --- fault injection ------------------------------------------------- */
    int      f_bad_sig;               /* corrupt the CSW signature            */
    int      f_bad_tag;               /* echo the wrong tag                   */
    int      f_status;                /* CSW status byte to report            */
    int      f_short_csw;             /* return < 13 bytes for the CSW        */
    int      f_short_data;            /* return this many fewer data bytes    */
    int      f_over_report;           /* claim to have moved more than asked  */
    int      f_resid_extra;           /* add this to the reported residue     */
    int      f_fail_call;             /* return -1 on this xfer call (1-based)*/
    int      f_fail_once;             /* clear f_fail_call after it fires     */

    int      f_stall_until_reset;     /* fail every xfer until recovery runs  */

    /* --- observability ----------------------------------------------------*/
    int      resets;                  /* reset_recovery invocations           */
    int      calls;                   /* xfer call count                      */
    uint8_t  last_cbw[31];            /* the last CBW we received, verbatim   */
    int      commands;                /* completed commands                   */
} mockdev;

static void mock_reset(mockdev *m) {
    memset(m, 0, sizeof *m);
    m->nblocks  = MOCK_BLOCKS;
    m->blk_size = 512;
    /* Fill the RAM disk with a position-dependent pattern so a mis-addressed
     * read is detectable, not just a zero/non-zero check. */
    for (size_t i = 0; i < sizeof m->disk; i++)
        m->disk[i] = (uint8_t)((i * 31u + (i >> 9) * 7u) & 0xFF);
}

/* The mock transport: one bulk transfer, IN or OUT. */
static int mock_xfer(void *ctx, int in, void *buf, int len) {
    mockdev *m = (mockdev *)ctx;
    uint8_t *p = (uint8_t *)buf;
    m->calls++;
    if (m->f_fail_call && m->calls == m->f_fail_call) {
        if (m->f_fail_once) m->f_fail_call = 0;
        return -1;
    }
    /* A stalled endpoint: every transfer fails, and the phase state is left
     * wherever it was, until the host performs BOT reset recovery. */
    if (m->f_stall_until_reset) return -1;

    if (m->phase == 0) {                       /* --- expect the CBW on OUT --- */
        if (in || len != 31) return -1;
        memcpy(m->last_cbw, p, 31);
        if (usb_bot_get32le(p + 0) != USB_BOT_CBW_SIG) return -1;
        m->tag      = usb_bot_get32le(p + 4);
        m->data_len = usb_bot_get32le(p + 8);
        m->data_in  = (p[12] & USB_BOT_CBW_IN) ? 1 : 0;
        m->cb_len   = p[14];
        if (m->cb_len < 1 || m->cb_len > 16) return -1;
        memcpy(m->cb, p + 15, 16);
        m->residue  = m->data_len;             /* nothing transferred yet */
        m->phase    = m->data_len ? 1 : 2;
        return 31;
    }

    if (m->phase == 1) {                       /* --- the data phase --- */
        if (in != m->data_in) return -1;
        if ((uint32_t)len != m->data_len) return -1;
        int n = len - m->f_short_data;
        if (n < 0) n = 0;

        uint8_t op = m->cb[0];
        if (in) {
            memset(p, 0, (size_t)len);
            if (op == USB_SCSI_READ_CAPACITY10) {
                usb_bot_put32be(p + 0, m->nblocks - 1);      /* last LBA */
                usb_bot_put32be(p + 4, m->blk_size);
            } else if (op == USB_SCSI_INQUIRY) {
                if (len >= 36) { p[0] = 0x00; memcpy(p + 8, "OSDEV   MOCK DISK      ", 23); }
            } else if (op == USB_SCSI_READ_10) {
                uint32_t lba = usb_bot_get32be(&m->cb[2]);
                uint32_t cnt = ((uint32_t)m->cb[7] << 8) | m->cb[8];
                if (lba + cnt > m->nblocks) return -1;
                if (cnt * 512u != (uint32_t)len) return -1;
                memcpy(p, m->disk + (size_t)lba * 512, (size_t)n);
            }
        } else {
            if (op == USB_SCSI_WRITE_10) {
                uint32_t lba = usb_bot_get32be(&m->cb[2]);
                uint32_t cnt = ((uint32_t)m->cb[7] << 8) | m->cb[8];
                if (lba + cnt > m->nblocks) return -1;
                if (cnt * 512u != (uint32_t)len) return -1;
                memcpy(m->disk + (size_t)lba * 512, p, (size_t)n);
            }
        }
        m->residue = m->data_len - (uint32_t)n;
        m->phase   = 2;
        return m->f_over_report ? len + m->f_over_report : n;
    }

    /* --- phase 2: the host reads the CSW --- */
    if (!in || len != 13) return -1;
    memset(p, 0, 13);
    usb_bot_put32le(p + 0, m->f_bad_sig ? 0xDEADBEEFu : USB_BOT_CSW_SIG);
    usb_bot_put32le(p + 4, m->f_bad_tag ? m->tag + 1 : m->tag);
    usb_bot_put32le(p + 8, m->residue + (uint32_t)m->f_resid_extra);
    p[12] = (uint8_t)m->f_status;
    m->phase = 0;
    m->commands++;
    return m->f_short_csw ? 12 : 13;
}

/* Stands in for Bulk-Only Mass Storage Reset + Clear Feature ENDPOINT_HALT:
 * clears the stall and resynchronises the transport back to expecting a CBW,
 * exactly as a real device does after the spec's recovery sequence. */
static int mock_reset_recovery(void *ctx) {
    mockdev *m = (mockdev *)ctx;
    m->resets++;
    m->f_stall_until_reset = 0;
    m->phase = 0;                     /* device is back to expecting a CBW */
    return 0;
}

static void bot_bind(usb_bot_t *b, mockdev *m, uint32_t max_data) {
    memset(b, 0, sizeof *b);
    b->xfer           = mock_xfer;
    b->reset_recovery = mock_reset_recovery;
    b->ctx            = m;
    b->max_data = max_data;
    b->lun      = 0;
}

int main(void) {
    mockdev m;
    usb_bot_t b;

    /* --- READ CAPACITY fills the geometry --------------------------------- */
    mock_reset(&m); bot_bind(&b, &m, 4096);
    OK(usb_bot_read_capacity(&b) == 0);
    OK(b.blocks == MOCK_BLOCKS);
    OK(b.block_size == 512);

    /* --- the exact on-wire CBW bytes -------------------------------------- */
    {
        mock_reset(&m); bot_bind(&b, &m, 4096);
        b.lun = 3;
        uint8_t sec[512];
        b.blocks = MOCK_BLOCKS; b.block_size = 512;
        OK(usb_bot_read10(&b, 7, 1, sec) == 0);
        const uint8_t *c = m.last_cbw;
        OK(usb_bot_get32le(c + 0) == USB_BOT_CBW_SIG);   /* 'USBC'            */
        OK(usb_bot_get32le(c + 8) == 512);               /* declared length   */
        OK(c[12] == USB_BOT_CBW_IN);                     /* IN for a read     */
        OK(c[13] == 3);                                  /* LUN honoured      */
        OK(c[14] == 10);                                 /* READ(10) CDB len  */
        OK(c[15] == USB_SCSI_READ_10);
        OK(usb_bot_get32be(c + 17) == 7);                /* LBA (BE32)        */
        OK(c[22] == 0 && c[23] == 1);                    /* 1 block (BE16)    */
        /* the CBW is zero-padded out to 31 bytes */
        for (int i = 15 + 10; i < 31; i++) OK(c[i] == 0);
    }

    /* --- tags are monotonic and echo-checked ------------------------------ */
    {
        mock_reset(&m); bot_bind(&b, &m, 4096);
        OK(usb_bot_read_capacity(&b) == 0);
        uint32_t t1 = usb_bot_get32le(m.last_cbw + 4);
        OK(usb_bot_read_capacity(&b) == 0);
        uint32_t t2 = usb_bot_get32le(m.last_cbw + 4);
        OK(t2 == t1 + 1);

        m.f_bad_tag = 1;                       /* device echoes the wrong tag */
        OK(usb_bot_read_capacity(&b) != 0);
    }

    /* --- READ(10): real content, single and multi-block -------------------- */
    {
        mock_reset(&m); bot_bind(&b, &m, 4096);
        OK(usb_bot_read_capacity(&b) == 0);
        uint8_t buf[8 * 512];
        OK(usb_bot_read10(&b, 0, 1, buf) == 0);
        OK(memcmp(buf, m.disk, 512) == 0);
        OK(usb_bot_read10(&b, 5, 8, buf) == 0);
        OK(memcmp(buf, m.disk + 5 * 512, 8 * 512) == 0);
    }

    /* --- chunking: a transfer larger than max_data becomes several commands - */
    {
        mock_reset(&m); bot_bind(&b, &m, 2048);   /* 4 blocks per data phase */
        OK(usb_bot_read_capacity(&b) == 0);
        OK(usb_bot_blocks_per_xfer(&b) == 4);
        int before = m.commands;
        uint8_t buf[10 * 512];
        OK(usb_bot_read10(&b, 2, 10, buf) == 0);
        OK(memcmp(buf, m.disk + 2 * 512, 10 * 512) == 0);
        OK(m.commands - before == 3);             /* 4 + 4 + 2 */
    }

    /* --- WRITE(10) round-trip, including across a chunk boundary ----------- */
    {
        mock_reset(&m); bot_bind(&b, &m, 2048);
        OK(usb_bot_read_capacity(&b) == 0);
        uint8_t out[6 * 512], back[6 * 512];
        for (size_t i = 0; i < sizeof out; i++) out[i] = (uint8_t)(0xA5 ^ (i * 13));
        OK(usb_bot_write10(&b, 9, 6, out) == 0);
        OK(memcmp(m.disk + 9 * 512, out, sizeof out) == 0);   /* landed on the disk */
        OK(usb_bot_read10(&b, 9, 6, back) == 0);
        OK(memcmp(back, out, sizeof out) == 0);               /* and reads back */
        /* untouched neighbours stayed untouched */
        OK(m.disk[8 * 512] == (uint8_t)((8 * 512) * 31u + 8u * 7u));
    }

    /* --- capacity bounds, including the 32-bit LBA wrap guard -------------- */
    {
        mock_reset(&m); bot_bind(&b, &m, 4096);
        OK(usb_bot_read_capacity(&b) == 0);
        uint8_t buf[512];
        OK(usb_bot_read10(&b, MOCK_BLOCKS, 1, buf) != 0);        /* past the end     */
        OK(usb_bot_read10(&b, MOCK_BLOCKS - 1, 2, buf) != 0);    /* straddles it     */
        OK(usb_bot_read10(&b, MOCK_BLOCKS - 1, 1, buf) == 0);    /* the last block   */
        OK(usb_bot_read10(&b, 0xFFFFFFF0u, 0x20, buf) != 0);     /* lba+count wraps  */
        OK(usb_bot_write10(&b, MOCK_BLOCKS, 1, buf) != 0);
        OK(usb_bot_read10(&b, 0, 0, buf) != 0);                  /* zero count       */
        OK(usb_bot_read10(&b, 0, 1, 0) != 0);                    /* NULL buffer      */
    }

    /* --- a non-512 block size is refused (the block layer assumes 512) ----- */
    {
        mock_reset(&m); m.blk_size = 4096; bot_bind(&b, &m, 4096);
        OK(usb_bot_read_capacity(&b) == 0);
        OK(b.block_size == 4096);
        uint8_t buf[512];
        OK(usb_bot_read10(&b, 0, 1, buf) != 0);
        OK(usb_bot_write10(&b, 0, 1, buf) != 0);
    }
    {   /* a zero block size is a malformed READ CAPACITY, not a divide-by-zero */
        mock_reset(&m); m.blk_size = 0; bot_bind(&b, &m, 4096);
        OK(usb_bot_read_capacity(&b) != 0);
    }

    /* --- CSW faults each fail clean ---------------------------------------- */
    {
        uint8_t buf[512];
        mock_reset(&m); bot_bind(&b, &m, 4096); m.f_bad_sig = 1;
        OK(usb_bot_read_capacity(&b) != 0);

        mock_reset(&m); bot_bind(&b, &m, 4096); m.f_status = 1;   /* command failed */
        OK(usb_bot_read_capacity(&b) != 0);

        mock_reset(&m); bot_bind(&b, &m, 4096); m.f_status = 2;   /* phase error */
        OK(usb_bot_read_capacity(&b) != 0);

        mock_reset(&m); bot_bind(&b, &m, 4096); m.f_short_csw = 1;
        OK(usb_bot_read_capacity(&b) != 0);

        /* a short data phase must fail the READ, not silently return junk */
        mock_reset(&m); bot_bind(&b, &m, 4096);
        OK(usb_bot_read_capacity(&b) == 0);
        m.f_short_data = 16;
        OK(usb_bot_read10(&b, 0, 1, buf) != 0);
    }

    /* --- residue semantics -------------------------------------------------- */
    {
        mock_reset(&m); bot_bind(&b, &m, 4096);
        OK(usb_bot_read_capacity(&b) == 0);

        /* IN: a device claiming (via residue) that it sent everything cannot
         * override a transport that only delivered part of it. */
        uint8_t buf[512];
        uint32_t got = 0;
        m.f_short_data = 100;                 /* transport moved 412 */
        m.f_resid_extra = -100;               /* but residue claims all 512 */
        uint8_t cb[10] = { USB_SCSI_READ_10, 0,0,0,0,0, 0, 0, 1, 0 };
        OK(usb_bot_command(&b, cb, 10, buf, 512, 1, &got) == 0);
        OK(got == 412);                       /* min(moved, by_residue) */

        /* IN: a transport over-reporting is clamped to the declared length. */
        mock_reset(&m); bot_bind(&b, &m, 4096);
        OK(usb_bot_read_capacity(&b) == 0);
        m.f_over_report = 64;
        got = 0;
        OK(usb_bot_command(&b, cb, 10, buf, 512, 1, &got) == 0);
        OK(got == 512);                       /* never more than we asked for */

        /* OUT: the device's residue is authoritative — a quiet short write is
         * reported, and so fails the caller's whole-transfer check. */
        mock_reset(&m); bot_bind(&b, &m, 4096);
        OK(usb_bot_read_capacity(&b) == 0);
        m.f_resid_extra = 128;                /* device accepted 128 fewer bytes */
        OK(usb_bot_write10(&b, 0, 1, buf) != 0);
    }

    /* --- argument validation ------------------------------------------------ */
    {
        mock_reset(&m); bot_bind(&b, &m, 4096);
        uint8_t buf[512], cb[10] = { USB_SCSI_READ_10, 0,0,0,0,0, 0, 0, 1, 0 };
        OK(usb_bot_command(&b, cb,  0, buf, 512, 1, 0) != 0);   /* cb_len < 1   */
        OK(usb_bot_command(&b, cb, 17, buf, 512, 1, 0) != 0);   /* cb_len > 16  */
        OK(usb_bot_command(&b, cb, 10,   0, 512, 1, 0) != 0);   /* NULL data    */
        OK(usb_bot_command(&b, cb, 10, buf, 8192, 1, 0) != 0);  /* > max_data   */
        OK(m.calls == 0);                                       /* nothing hit the wire */

        usb_bot_t nb; memset(&nb, 0, sizeof nb);
        OK(usb_bot_command(&nb, cb, 10, buf, 512, 1, 0) != 0);  /* no transport */
    }

    /* --- the single retry --------------------------------------------------- */
    {
        /* First attempt dies on its very first transfer; the retry succeeds. */
        mock_reset(&m); bot_bind(&b, &m, 4096);
        m.f_fail_call = 1; m.f_fail_once = 1;
        OK(usb_bot_read_capacity(&b) == 0);
        OK(m.calls > 1);

        /* A persistently failing device still fails (retry is not a loop). */
        mock_reset(&m); bot_bind(&b, &m, 4096);
        m.f_status = 1;
        int before = m.commands;
        OK(usb_bot_read_capacity(&b) != 0);
        OK(m.commands - before == 2);          /* tried exactly twice */
    }

    /* --- BOT reset recovery after a stall (M1898) ---------------------------
     * A stalled bulk endpoint leaves the device and host disagreeing about the
     * transport phase AND the data toggle, so a bare retry fails the same way.
     * The spec's recovery sequence is what resynchronises them. */
    {
        /* A stall that persists until recovery runs: the retry path must invoke
         * recovery and then succeed. Without the hook this command is unfixable. */
        mock_reset(&m); bot_bind(&b, &m, 4096);
        m.f_stall_until_reset = 1;
        OK(usb_bot_read_capacity(&b) == 0);      /* succeeds *because* of recovery */
        EQ(m.resets, 1);

        /* A stall left mid-transfer desynchronises the phase; recovery resets it
         * so the following CBW is understood. */
        mock_reset(&m); bot_bind(&b, &m, 4096);
        OK(usb_bot_read_capacity(&b) == 0);       /* get geometry first */
        m.f_stall_until_reset = 1;
        uint8_t sec[512];
        OK(usb_bot_read10(&b, 0, 1, sec) == 0);
        OK(m.resets >= 1);

        /* Recovery is only attempted when something actually failed. */
        mock_reset(&m); bot_bind(&b, &m, 4096);
        OK(usb_bot_read_capacity(&b) == 0);
        EQ(m.resets, 0);

        /* A transport with NO recovery hook still behaves exactly as before: one
         * retry, no crash, and a persistent failure still reports failure. */
        mock_reset(&m); bot_bind(&b, &m, 4096);
        b.reset_recovery = 0;
        m.f_stall_until_reset = 1;
        OK(usb_bot_read_capacity(&b) != 0);
        EQ(m.resets, 0);

        /* Recovery that itself fails must not wedge or loop: the command simply
         * reports failure after its single retry. */
        mock_reset(&m); bot_bind(&b, &m, 4096);
        m.f_stall_until_reset = 1;
        m.f_status = 1;                            /* stays broken after recovery */
        OK(usb_bot_read_capacity(&b) != 0);
    }

    /* --- INQUIRY + TEST UNIT READY ------------------------------------------ */
    {
        mock_reset(&m); bot_bind(&b, &m, 4096);
        OK(usb_bot_test_unit_ready(&b) == 0);
        uint8_t inq[36];
        OK(usb_bot_inquiry(&b, inq) == 36);
        OK(memcmp(inq + 8, "OSDEV   ", 8) == 0);
    }

    /* --- packet accounting for a per-endpoint data toggle (M1891) ------------
     * A controller with a data toggle (UHCI/EHCI) must advance it once per
     * packet, counted from the bytes ACTUALLY moved. The bug this guards against
     * counted the bytes REQUESTED, which only shows up when the two counts differ
     * in parity — so the cases below pair "harmless shortfall" against "toggle
     * flips the wrong way". */
    {
        /* exact multiples and partials of maxp */
        OK(usb_bot_packets(0,    512) == 1);   /* zero-length transfer is one ZLP */
        OK(usb_bot_packets(1,    512) == 1);
        OK(usb_bot_packets(511,  512) == 1);
        OK(usb_bot_packets(512,  512) == 1);
        OK(usb_bot_packets(513,  512) == 2);
        OK(usb_bot_packets(1024, 512) == 2);
        OK(usb_bot_packets(1536, 512) == 3);
        OK(usb_bot_packets(31,    64) == 1);   /* a 31-byte CBW at full-speed maxp */
        OK(usb_bot_packets(13,    64) == 1);   /* a 13-byte CSW */

        /* The shortfall that is HARMLESS: 412 of a requested 512 is still one
         * packet, so the toggle lands in the same place either way. */
        OK(usb_bot_packets(412, 512) == usb_bot_packets(512, 512));

        /* The shortfall that BITES: 1024 delivered of a requested 1536 is 2
         * packets vs 3 — opposite parity, so a toggle advanced by the requested
         * count desynchronises from the device. */
        OK(usb_bot_packets(1024, 512) != usb_bot_packets(1536, 512));
        OK((usb_bot_packets(1024, 512) & 1) != (usb_bot_packets(1536, 512) & 1));

        /* A short transfer never counts MORE packets than the full one. */
        for (int maxp = 8; maxp <= 512; maxp *= 2)
            for (int req = 0; req <= 4096; req += 37)
                for (int got = 0; got <= req; got += 53)
                    OK(usb_bot_packets(got, maxp) <= usb_bot_packets(req, maxp));

        /* Defensive: a bad maxp is reported as 0 rather than dividing by zero. */
        OK(usb_bot_packets(512, 0)  == 0);
        OK(usb_bot_packets(512, -1) == 0);
    }

    /* --- blocks-per-transfer is capped by the CDB's 16-bit count field ------- */
    {
        mock_reset(&m); bot_bind(&b, &m, 0xFFFFFFFFu);
        OK(usb_bot_blocks_per_xfer(&b) == 0xFFFFu);
        bot_bind(&b, &m, 512);
        OK(usb_bot_blocks_per_xfer(&b) == 1);
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
