/*
 * svga.c — VMware SVGA-II (PCI 0x15AD:0x0405) paravirtual 2D display driver.
 *
 * VMware SVGA-II is the second widely-emulated paravirtual display adapter (the
 * first, in this kernel, being QEMU std-VGA via Bochs DISPI in bochs_vbe.c/fb.c,
 * and the third virtio-gpu in virtio_gpu.c). QEMU emulates it as `-vga vmware`
 * or `-device vmware-svga`. Unlike DISPI's continuously-scanned linear
 * framebuffer, SVGA-II is driven through THREE PCI resources:
 *
 *   BAR0 = an I/O-port INDEX/VALUE register file. Write a register number to
 *          IObase+SVGA_INDEX_PORT, then read/write that register's 32-bit value
 *          at IObase+SVGA_VALUE_PORT (both 32-bit port accesses). This is how
 *          we confirm the device id/version, set the mode, and learn where the
 *          framebuffer and FIFO live.
 *   BAR1 = the LINEAR FRAMEBUFFER (VRAM): a flat array of 0x00RRGGBB pixels (the
 *          SAME layout fb.c draws into), but the host does NOT auto-scan it.
 *   BAR2 = the command FIFO: a ring of 32-bit words. To present, you write
 *          pixels into BAR1, push an SVGA_CMD_UPDATE rectangle into the FIFO,
 *          bump the ring's NEXT_CMD pointer, and poke SVGA_REG_SYNC; the host
 *          then copies that rectangle of the framebuffer to the display.
 *
 * Bring-up: confirm SVGA_ID_2 (write it to SVGA_REG_ID, read it back) → read
 * FB base/size + FIFO base/size → set WIDTH/HEIGHT/BPP=32 → SVGA_REG_ENABLE=1 →
 * initialize the FIFO ring (MIN/MAX/NEXT_CMD/STOP, the first four words of FIFO
 * memory) → SVGA_REG_CONFIG_DONE=1. svga_present() then drives the per-frame
 * UPDATE.
 *
 * SAFE SCOPE: ADDITIVE. The boot display path (fb.c + bochs_vbe.c, the linear
 * framebuffer) is UNTOUCHED; svga_init() is a clean no-op (returns -1) when no
 * VMware SVGA-II device is attached, so a machine without one boots unchanged.
 * SVGA_ID_2 is confirmed BEFORE touching anything else; WIDTH/HEIGHT are capped
 * to a sane ceiling and to what the FB size allows; every FIFO write is bounded
 * within [MIN,MAX) with the NEXT_CMD wrap handled; framebuffer writes are bounded
 * within the FB size; waits on SVGA_REG_BUSY are finite. Like virtio_gpu.c, the
 * headless proof is svga_selftest() (program a mode, write a test pattern, run a
 * FIFO UPDATE, read registers back), with fb.c untouched.
 */
#include "svga.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "console.h"
#include "io.h"

/* ---- PCI identity ---------------------------------------------------------- */
#define SVGA_PCI_VENDOR  0x15AD   /* VMware */
#define SVGA_PCI_DEVICE  0x0405   /* SVGA-II */

/* ---- I/O-port register file (BAR0) ----------------------------------------
 * Byte offsets from the I/O BAR base, accessed with 32-bit in/out: write a
 * register index to the index port, then read/write the value port. */
#define SVGA_INDEX_PORT  0
#define SVGA_VALUE_PORT  1

/* ---- the canonical SVGA-II register numbers (svga_reg.h) -------------------
 * These are written to the index port; their 32-bit value is then transferred
 * through the value port. Only the subset this driver uses is defined. */
#define SVGA_REG_ID                0   /* device id / version handshake        */
#define SVGA_REG_ENABLE            1   /* 1 = SVGA mode on (0 = legacy VGA)    */
#define SVGA_REG_WIDTH             2   /* current mode width  (pixels)         */
#define SVGA_REG_HEIGHT            3   /* current mode height (pixels)         */
#define SVGA_REG_MAX_WIDTH         4   /* RO: largest supported width          */
#define SVGA_REG_MAX_HEIGHT        5   /* RO: largest supported height         */
#define SVGA_REG_DEPTH             6   /* colour depth (bits used per pixel)   */
#define SVGA_REG_BITS_PER_PIXEL    7   /* bits per pixel of the framebuffer    */
#define SVGA_REG_BYTES_PER_LINE   12   /* RO once mode set: framebuffer pitch  */
#define SVGA_REG_FB_START         13   /* RO: framebuffer (VRAM) phys address  */
#define SVGA_REG_FB_OFFSET        14   /* RO: byte offset of the visible image */
#define SVGA_REG_VRAM_SIZE        15   /* RO: total VRAM bytes                 */
#define SVGA_REG_FB_SIZE          16   /* RO: visible framebuffer size (bytes) */
#define SVGA_REG_CAPABILITIES     17   /* RO: SVGA_CAP_* feature bits          */
#define SVGA_REG_MEM_START        18   /* RO: command-FIFO (MMIO) phys address */
#define SVGA_REG_MEM_SIZE         19   /* RO: command-FIFO size (bytes)        */
#define SVGA_REG_CONFIG_DONE      20   /* write 1 once the FIFO ring is set up */
#define SVGA_REG_SYNC             21   /* write != 0 to flush the FIFO         */
#define SVGA_REG_BUSY             22   /* RO: nonzero while the FIFO is busy   */

/* SVGA_REG_ID handshake: write SVGA_ID_2, read it back. If the device supports
 * SVGA-II it echoes SVGA_ID_2; otherwise it returns a lower id (or doesn't
 * echo). The high half (0x90000000, "magic") plus the version (2) is the id. */
#define SVGA_MAGIC      0x900000u
#define SVGA_ID_2       (uint32_t)((SVGA_MAGIC << 8) | 2)   /* 0x90000002 */

/* ---- FIFO ring header (the first words of the FIFO memory) -----------------
 * The legacy (pre-GMR) FIFO is a ring of 32-bit words. Its first four words are
 * registers describing the ring: MIN (byte offset where commands begin), MAX
 * (one past the ring's last byte), NEXT_CMD (the driver's write cursor, in
 * bytes), STOP (the host's read cursor, in bytes). We index FIFO memory as a
 * uint32_t array, so these are WORD indices. */
#define SVGA_FIFO_MIN        0
#define SVGA_FIFO_MAX        1
#define SVGA_FIFO_NEXT_CMD   2
#define SVGA_FIFO_STOP       3
#define SVGA_FIFO_NUM_REGS   4    /* MIN must be at least this many words in  */

/* FIFO command opcodes (a command is a sequence of 32-bit words). We only emit
 * UPDATE: opcode (1) followed by x, y, width, height. */
#define SVGA_CMD_UPDATE      1

/* ---- defensive caps -------------------------------------------------------
 * Same ceiling as bochs_vbe.c / virtio_gpu.c so the mode stays a sane size. */
#define SVGA_MAX_W   1920
#define SVGA_MAX_H   1200

/* Default mode if the device reports nothing usable (matches the std-VGA boot). */
#define SVGA_DEF_W   1024
#define SVGA_DEF_H    768

/* Driver state. */
static struct {
    int present;

    uint16_t io_base;            /* BAR0 I/O-port base for the index/value file */

    volatile uint32_t *fb;       /* BAR1 linear framebuffer (mapped) */
    uint64_t  fb_phys;
    uint32_t  fb_size;           /* visible framebuffer size in bytes (capped to map) */
    uint32_t  fb_mapped;         /* bytes actually mapped (page-rounded)            */

    volatile uint32_t *fifo;     /* BAR2 command FIFO (mapped), indexed as words */
    uint64_t  fifo_phys;
    uint32_t  fifo_size;         /* FIFO size in bytes (capped to mapped) */
    uint32_t  fifo_mapped;

    int width, height;
    uint32_t bytes_per_line;     /* device-reported pitch */
} sv;

/* ---- index/value register file accessors ---------------------------------- */
static inline void svga_w(uint32_t reg, uint32_t val) {
    outl(sv.io_base + SVGA_INDEX_PORT, reg);
    outl(sv.io_base + SVGA_VALUE_PORT, val);
}
static inline uint32_t svga_r(uint32_t reg) {
    outl(sv.io_base + SVGA_INDEX_PORT, reg);
    return inl(sv.io_base + SVGA_VALUE_PORT);
}

/* Map `len` bytes of MMIO starting at the page containing `phys` (identity map),
 * cache-disabled — same helper shape virtio_gpu.c/ahci.c/hda.c use. Returns a
 * pointer to `phys`, or NULL on a zero address. Maps whole pages. */
static volatile uint32_t *map_mmio(uint64_t phys, uint32_t len) {
    if (!phys || !len)
        return NULL;
    uint64_t start = phys & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end   = phys + len;
    for (uint64_t off = start; off < end; off += PAGE_SIZE)
        vmm_map(off, off, PTE_WRITABLE | PTE_PCD);
    return (volatile uint32_t *)(uintptr_t)phys;
}

/* ---- FIFO ring helpers ----------------------------------------------------- */

/* Initialize the FIFO ring: commands start at MIN (just past the four header
 * words, page-aligned conceptually — here 16 bytes), span up to the FIFO size,
 * and the write/read cursors start empty (NEXT_CMD == STOP == MIN). Then tell
 * the device the ring is configured. */
static void fifo_init(void) {
    uint32_t min = SVGA_FIFO_NUM_REGS * 4;       /* 16 bytes: just past the 4 reg words */
    uint32_t max = sv.fifo_size;
    /* The ring must hold at least the header plus one max-size command; if the
     * device reported a tiny FIFO, clamp MAX up to what we mapped (we already
     * verified fifo_size >= a floor in init). */
    sv.fifo[SVGA_FIFO_MIN]      = min;
    sv.fifo[SVGA_FIFO_MAX]      = max;
    sv.fifo[SVGA_FIFO_NEXT_CMD] = min;
    sv.fifo[SVGA_FIFO_STOP]     = min;
    svga_w(SVGA_REG_CONFIG_DONE, 1);
}

/* Push one 32-bit word into the FIFO ring at NEXT_CMD, wrapping within
 * [MIN,MAX). If the ring is full (advancing NEXT_CMD would collide with STOP),
 * sync the device to let it drain first. Returns 0 on success, -1 if the word
 * cannot be written (ring un-init / would overrun the mapped region). */
static int fifo_push(uint32_t word) {
    uint32_t min  = sv.fifo[SVGA_FIFO_MIN];
    uint32_t max  = sv.fifo[SVGA_FIFO_MAX];
    uint32_t next = sv.fifo[SVGA_FIFO_NEXT_CMD];

    /* Bound everything within the mapped FIFO: MIN must be >= the header, MAX
     * must not exceed what we mapped, and NEXT_CMD must be a valid in-ring,
     * word-aligned byte offset. Reject (don't write OOB) if any is off. */
    if (max > sv.fifo_mapped) max = sv.fifo_mapped;
    if (min < SVGA_FIFO_NUM_REGS * 4 || max <= min || (max & 3) || (min & 3))
        return -1;
    if (next < min || next >= max || (next & 3))
        return -1;

    uint32_t nextcmd = next + 4;
    if (nextcmd >= max)
        nextcmd = min;                            /* wrap to the ring start */

    /* If the ring is full (the next write position == the host's read cursor),
     * sync so the host drains, with a finite wait. */
    if (nextcmd == sv.fifo[SVGA_FIFO_STOP]) {
        svga_w(SVGA_REG_SYNC, 1);
        for (int i = 0; i < 1000000 && nextcmd == sv.fifo[SVGA_FIFO_STOP]; i++)
            (void)svga_r(SVGA_REG_BUSY);
        if (nextcmd == sv.fifo[SVGA_FIFO_STOP])
            return -1;                            /* still full — give up cleanly */
    }

    sv.fifo[next / 4] = word;                     /* word index = byte offset / 4 */
    __asm__ volatile("" ::: "memory");            /* publish the word before the cursor */
    sv.fifo[SVGA_FIFO_NEXT_CMD] = nextcmd;
    return 0;
}

/* Emit an SVGA_CMD_UPDATE for rect (x,y,w,h): opcode then the four rect words.
 * Returns 0 if all five words were pushed, -1 otherwise. */
static int fifo_update(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (fifo_push(SVGA_CMD_UPDATE) != 0) return -1;
    if (fifo_push(x) != 0) return -1;
    if (fifo_push(y) != 0) return -1;
    if (fifo_push(w) != 0) return -1;
    if (fifo_push(h) != 0) return -1;
    return 0;
}

/* Flush the FIFO: poke SYNC, then wait (finite) for BUSY to clear. */
static void fifo_sync(void) {
    svga_w(SVGA_REG_SYNC, 1);
    for (int i = 0; i < 1000000 && svga_r(SVGA_REG_BUSY); i++)
        __asm__ volatile("pause");
}

/* ---- bring-up ------------------------------------------------------------- */
int svga_init(void) {
    memset(&sv, 0, sizeof(sv));

    pci_device_t dev = pci_find(SVGA_PCI_VENDOR, SVGA_PCI_DEVICE);
    if (!dev.valid)
        return -1;                                /* no VMware SVGA-II — clean no-op */

    /* Enable memory-space decode + bus mastering (the FB + FIFO are memory
     * BARs; CONFIG_DONE lets the host DMA out of the FIFO). */
    pci_enable_bus_master(&dev);

    /* BAR0 is the I/O-port register file. pci_bar masks the low flag bits; an
     * I/O BAR keeps bit0 set in the raw value, so mask it to the port base. */
    uint32_t bar0 = pci_bar(&dev, 0);
    if (bar0 == 0)
        return -1;
    sv.io_base = (uint16_t)(bar0 & 0xFFFCu);      /* I/O BARs are dword-aligned */

    /* CONFIRM THE DEVICE FIRST, before touching anything else: write SVGA_ID_2
     * to SVGA_REG_ID and read it back. A real SVGA-II echoes it; anything else
     * means the device is absent / a wrong version → bail clean. */
    svga_w(SVGA_REG_ID, SVGA_ID_2);
    uint32_t id = svga_r(SVGA_REG_ID);
    if (id != SVGA_ID_2)
        return -1;

    /* Read the framebuffer + FIFO geometry from the device. */
    uint64_t fb_start   = svga_r(SVGA_REG_FB_START);
    uint32_t fb_size    = svga_r(SVGA_REG_FB_SIZE);
    uint32_t vram_size  = svga_r(SVGA_REG_VRAM_SIZE);
    uint64_t fifo_start = svga_r(SVGA_REG_MEM_START);
    uint32_t fifo_size  = svga_r(SVGA_REG_MEM_SIZE);
    uint32_t max_w      = svga_r(SVGA_REG_MAX_WIDTH);
    uint32_t max_h      = svga_r(SVGA_REG_MAX_HEIGHT);

    /* The device must report sane FB + FIFO regions. The FIFO must hold at least
     * its header plus a few commands; require a small floor (and a non-zero FB). */
    if (fb_start == 0 || fifo_start == 0)
        return -1;
    if (fb_size == 0 || fb_size > vram_size)      /* visible image fits in VRAM */
        fb_size = vram_size;                      /* (some hosts report 0 here) */
    if (fb_size == 0)
        return -1;
    if (fifo_size < SVGA_FIFO_NUM_REGS * 4 + 64)  /* header + a little command room */
        return -1;

    /* Choose a mode: cap to our ceiling AND to what the device allows AND to
     * what the framebuffer can hold (width*height*4 must fit in fb_size). */
    int w = SVGA_DEF_W, h = SVGA_DEF_H;
    if (max_w && (uint32_t)w > max_w) w = (int)max_w;
    if (max_h && (uint32_t)h > max_h) h = (int)max_h;
    if (w > SVGA_MAX_W) w = SVGA_MAX_W;
    if (h > SVGA_MAX_H) h = SVGA_MAX_H;
    if (w <= 0 || h <= 0)
        return -1;
    /* Reject a mode whose framebuffer would not fit (safety: never let a draw
     * index past the FB). If the default doesn't fit the reported FB, fall back
     * to a smaller standard mode that does. */
    if ((uint64_t)w * (uint64_t)h * 4u > fb_size) {
        w = 640; h = 480;
        if ((uint64_t)w * (uint64_t)h * 4u > fb_size)
            return -1;                            /* FB absurdly small — bail */
    }

    /* Map the framebuffer (cap the mapping to the visible FB size) and the FIFO
     * (cap to its reported size), cache-disabled, identity-mapped. */
    uint32_t fb_map_bytes = fb_size;
    sv.fb = map_mmio(fb_start, fb_map_bytes);
    if (!sv.fb)
        return -1;
    sv.fb_phys   = fb_start;
    sv.fb_size   = fb_size;
    sv.fb_mapped = (fb_map_bytes + (PAGE_SIZE - 1)) & ~(uint32_t)(PAGE_SIZE - 1);

    sv.fifo = map_mmio(fifo_start, fifo_size);
    if (!sv.fifo)
        return -1;
    sv.fifo_phys   = fifo_start;
    sv.fifo_size   = fifo_size;
    sv.fifo_mapped = (fifo_size + (PAGE_SIZE - 1)) & ~(uint32_t)(PAGE_SIZE - 1);

    /* Program the mode: width, height, 32 bpp, then enable SVGA mode. */
    svga_w(SVGA_REG_WIDTH,          (uint32_t)w);
    svga_w(SVGA_REG_HEIGHT,         (uint32_t)h);
    svga_w(SVGA_REG_BITS_PER_PIXEL, 32);
    svga_w(SVGA_REG_ENABLE,         1);

    /* Read the geometry the device actually committed to (width/height/pitch
     * may be normalized by the host). Re-clamp to the mapped FB to be safe. */
    sv.width          = (int)svga_r(SVGA_REG_WIDTH);
    sv.height         = (int)svga_r(SVGA_REG_HEIGHT);
    sv.bytes_per_line = svga_r(SVGA_REG_BYTES_PER_LINE);
    if (sv.width <= 0 || sv.height <= 0) {        /* host didn't honor the set */
        sv.width = w; sv.height = h;
    }
    if (sv.width  > SVGA_MAX_W) sv.width  = SVGA_MAX_W;
    if (sv.height > SVGA_MAX_H) sv.height = SVGA_MAX_H;
    /* Final FB-fit guard against the committed geometry. */
    if ((uint64_t)sv.width * (uint64_t)sv.height * 4u > sv.fb_size) {
        svga_w(SVGA_REG_ENABLE, 0);
        return -1;
    }

    /* Initialize the FIFO ring and tell the device it's configured. */
    fifo_init();

    sv.present = 1;
    kprintf("[ ok ] vmware-svga up: SVGA_ID_2 confirmed, mode %dx%d@32 "
            "(FB %p %u KB, FIFO %p %u KB; boot display stays on the linear framebuffer).\n",
            sv.width, sv.height, (void *)(uintptr_t)sv.fb_phys, sv.fb_size / 1024,
            (void *)(uintptr_t)sv.fifo_phys, sv.fifo_size / 1024);
    return 0;
}

int       svga_active(void)      { return sv.present; }
int       svga_width(void)       { return sv.present ? sv.width  : 0; }
int       svga_height(void)      { return sv.present ? sv.height : 0; }
uint32_t *svga_framebuffer(void) { return sv.present ? (uint32_t *)sv.fb : 0; }

/* Present the rectangle [x,y,w,h]: emit an SVGA_CMD_UPDATE for that rect into
 * the FIFO, then sync. The rect is clamped within [0,width]x[0,height] so we
 * never tell the host to read framebuffer pixels past the mode. */
int svga_present(int x, int y, int w, int h) {
    if (!sv.present)
        return -1;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= sv.width || y >= sv.height) return -1;
    if (x + w > sv.width)  w = sv.width  - x;
    if (y + h > sv.height) h = sv.height - y;
    if (w <= 0 || h <= 0)
        return -1;
    if (fifo_update((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h) != 0)
        return -1;
    fifo_sync();
    return 0;
}

/* ---- boot-time verification (the headless proof, like virtio_gpu_selftest) - */
void svga_selftest(void) {
    if (!sv.present) {
        kprintf("[vmware-svga] no VMware SVGA-II device found "
                "(none attached; linear-framebuffer display intact).\n\n");
        return;
    }

    kprintf("[vmware-svga] selftest: device up, mode %dx%d, pitch %u bytes; "
            "FB phys %p (%u bytes), FIFO phys %p (%u bytes).\n",
            sv.width, sv.height, sv.bytes_per_line,
            (void *)(uintptr_t)sv.fb_phys, sv.fb_size,
            (void *)(uintptr_t)sv.fifo_phys, sv.fifo_size);

    /* Fill the framebuffer with a known test pattern: vertical colour bands (no
     * FP — the kernel builds -mgeneral-regs-only). Bands prove a real write to
     * the BAR1 framebuffer, not just a status code. Use the device pitch
     * (bytes_per_line) to step rows, since the host may pad the stride. */
    static const uint32_t bands[8] = {
        0x000000, 0xFF0000, 0x00FF00, 0x0000FF,
        0xFFFF00, 0x00FFFF, 0xFF00FF, 0xFFFFFF
    };
    uint32_t pitch_px = sv.bytes_per_line ? (sv.bytes_per_line / 4) : (uint32_t)sv.width;
    if (pitch_px == 0) pitch_px = (uint32_t)sv.width;
    for (int y = 0; y < sv.height; y++) {
        /* Bound the row within the mapped FB before writing it. */
        uint64_t row_off = (uint64_t)y * pitch_px * 4u;
        if (row_off + (uint64_t)sv.width * 4u > sv.fb_mapped)
            break;
        volatile uint32_t *row = sv.fb + (size_t)y * pitch_px;
        for (int x = 0; x < sv.width; x++)
            row[x] = bands[(x * 8) / sv.width];
    }

    /* Read a couple of registers back to confirm the device state (the headless
     * equivalent of "DMA ADVANCING"): SVGA_ID_2 still echoes, ENABLE is on. */
    int id_ok     = (svga_r(SVGA_REG_ID) == SVGA_ID_2);
    int enable_ok = (svga_r(SVGA_REG_ENABLE) != 0);
    uint32_t rd_w = svga_r(SVGA_REG_WIDTH);
    uint32_t rd_h = svga_r(SVGA_REG_HEIGHT);

    /* Emit an UPDATE over the WHOLE screen + sync (the present cycle), then a
     * sub-rect present to exercise the rect clamp + FIFO wrap math. */
    int rc_full = fifo_update(0, 0, (uint32_t)sv.width, (uint32_t)sv.height);
    fifo_sync();
    int rc_rect = svga_present(0, 0, sv.width / 2, sv.height / 2);

    kprintf("[vmware-svga] selftest: SVGA_ID_2=%s ENABLE=%s readback %ux%u; "
            "UPDATE(full)=%s rect-present=%s\n",
            id_ok ? "OK" : "FAIL", enable_ok ? "OK" : "FAIL", rd_w, rd_h,
            rc_full == 0 ? "OK" : "FAIL", rc_rect == 0 ? "OK" : "FAIL");

    if (id_ok && enable_ok && rc_full == 0 && rc_rect == 0)
        kprintf("[ ok ] vmware-svga present cycle complete: test pattern written to FB, "
                "UPDATE emitted + synced, registers confirmed.\n\n");
    else
        kprintf("[vmware-svga] PRESENT CYCLE FAILED (a step did not confirm OK).\n\n");
}
