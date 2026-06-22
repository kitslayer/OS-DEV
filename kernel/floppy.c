/*
 * floppy.c — 82077AA floppy disk controller (FDC) driver, read path, via ISA DMA.
 *
 * See floppy.h for the high-level shape. The interesting, NEW-to-this-kernel
 * mechanism here is ISA DMA: the floppy controller cannot bus-master like the
 * AHCI/NVMe/virtio/e1000 controllers do, so it hands its data stream to the
 * legacy 8237 DMA controller (channel 2). To read a sector we:
 *
 *   1. program the 8237 channel 2 to WRITE-to-memory (the FDC reads the disk and
 *      pushes bytes; from memory's point of view that's a write) at the physical
 *      address of a bounce buffer, for `count` bytes;
 *   2. issue the FDC READ DATA command with the target CHS;
 *   3. wait for the FDC's result phase (poll the MSR with a finite timeout),
 *      read the 7 result bytes, and check ST0/ST1/ST2 for errors;
 *   4. copy the bounce buffer into the caller's buffer.
 *
 * The 8237 has hard constraints the bounce buffer must satisfy: it can only
 * address the low 16 MiB (a 24-bit address: 16 low bits in the channel address
 * register + 8 bits in the channel's page register) and a single transfer must
 * not cross a 64 KiB physical boundary (the 8237 increments only the low 16 bits
 * — the page register does not carry). pmm_alloc_frame() returns identity-mapped
 * low RAM (phys == virt, well under 16 MiB), so a frame satisfies the 16 MiB
 * rule; we additionally REJECT (and re-try a few times for) any frame that would
 * straddle a 64 KiB boundary, asserting the constraint rather than trusting it.
 *
 * We POLL for completion (like ahci.c / nvme.c) rather than taking IRQ6: it is
 * simpler and matches the other drivers. So IRQ6 stays MASKED at the PIC the
 * whole time (pic_init() masks all lines at boot and we never unmask 6), which
 * means a stray completion interrupt can never fire and wedge anything. The FDC
 * still latches its "operation complete" internally; SENSE INTERRUPT STATUS in
 * the seek/recalibrate result phase reads that latch without needing the IRQ.
 */
#include "floppy.h"
#include "io.h"
#include "timer.h"
#include "pmm.h"
#include "vmm.h"
#include "console.h"
#include "string.h"
#include "pic.h"

/* ---- 82077AA register file (legacy I/O ports) ---------------------------- */
#define FDC_DOR  0x3F2   /* Digital Output Register (motor/DMA/reset/drive sel) */
#define FDC_MSR  0x3F4   /* Main Status Register (read: RQM/DIO/CMD-BSY/...)     */
#define FDC_DSR  0x3F4   /* Data rate Select Register (write side of 0x3F4)     */
#define FDC_FIFO 0x3F5   /* data FIFO (command bytes out, result bytes in)      */
#define FDC_CCR  0x3F7   /* Configuration Control Register (data rate)          */

/* DOR bits. */
#define DOR_DSEL0   0x00   /* drive-select 0 in bits 0..1 (we use drive 0)      */
#define DOR_NRESET  0x04   /* bit2: 1 = controller NOT in reset (i.e. enabled)  */
#define DOR_DMAGATE 0x08   /* bit3: 1 = DMA + IRQ enabled                       */
#define DOR_MOTOR0  0x10   /* bit4: drive-0 motor on                            */

/* MSR bits. */
#define MSR_RQM     0x80   /* bit7: FIFO ready to transfer a byte               */
#define MSR_DIO     0x40   /* bit6: data direction — 1 = FDC->CPU (read result) */
#define MSR_NDMA    0x20   /* bit5: non-DMA mode in progress                    */
#define MSR_CMDBSY  0x10   /* bit4: a command is in progress                    */

/* FDC commands (low 5 bits) + flags (high bits). */
#define CMD_SPECIFY      0x03
#define CMD_SENSE_INT    0x08
#define CMD_RECALIBRATE  0x07
#define CMD_SEEK         0x0F
#define CMD_READ_DATA    0x06
/* READ DATA modifiers: MT (multi-track), MFM (the standard double-density
 * encoding), SK (skip deleted) — 0xE6 = MT|MFM|SK | READ DATA. */
#define READ_FLAGS       (0x80 | 0x40 | 0x20)   /* MT | MFM | SK */

/* ST0 (first result byte) interrupt-code field (bits 7..6): 00 = normal end. */
#define ST0_IC_MASK      0xC0
#define ST0_IC_NORMAL    0x00

/* ---- 8237 ISA DMA controller (channel 2) --------------------------------- */
/* The low (8-bit) DMA controller's registers. Channel 2 is the floppy channel.
 * Its address/count registers are at 0x04/0x05; the page register is 0x81. The
 * single-mask, mode, flip-flop-clear, master-clear ports are shared. */
#define DMA_CH2_ADDR     0x04   /* channel-2 base+current address (low 16 bits) */
#define DMA_CH2_COUNT    0x05   /* channel-2 base+current count (bytes-1)       */
#define DMA_CH2_PAGE     0x81   /* channel-2 page register (address bits 16..23)*/
#define DMA_MASK_SINGLE  0x0A   /* single-channel mask register                 */
#define DMA_MODE         0x0B   /* mode register                                */
#define DMA_FLIPFLOP     0x0C   /* clear byte pointer flip-flop (any write)     */

#define DMA_MASK_CH2     0x06   /* mask channel 2 (bit2 set + channel# 2)       */
#define DMA_UNMASK_CH2   0x02   /* unmask channel 2 (channel# 2, mask bit clear)*/
/* Mode byte: bits1..0 = channel (2); bits3..2 = transfer type
 * (01 = write-to-memory for a disk READ, 10 = read-from-memory for a write);
 * bit4 = auto-init off; bits7..6 = mode (01 = single transfer). */
#define DMA_MODE_READ    0x46   /* single + write-to-memory + channel 2         */

/* ---- driver state -------------------------------------------------------- */
static int      g_present;          /* 1 once floppy_init() succeeded          */
static int      g_probed;           /* have we run floppy_init() yet?          */
static uint64_t g_bounce_phys;      /* the bounce buffer's physical address    */
static uint8_t  g_motor_on;         /* is the drive-0 motor currently spun up? */

/* The ISA-DMA bounce buffer. ISA DMA (the 8237) can only address the low 16 MiB
 * (a 24-bit physical address) and a single transfer must not cross a 64 KiB
 * boundary. The kernel's static arenas push kernel_end well past 16 MiB (the JS
 * arena alone is 44 MiB), so the PMM has NO free frame that low — instead we
 * RESERVE the buffer in the kernel image's .lowbss section (see linker.ld),
 * which is placed before the big .bss arenas and is therefore guaranteed to sit
 * within the low 16 MiB. Page-aligned (4 KiB) so it can never straddle a 64 KiB
 * boundary; init asserts both. The kernel runs identity-mapped in the low 1 GiB,
 * so its virtual address is also its physical address — what the 8237 needs. */
static uint8_t g_bounce[PAGE_SIZE]
    __attribute__((aligned(PAGE_SIZE), section(".lowbss")));

/* The bounce buffer is one page (4 KiB). A 1.44 MB track is 9216 B, but we keep
 * a single sector's worth of guaranteed-safe transfer per command and never
 * cross a track; capping `count` at FLOPPY_DMA_MAX_SECTORS bounds every DMA to
 * the bounce size and keeps a transfer far inside one 64 KiB window. 4 KiB = 8
 * sectors, well under a track's 18, so a single READ DATA never spans tracks. */
#define FLOPPY_DMA_MAX_SECTORS (PAGE_SIZE / FLOPPY_SECTOR_SIZE)   /* 8 */

/* ---- low-level FDC FIFO handshake (all finite-timeout) -------------------- */

/* Wait until the controller's FIFO is ready (RQM=1) to move a byte in the given
 * direction (dio: 0 = CPU->FDC for a command byte, MSR_DIO = FDC->CPU for a
 * result byte). Returns 0 when ready, -1 on timeout / absent controller (whose
 * MSR reads 0xFF: RQM and DIO both 1, so a result-direction wait would falsely
 * succeed — callers detect absence up front in floppy_reset instead). */
static int fdc_wait_rqm(uint8_t dio) {
    for (int i = 0; i < 100000; i++) {
        uint8_t msr = inb(FDC_MSR);
        if ((msr & MSR_RQM) && ((msr & MSR_DIO) == dio))
            return 0;
    }
    return -1;
}

/* Write one command/parameter byte to the FIFO (waits for RQM, CPU->FDC). */
static int fdc_write(uint8_t b) {
    if (fdc_wait_rqm(0) < 0)
        return -1;
    outb(FDC_FIFO, b);
    return 0;
}

/* Read one result byte from the FIFO (waits for RQM, FDC->CPU). */
static int fdc_read(uint8_t *out) {
    if (fdc_wait_rqm(MSR_DIO) < 0)
        return -1;
    *out = inb(FDC_FIFO);
    return 0;
}

/* SENSE INTERRUPT STATUS: returns ST0 + the present-cylinder number. Used to
 * acknowledge a reset/seek/recalibrate completion (clears the FDC's internal
 * interrupt latch — necessary even when we poll and keep IRQ6 masked). */
static int fdc_sense_interrupt(uint8_t *st0, uint8_t *cyl) {
    if (fdc_write(CMD_SENSE_INT) < 0)
        return -1;
    if (fdc_read(st0) < 0)
        return -1;
    if (fdc_read(cyl) < 0)
        return -1;
    return 0;
}

/* ---- motor control ------------------------------------------------------- */

/* Spin the drive-0 motor up (DMA gated on, not in reset) and wait for it to
 * reach speed. The 82077AA needs ~300 ms after motor-on before a read is
 * reliable; we wait 500 ms (50 ticks at 100 Hz) to be safe. Idempotent. */
static void floppy_motor_on(void) {
    outb(FDC_DOR, DOR_NRESET | DOR_DMAGATE | DOR_MOTOR0 | DOR_DSEL0);
    if (!g_motor_on) {
        timer_wait(50);          /* ~500 ms spin-up */
        g_motor_on = 1;
    }
}

/* Drop the motor (leave the controller enabled + DMA gated). */
static void floppy_motor_off(void) {
    outb(FDC_DOR, DOR_NRESET | DOR_DMAGATE | DOR_DSEL0);
    g_motor_on = 0;
}

/* ---- ISA DMA (8237 channel 2) -------------------------------------------- */

/* Program the 8237 for a `len`-byte transfer into the bounce buffer. `write`
 * selects the FDC direction: 0 = disk READ (8237 writes memory). Returns 0, or
 * -1 if the buffer/length violates an ISA-DMA constraint (caught at setup too).
 *
 * Sequence (per the 8237 datasheet, the canonical floppy-DMA recipe):
 *   mask the channel; clear the flip-flop; set the mode; write the 16-bit
 *   address low-then-high; write the page (bits 16..23); clear the flip-flop;
 *   write the 16-bit count low-then-high (count = bytes-1); unmask the channel. */
static int dma_prepare(uint32_t len, int write) {
    uint64_t phys = g_bounce_phys;
    /* The 8237 addresses 24 bits (16 MiB) and must not cross a 64 KiB boundary.
     * Both are guaranteed at setup, but re-assert here (defence in depth). */
    if (phys > 0x00FFFFFFull)
        return -1;
    if (len == 0 || len > PAGE_SIZE)
        return -1;
    if (((phys & 0xFFFF) + (len - 1)) > 0xFFFF)   /* would cross a 64 KiB window */
        return -1;

    uint8_t addr_lo = (uint8_t)(phys & 0xFF);
    uint8_t addr_hi = (uint8_t)((phys >> 8) & 0xFF);
    uint8_t page    = (uint8_t)((phys >> 16) & 0xFF);
    uint16_t count  = (uint16_t)(len - 1);        /* the 8237 count is bytes-1 */

    outb(DMA_MASK_SINGLE, DMA_MASK_CH2);          /* mask channel 2 while we program it */
    outb(DMA_FLIPFLOP, 0xFF);                     /* reset the byte flip-flop */
    outb(DMA_MODE, write ? 0x4A : DMA_MODE_READ); /* 0x4A = read-from-memory (write to disk) */

    outb(DMA_CH2_ADDR, addr_lo);                  /* address bits 0..7 */
    outb(DMA_CH2_ADDR, addr_hi);                  /* address bits 8..15 */
    outb(DMA_CH2_PAGE, page);                     /* address bits 16..23 */

    outb(DMA_FLIPFLOP, 0xFF);                     /* reset the flip-flop again for the count */
    outb(DMA_CH2_COUNT, (uint8_t)(count & 0xFF)); /* count bits 0..7 */
    outb(DMA_CH2_COUNT, (uint8_t)(count >> 8));   /* count bits 8..15 */

    outb(DMA_MASK_SINGLE, DMA_UNMASK_CH2);        /* unmask: the channel is armed */
    return 0;
}

/* ---- LBA -> CHS for a 1.44 MB diskette ----------------------------------- */
static void lba_to_chs(uint32_t lba, uint8_t *cyl, uint8_t *head, uint8_t *sect) {
    *cyl  = (uint8_t)(lba / (FLOPPY_SECTORS_TRACK * FLOPPY_HEADS));
    *head = (uint8_t)((lba / FLOPPY_SECTORS_TRACK) % FLOPPY_HEADS);
    *sect = (uint8_t)((lba % FLOPPY_SECTORS_TRACK) + 1);   /* sectors are 1-based */
}

/* ---- controller bring-up ------------------------------------------------- */

/* Reset + configure the controller and recalibrate the head to cylinder 0.
 * Detects an absent FDC (no sane MSR after reset) and bails cleanly. Returns 0
 * on success, -1 on absence/timeout/error.
 *
 * Note we do NOT sample the MSR *before* the reset to detect absence: at boot
 * the firmware leaves the controller held in reset (DOR bit2 = 0), so a PRESENT
 * controller's MSR also reads 0x00 then — a pre-reset 0x00 says nothing. We
 * reset first, then require a sane post-reset MSR (RQM set, CMD-BSY clear, not
 * the all-ones float of an empty I/O range) as the present/absent test. */
static int floppy_reset(void) {
    /* Enter then leave reset (DOR bit2 low -> high), with DMA gated on. Toggling
     * NRESET low for a moment and back high triggers the controller's reset,
     * which raises an interrupt we must clear with SENSE INTERRUPT x4. */
    outb(FDC_DOR, 0x00);                                   /* assert reset (all off) */
    timer_wait(1);                                         /* ~10 ms */
    outb(FDC_DOR, DOR_NRESET | DOR_DMAGATE | DOR_DSEL0);   /* deassert reset, gate DMA */
    timer_wait(1);                                         /* let it come out of reset */

    /* Absent-controller test: a present 82077AA settles its MSR to 0x80 (RQM=1,
     * CMD-BSY=0, DIO=0 — ready to accept a command) within microseconds of
     * leaving reset. An ABSENT controller leaves the I/O range floating: MSR
     * reads 0xFF (RQM looks set but CMD-BSY is *also* set, impossible at idle) or
     * stays 0x00. Require a sane "ready, idle, accept-command" MSR; bail on a
     * floating bus so we never spin on a controller that isn't there. */
    int sane = 0;
    for (int i = 0; i < 100000; i++) {
        uint8_t msr = inb(FDC_MSR);
        if (msr == 0xFF)
            return -1;                       /* floating bus: no controller */
        if ((msr & MSR_RQM) && !(msr & MSR_CMDBSY) && !(msr & MSR_DIO)) {
            sane = 1;
            break;
        }
    }
    if (!sane)
        return -1;                           /* never became ready: treat as absent */

    /* After a reset the controller pulses its interrupt; the standard recovery
     * is four SENSE INTERRUPT STATUS commands (one per the 4 possible drives) to
     * clear the polling state. */
    for (int i = 0; i < 4; i++) {
        uint8_t st0, cyl;
        if (fdc_sense_interrupt(&st0, &cyl) < 0)
            return -1;
    }

    /* Data rate: 500 kbps for a 1.44 MB diskette -> CCR = 0. */
    outb(FDC_CCR, 0x00);

    /* SPECIFY: step-rate / head-unload / head-load / non-DMA. The classic
     * 1.44 MB values: SRT=8 (=> byte1 0xDF with HUT=0xF) and HLT=2 with ND=0
     * (=> byte2 0x02). These are the textbook constants every FDC driver uses. */
    if (fdc_write(CMD_SPECIFY) < 0) return -1;
    if (fdc_write(0xDF) < 0)        return -1;   /* SRT=D, HUT=F */
    if (fdc_write(0x02) < 0)        return -1;   /* HLT=1, ND=0 (DMA mode) */

    /* Spin the motor before the recalibrate seek so the head can move reliably. */
    floppy_motor_on();

    /* RECALIBRATE: seek the head to cylinder 0. May need more than one pass on a
     * head that started far out (recalibrate steps at most 79 tracks); try twice
     * and confirm via SENSE INTERRUPT that ST0 reports a normal seek-end to
     * cylinder 0. */
    int recal_ok = 0;
    for (int attempt = 0; attempt < 2 && !recal_ok; attempt++) {
        if (fdc_write(CMD_RECALIBRATE) < 0) return -1;
        if (fdc_write(DOR_DSEL0) < 0)       return -1;   /* drive 0 */
        /* RECALIBRATE has no result phase; it completes by raising the interrupt.
         * Poll until the controller is no longer busy, then SENSE INTERRUPT. */
        int busy = 1;
        for (int i = 0; i < 1000000 && busy; i++)
            if (!(inb(FDC_MSR) & MSR_CMDBSY))
                busy = 0;
        if (busy)
            return -1;                       /* never finished: bail (no hang) */
        uint8_t st0, cyl;
        if (fdc_sense_interrupt(&st0, &cyl) < 0)
            return -1;
        /* A good recalibrate ends with the seek-end / track-0 status and cyl 0. */
        if (cyl == 0 && (st0 & ST0_IC_MASK) == ST0_IC_NORMAL)
            recal_ok = 1;
    }
    if (!recal_ok)
        return -1;
    return 0;
}

/* Seek the head to `cyl` on `head` and confirm via SENSE INTERRUPT. */
static int floppy_seek(uint8_t cyl, uint8_t head) {
    if (fdc_write(CMD_SEEK) < 0)                          return -1;
    if (fdc_write((uint8_t)((head << 2) | DOR_DSEL0)) < 0) return -1;  /* head + drive 0 */
    if (fdc_write(cyl) < 0)                               return -1;

    int busy = 1;
    for (int i = 0; i < 1000000 && busy; i++)
        if (!(inb(FDC_MSR) & MSR_CMDBSY))
            busy = 0;
    if (busy)
        return -1;

    uint8_t st0, got;
    if (fdc_sense_interrupt(&st0, &got) < 0)
        return -1;
    if (got != cyl || (st0 & ST0_IC_MASK) != ST0_IC_NORMAL)
        return -1;
    /* Head-settle time after a seek (~15 ms is ample for QEMU and real drives). */
    timer_wait(2);
    return 0;
}

/* ---- public API ---------------------------------------------------------- */

int floppy_init(void) {
    if (g_probed)
        return g_present ? 0 : -1;
    g_probed = 1;
    g_present = 0;

    /* Resolve the reserved .lowbss bounce buffer's PHYSICAL address (the kernel
     * is identity-mapped in the low 1 GiB, so phys == virt — vmm_translate is the
     * correct, general way to get it). ASSERT the two ISA-DMA constraints: the
     * 8237 addresses only the low 16 MiB, and a single transfer must not cross a
     * 64 KiB boundary. .lowbss + page alignment guarantee both, but if the layout
     * ever changed to violate them we cleanly disable the floppy rather than
     * program a DMA that would wrap or address the wrong RAM. */
    uint64_t phys = vmm_translate((uint64_t)(uintptr_t)g_bounce);
    if (!phys)
        phys = (uint64_t)(uintptr_t)g_bounce;   /* identity-map fallback */
    if (phys > 0x00FFFFFFull) {
        /* The buffer ended up above 16 MiB — ISA DMA can't reach it. Disable. */
        return -1;
    }
    if (((phys & 0xFFFF) + (PAGE_SIZE - 1)) > 0xFFFF) {   /* crosses a 64 KiB window */
        return -1;
    }
    g_bounce_phys = phys;
    memset(g_bounce, 0, PAGE_SIZE);

    /* Keep IRQ6 masked: we poll. pic_init() already masked everything at boot, so
     * this is belt-and-braces to guarantee a stray FDC interrupt can never fire. */
    pic_mask(6);

    if (floppy_reset() < 0) {
        /* No controller / bring-up failed: clean no-op. Drop the motor; the
         * static bounce buffer needs no freeing. */
        floppy_motor_off();
        g_bounce_phys = 0;
        return -1;
    }

    /* The controller reset + recalibrated, but that does NOT mean a readable
     * diskette is in the drive — on QEMU the FDC is always present even with no
     * `-drive if=floppy`. Probe by actually reading sector 0: if it fails, there
     * is a controller but no usable medium, which we report as NOT present so
     * floppy_selftest stays a clean no-op (and other drivers' tests don't trip on
     * a spurious "READ FAILED"). g_present is set first so floppy_read() runs the
     * probe, then cleared on failure. */
    g_present = 1;
    {
        uint8_t probe[FLOPPY_SECTOR_SIZE];
        if (floppy_read(0, 1, probe) != 0) {
            g_present = 0;
            floppy_motor_off();
            g_bounce_phys = 0;
            return -1;                       /* controller present, no readable diskette */
        }
    }
    return 0;
}

int floppy_present(void) {
    return g_present;
}

/* Read one FDC command's worth of sectors (<= one track, never crossing a track)
 * into the bounce buffer, then copy to `dst`. `lba`..`lba+n` must lie within a
 * single track. Returns 0 on success, -1 on error. */
static int floppy_read_chunk(uint32_t lba, uint32_t n, uint8_t *dst) {
    uint8_t cyl, head, sect;
    lba_to_chs(lba, &cyl, &head, &sect);

    floppy_motor_on();
    if (floppy_seek(cyl, head) < 0)
        return -1;

    uint32_t bytes = n * FLOPPY_SECTOR_SIZE;

    /* Retry the read a couple of times: the first access to a freshly-seeked
     * track occasionally reports an overrun/CRC blip on real hardware; a re-read
     * almost always succeeds. Each attempt re-arms the DMA. */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (dma_prepare(bytes, 0) < 0)       /* arm 8237 channel 2 for a disk read */
            return -1;

        /* Issue READ DATA: command byte (with MT|MFM|SK), then the 8 parameters:
         *   head/drive, cylinder, head, start sector, bytes/sector code (2=512),
         *   end-of-track sector, gap length (0x1B for 1.44 MB), data length
         *   (0xFF when bytes/sector code is non-zero). */
        int ok = 1;
        ok = ok && fdc_write(CMD_READ_DATA | READ_FLAGS) == 0;
        ok = ok && fdc_write((uint8_t)((head << 2) | DOR_DSEL0)) == 0;
        ok = ok && fdc_write(cyl) == 0;
        ok = ok && fdc_write(head) == 0;
        ok = ok && fdc_write(sect) == 0;
        ok = ok && fdc_write(0x02) == 0;                       /* 512 B/sector */
        ok = ok && fdc_write((uint8_t)(sect + n - 1)) == 0;    /* last sector on this track */
        ok = ok && fdc_write(0x1B) == 0;                       /* GAP3 for 1.44 MB */
        ok = ok && fdc_write(0xFF) == 0;                       /* DTL (unused when N!=0) */
        if (!ok)
            return -1;

        /* The FDC streams the sectors to the 8237; when done it drops CMD-BSY and
         * presents 7 result bytes. Poll for the result phase (RQM=1, DIO=FDC->CPU)
         * with a finite timeout. */
        uint8_t st[7];
        int got = 0;
        int timed_out = 0;
        for (int i = 0; i < 7; i++) {
            if (fdc_read(&st[i]) < 0) { timed_out = 1; break; }
            got++;
        }
        if (timed_out || got != 7) {
            /* Drain any partial result so the next attempt starts clean. */
            for (int i = 0; i < 7; i++) {
                uint8_t junk;
                if (fdc_wait_rqm(MSR_DIO) < 0) break;
                junk = inb(FDC_FIFO); (void)junk;
            }
            continue;                        /* retry */
        }

        /* st[0]=ST0, st[1]=ST1, st[2]=ST2, st[3]=cyl, st[4]=head, st[5]=sect,
         * st[6]=bytes/sector. A normal read ends with ST0's interrupt-code = 00
         * and ST1/ST2 (error bits) all clear. */
        uint8_t st0 = st[0], st1 = st[1], st2 = st[2];
        if ((st0 & ST0_IC_MASK) == ST0_IC_NORMAL && st1 == 0 && st2 == 0) {
            memcpy(dst, g_bounce, bytes);    /* DMA landed it in the bounce buffer */
            return 0;
        }
        /* Otherwise retry (re-seek to be safe — head may have stepped). */
        if (floppy_seek(cyl, head) < 0)
            return -1;
    }
    return -1;                               /* all attempts failed */
}

int floppy_read(uint32_t lba, uint32_t count, void *buf) {
    if (!buf || count == 0)
        return -1;
    if (!g_probed)
        floppy_init();
    if (!g_present)
        return -1;
    /* Refuse any access past the end of the 2880-sector geometry. */
    if ((uint64_t)lba + count > FLOPPY_TOTAL_SECTORS)
        return -1;

    uint8_t *p = (uint8_t *)buf;
    while (count > 0) {
        /* Bound a single FDC command to (a) the bounce-buffer cap and (b) the
         * remainder of the current track — READ DATA must not cross a track. */
        uint32_t sect_in_track = lba % FLOPPY_SECTORS_TRACK;        /* 0-based */
        uint32_t track_left = FLOPPY_SECTORS_TRACK - sect_in_track;
        uint32_t chunk = count;
        if (chunk > FLOPPY_DMA_MAX_SECTORS) chunk = FLOPPY_DMA_MAX_SECTORS;
        if (chunk > track_left)             chunk = track_left;

        if (floppy_read_chunk(lba, chunk, p) < 0)
            return -1;

        lba   += chunk;
        count -= chunk;
        p     += chunk * FLOPPY_SECTOR_SIZE;
    }
    return 0;
}

/* ---- boot-time verification ---------------------------------------------- */

static uint8_t selftest_buf[FLOPPY_SECTOR_SIZE * 4];

void floppy_selftest(void) {
    if (floppy_init() != 0) {
        kprintf("[floppy] no floppy controller/diskette found "
                "(none attached; legacy ATA boot intact).\n\n");
        return;
    }

    kprintf("[ ok ] floppy 82077AA up: reset + recalibrate OK, motor spun, "
            "ISA-DMA ch2 bounce @phys 0x%x (boot stays on legacy ATA).\n",
            (unsigned)g_bounce_phys);

    /* Read a few sectors via ISA DMA and log the first bytes + an additive
     * checksum, so the read can be matched byte-for-byte against known on-disk
     * content (exactly like ahci_selftest / nvme_selftest). Sectors 0, 1, 2 are
     * the first track, head 0 — the simplest possible read. */
    int read_any = 0;
    for (uint32_t lba = 0; lba < 3; lba++) {
        if (floppy_read(lba, 1, selftest_buf) != 0) {
            kprintf("[floppy] sector %u: READ FAILED\n", lba);
            continue;
        }
        read_any = 1;
        uint32_t sum = 0;
        for (uint32_t i = 0; i < FLOPPY_SECTOR_SIZE; i++)
            sum += selftest_buf[i];
        kprintf("[floppy] sector %u sum=%08x first16=", lba, sum);
        for (int i = 0; i < 16; i++)
            kprintf("%02x ", selftest_buf[i]);
        kprintf(" |");
        for (int i = 0; i < 16; i++) {
            uint8_t c = selftest_buf[i];
            kprintf("%c", (c >= 0x20 && c < 0x7F) ? (char)c : '.');
        }
        kprintf("|\n");
    }

    /* A multi-sector read in one call (exercises the chunk/track-cap loop): read
     * sectors 5..8 (4 sectors) and checksum the lot. */
    if (floppy_read(5, 4, selftest_buf) == 0) {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < FLOPPY_SECTOR_SIZE * 4; i++)
            sum += selftest_buf[i];
        kprintf("[floppy] sectors 5..8 (4-sector read) sum=%08x\n", sum);
    }

    floppy_motor_off();   /* done — stop the motor */

    if (read_any)
        kprintf("[ ok ] floppy ISA-DMA read self-test complete "
                "(bytes above are the real diskette content).\n\n");
    else
        kprintf("[floppy] read self-test: no sectors read (see above).\n\n");
}
