/*
 * usb.c — a minimal UHCI driver that drives QEMU's usb-tablet.
 *
 * Why: a PS/2 mouse is *relative*, so its cursor can't match the host pointer
 * in a window. The usb-tablet is *absolute* — it reports an (x,y) in 0..32767 —
 * so the cursor tracks 1:1. To read it we need a USB host controller. UHCI
 * (USB 1.1) is the simplest: it's driven through I/O ports plus DMA data
 * structures in RAM.
 *
 * The model: a 1024-entry **frame list** the controller walks 1000x/sec; each
 * entry points at a **queue head** (QH); a QH points at a chain of **transfer
 * descriptors** (TDs), each describing one packet (SETUP/IN/OUT) with a buffer.
 * We use one QH for control transfers (enumeration) and one for the tablet's
 * interrupt-IN endpoint (the stream of HID reports). All these structures and
 * buffers live in identity-mapped low RAM so their physical == virtual address,
 * which is what the controller's DMA needs.
 */
#include "usb.h"
#include "pci.h"
#include "pmm.h"
#include "mouse.h"
#include "fb.h"
#include "console.h"
#include "string.h"
#include "timer.h"
#include "io.h"
#include <stdint.h>

/* UHCI I/O registers (offsets from the I/O base, BAR4). */
#define REG_CMD     0x00
#define REG_STS     0x02
#define REG_INTR    0x04
#define REG_FRNUM   0x06
#define REG_FRBASE  0x08
#define REG_SOF     0x0C
#define REG_PORTSC1 0x10
#define REG_PORTSC2 0x12

struct uhci_td {
    volatile uint32_t link;
    volatile uint32_t cs;       /* control + status */
    volatile uint32_t token;
    volatile uint32_t buffer;
    uint32_t reserved[4];       /* pad to 32 bytes */
} __attribute__((packed));

struct uhci_qh {
    volatile uint32_t head;     /* horizontal link (next QH) */
    volatile uint32_t element;  /* vertical link (first TD)   */
    uint32_t reserved[2];
} __attribute__((packed));

#define TD_ACTIVE   (1u << 23)
#define TD_ERRMASK  (0x7E0000u)     /* status error bits 17..22 */
#define PID_SETUP   0x2D
#define PID_IN      0x69
#define PID_OUT     0xE1

static uint16_t io;
static uint32_t *framelist;
static struct uhci_qh *qh_ctrl, *qh_int;
static struct uhci_td *tds;        /* control transfer TD pool */
static struct uhci_td *int_td;     /* interrupt endpoint TD     */
static uint8_t  *setup_buf, *data_buf, *report_buf;

static int dev_addr, ep_in, ep_maxp, int_toggle, ready;

static uint16_t rd(uint16_t o)            { return inw(io + o); }
static void     wr(uint16_t o, uint16_t v){ outw(io + o, v); }

static uint32_t phys(void *p) { return (uint32_t)(uintptr_t)p; }

/* Build a TD. mlen is the data length (0 allowed). */
static void make_td(struct uhci_td *td, uint32_t next, uint8_t pid,
                    uint8_t addr, uint8_t ep, int toggle, int mlen, void *buf) {
    td->link = next;
    td->cs = TD_ACTIVE | (3u << 27);                       /* 3 error retries */
    uint32_t maxlen = (mlen == 0) ? 0x7FF : (uint32_t)(mlen - 1) & 0x7FF;
    td->token = pid | ((uint32_t)addr << 8) | ((uint32_t)ep << 15)
              | ((uint32_t)(toggle & 1) << 19) | (maxlen << 21);
    td->buffer = buf ? phys(buf) : 0;
}

/* Run the control QH's TD chain and wait for it to finish. 0 = ok. */
static int run_control(int ntd) {
    for (int i = 0; i < ntd - 1; i++)
        tds[i].link = phys(&tds[i + 1]) | 0x4;   /* Q=0, T=0, Vf=1 (depth-first) */
    tds[ntd - 1].link = 1;                       /* terminate */

    qh_ctrl->element = phys(&tds[0]);
    uint64_t deadline = timer_ticks() + 30;      /* up to ~300 ms */
    while (timer_ticks() < deadline) {
        if (qh_ctrl->element & 1) {              /* QH drained -> all TDs done */
            for (int i = 0; i < ntd; i++)
                if (tds[i].cs & TD_ERRMASK)
                    return -1;
            return 0;
        }
    }
    return -1;                                   /* timed out */
}

/* A standard control transfer. setup[8] is the request; data/len is the data
 * stage (may be 0). `in` = data direction is device->host. */
static int control(const uint8_t setup[8], void *data, int len, int in) {
    memcpy(setup_buf, setup, 8);
    if (!in && len)
        memcpy(data_buf, data, len);

    int n = 0;
    make_td(&tds[n++], 0, PID_SETUP, dev_addr, 0, 0, 8, setup_buf);

    int toggle = 1, off = 0;
    uint8_t dpid = in ? PID_IN : PID_OUT;
    while (off < len) {
        int chunk = len - off;
        if (chunk > ep_maxp) chunk = ep_maxp;
        make_td(&tds[n++], 0, dpid, dev_addr, 0, toggle, chunk, data_buf + off);
        toggle ^= 1;
        off += chunk;
    }

    make_td(&tds[n++], 0, in ? PID_OUT : PID_IN, dev_addr, 0, 1, 0, 0);  /* status */

    int rc = run_control(n);
    if (rc == 0 && in && len)
        memcpy(data, data_buf, len);
    return rc;
}

static int get_descriptor(int type, int index, void *out, int len) {
    uint8_t s[8] = { 0x80, 0x06, (uint8_t)index, (uint8_t)type,
                     0, 0, (uint8_t)len, (uint8_t)(len >> 8) };
    return control(s, out, len, 1);
}

static int set_address(int addr) {
    uint8_t s[8] = { 0x00, 0x05, (uint8_t)addr, 0, 0, 0, 0, 0 };
    return control(s, 0, 0, 0);
}

static int set_configuration(int cfg) {
    uint8_t s[8] = { 0x00, 0x09, (uint8_t)cfg, 0, 0, 0, 0, 0 };
    return control(s, 0, 0, 0);
}

/* Reset and enable a root port; returns 1 if a device is attached. */
static int reset_port(uint16_t portsc) {
    if (!(rd(portsc) & 0x1))
        return 0;                          /* nothing connected */
    wr(portsc, rd(portsc) | (1 << 9));     /* assert reset */
    timer_wait(6);                         /* ~60 ms */
    wr(portsc, rd(portsc) & ~(1 << 9));    /* deassert */
    timer_wait(1);
    wr(portsc, rd(portsc) | (1 << 2));     /* enable port */
    timer_wait(1);
    return (rd(portsc) & 0x4) ? 1 : 0;     /* enabled? */
}

int usb_tablet_init(void) {
    pci_device_t dev = pci_find(0x8086, 0x7020);   /* PIIX3 UHCI */
    if (!dev.valid) { kprintf("[usb] no UHCI controller\n"); return -1; }
    pci_enable_bus_master(&dev);
    pci_write32(dev.bus, dev.slot, dev.func, 0xC0, 0x8F00);  /* disable legacy/SMI */
    io = (uint16_t)pci_bar(&dev, 4);

    /* reset the controller */
    wr(REG_CMD, 0x0004); timer_wait(2); wr(REG_CMD, 0);      /* global reset */
    wr(REG_CMD, 0x0002);                                     /* HC reset */
    for (int i = 0; i < 100000 && (rd(REG_CMD) & 0x0002); i++) {}
    wr(REG_INTR, 0);                                         /* no interrupts; we poll */

    /* allocate DMA structures (identity-mapped low RAM) */
    framelist = (uint32_t *)(uintptr_t)pmm_alloc_frame();
    uint8_t *pool = (uint8_t *)(uintptr_t)pmm_alloc_frame();
    uint8_t *bufs = (uint8_t *)(uintptr_t)pmm_alloc_frame();
    qh_int  = (struct uhci_qh *)(pool + 0);
    qh_ctrl = (struct uhci_qh *)(pool + 16);
    tds     = (struct uhci_td *)(pool + 64);
    int_td  = (struct uhci_td *)(pool + 64 + 16 * sizeof(struct uhci_td));
    setup_buf  = bufs + 0;
    data_buf   = bufs + 16;
    report_buf = bufs + 320;

    qh_ctrl->head = 1;                       /* terminate */
    qh_ctrl->element = 1;
    qh_int->head = phys(qh_ctrl) | 2;        /* -> control QH (Q bit) */
    qh_int->element = 1;
    for (int i = 0; i < 1024; i++)
        framelist[i] = phys(qh_int) | 2;     /* every frame -> interrupt QH */

    outl(io + REG_FRBASE, phys(framelist));   /* 32-bit register! */
    wr(REG_FRNUM, 0);
    outb(io + REG_SOF, 0x40);
    wr(REG_STS, 0xFFFF);                      /* clear status */
    wr(REG_CMD, 0x0081);                      /* Run + Max Packet 64 */

    /* find and reset the port the tablet is on */
    if (!reset_port(REG_PORTSC1) && !reset_port(REG_PORTSC2)) {
        kprintf("[usb] no device on either root port\n");
        return -1;
    }
    timer_wait(2);

    /* enumerate: addr 0, ep0 max packet defaults to 8 */
    dev_addr = 0; ep_maxp = 8;
    uint8_t devdesc[18];
    if (get_descriptor(1, 0, devdesc, 8) != 0) { kprintf("[usb] device descriptor failed\n"); return -1; }
    ep_maxp = devdesc[7] ? devdesc[7] : 8;

    if (set_address(1) != 0) { kprintf("[usb] SET_ADDRESS failed\n"); return -1; }
    dev_addr = 1;
    timer_wait(1);

    /* config descriptor: header first (to learn total length), then all of it */
    uint8_t cfg[256];
    if (get_descriptor(2, 0, cfg, 9) != 0) { kprintf("[usb] GET cfg hdr failed\n"); return -1; }
    int total = cfg[2] | (cfg[3] << 8);
    if (total > (int)sizeof(cfg)) total = sizeof(cfg);
    if (get_descriptor(2, 0, cfg, total) != 0) { kprintf("[usb] config descriptor failed\n"); return -1; }

    /* walk the config descriptor for an interrupt-IN endpoint */
    ep_in = 0;
    for (int i = 0; i + 1 < total; ) {
        int blen = cfg[i], btype = cfg[i + 1];
        if (blen == 0) break;
        if (btype == 0x05) {                         /* ENDPOINT descriptor */
            uint8_t addr = cfg[i + 2], attr = cfg[i + 3];
            if ((addr & 0x80) && (attr & 0x03) == 0x03) {   /* IN + interrupt */
                ep_in = addr & 0x0F;
                ep_maxp = cfg[i + 4] | (cfg[i + 5] << 8);
            }
        }
        i += blen;
    }
    if (!ep_in) { kprintf("[usb] no interrupt-IN endpoint\n"); return -1; }

    if (set_configuration(cfg[5]) != 0) { kprintf("[usb] SET_CONFIG failed\n"); return -1; }

    /* arm the interrupt endpoint */
    int_toggle = 0;
    make_td(int_td, 1, PID_IN, dev_addr, ep_in, int_toggle, ep_maxp, report_buf);
    qh_int->element = phys(int_td);

    mouse_set_abs(fb_width() / 2, fb_height() / 2, 0);
    ready = 1;
    kprintf("[usb] tablet ready: ep%d maxp=%d (absolute pointer)\n", ep_in, ep_maxp);
    return 0;
}

void usb_tablet_poll(void) {
    if (!ready)
        return;
    if (int_td->cs & TD_ACTIVE)
        return;                              /* no new report yet */

    int actlen = (int)((int_td->cs & 0x7FF) + 1) & 0x7FF;  /* 0x7FF -> 0 */
    if ((int_td->cs & TD_ERRMASK) == 0 && actlen >= 5) {
        /* QEMU tablet report: [buttons][x_lo][x_hi][y_lo][y_hi][wheel] */
        int buttons = report_buf[0] & 0x07;
        int rx = report_buf[1] | (report_buf[2] << 8);
        int ry = report_buf[3] | (report_buf[4] << 8);
        int x = rx * (fb_width() - 1) / 32767;
        int y = ry * (fb_height() - 1) / 32767;
        mouse_set_abs(x, y, buttons);
        if (actlen >= 6 && report_buf[5])         /* 6th byte: signed wheel delta (+up / -down) */
            mouse_add_wheel((int)(int8_t)report_buf[5]);
    }

    int_toggle ^= 1;                          /* re-arm with toggled data bit */
    make_td(int_td, 1, PID_IN, dev_addr, ep_in, int_toggle, ep_maxp, report_buf);
    qh_int->element = phys(int_td);
}
