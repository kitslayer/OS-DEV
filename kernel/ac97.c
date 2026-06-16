/*
 * ac97.c — Intel 82801AA AC'97 audio (the QEMU `-device AC97`).
 *
 * The chip exposes two port-I/O register windows via PCI BARs: NAM (BAR0, the
 * AC'97 mixer/codec) and NABM (BAR1, the bus-master DMA engine). PCM output is
 * driven by a Buffer Descriptor List (BDL): an array of up to 32 entries, each
 * { physical buffer address, sample count, control }. We hand the engine a list
 * of plain RAM frames (whose physical address doubles as a virtual one via the
 * identity map, like the e1000 rings), point it at the codec, and it DMAs the
 * 16-bit-stereo-48 kHz samples out.
 *
 * This first cut plays a finite buffer and blocks until it has drained — enough
 * for a `tone` command and a building block for the DOOM mixer (which will stream
 * a ring instead).
 */
#include "ac97.h"
#include "pci.h"
#include "io.h"
#include "pmm.h"
#include "timer.h"
#include "console.h"
#include "kheap.h"
#include "string.h"
#include <stdint.h>
#include <stddef.h>

/* NAM (mixer) registers — 16-bit, relative to BAR0 */
#define NAM_RESET    0x00
#define NAM_MASTER   0x02      /* master volume     */
#define NAM_PCM_OUT  0x18      /* PCM-out volume     */

/* NABM (bus master) registers — the PCM-Out box lives at offset 0x10 of BAR1 */
#define PO_BDBAR     0x10      /* 32-bit: BDL physical base        */
#define PO_CIV       0x14      /*  8-bit: current buffer index (RO) */
#define PO_LVI       0x15      /*  8-bit: last valid buffer index   */
#define PO_SR        0x16      /* 16-bit: status                    */
#define PO_PICB      0x18      /* 16-bit: samples left in cur buffer*/
#define PO_CR        0x1B      /*  8-bit: control                   */

#define CR_RPBM      0x01      /* run (1) / pause (0) bus master    */
#define CR_RR        0x02      /* reset this DMA engine             */
#define SR_DCH       0x01      /* DMA controller halted             */

#define NUM_BUF      32                 /* BDL entries */
#define BUF_FRAMES   (PAGE_SIZE / 4)    /* 16-bit stereo frames per 4 KiB buffer = 1024 */

typedef struct {
    uint32_t addr;     /* physical address of the sample buffer */
    uint16_t samples;  /* number of 16-bit samples (= frames * 2) */
    uint16_t ctrl;     /* bit15 IOC, bit14 BUP */
} __attribute__((packed)) bdl_entry_t;

static uint16_t     nam, nabm;             /* I/O port bases */
static bdl_entry_t *bdl;                   /* 32 entries (one frame) */
static uint64_t     bdl_phys;
static uint64_t     buf_phys[NUM_BUF];     /* the sample-buffer pool */
static int          inited;

int ac97_ready(void) { return inited; }

int ac97_init(void) {
    pci_device_t d = pci_find(0x8086, 0x2415);   /* 82801AA AC'97 */
    if (!d.valid) { kprintf("[ac97] no device\n"); return -1; }

    /* enable I/O space (bit0) + bus master (bit2) in the PCI command register */
    uint32_t cmd = pci_read32(d.bus, d.slot, d.func, 0x04);
    pci_write32(d.bus, d.slot, d.func, 0x04, cmd | 0x05);

    nam  = (uint16_t)pci_bar(&d, 0);
    nabm = (uint16_t)pci_bar(&d, 1);

    outw(nam + NAM_RESET, 1);          /* reset the mixer */
    outw(nam + NAM_MASTER, 0x0000);    /* 0x0000 = full volume, unmuted */
    outw(nam + NAM_PCM_OUT, 0x0000);

    outb(nabm + PO_CR, CR_RR);                       /* reset the PCM-out engine */
    for (int i = 0; i < 100000 && (inb(nabm + PO_CR) & CR_RR); i++) { }

    bdl_phys = pmm_alloc_frame();
    bdl = (bdl_entry_t *)(uintptr_t)bdl_phys;
    memset(bdl, 0, NUM_BUF * sizeof(bdl_entry_t));
    for (int i = 0; i < NUM_BUF; i++) buf_phys[i] = pmm_alloc_frame();   /* 128 KiB pool */
    outl(nabm + PO_BDBAR, (uint32_t)bdl_phys);

    inited = 1;
    kprintf("[ ok ] AC'97 audio: NAM=%x NABM=%x\n", nam, nabm);
    return 0;
}

void ac97_play(const int16_t *frames, int nframes) {
    if (!inited || nframes <= 0) return;

    /* Fill the buffer pool + BDL, at most NUM_BUF * BUF_FRAMES frames per batch. */
    int total = nframes, pos = 0;
    while (pos < total) {
        int batch_start = pos, n = 0;
        for (; n < NUM_BUF && pos < total; n++) {
            int chunk = total - pos;
            if (chunk > (int)BUF_FRAMES) chunk = BUF_FRAMES;
            memcpy((int16_t *)(uintptr_t)buf_phys[n], frames + (size_t)pos * 2, (size_t)chunk * 4);
            bdl[n].addr    = (uint32_t)buf_phys[n];
            bdl[n].samples = (uint16_t)(chunk * 2);   /* 2 samples per stereo frame */
            bdl[n].ctrl    = 0;
            pos += chunk;
        }

        outl(nabm + PO_BDBAR, (uint32_t)bdl_phys);
        outb(nabm + PO_LVI, (uint8_t)(n - 1));        /* play entries 0..n-1 */
        outb(nabm + PO_CR, CR_RPBM);                  /* run */

        /* Block for this batch's duration at 48 kHz, then confirm it halted.
         * The syscall path sets IF=1 so timer_wait's hlt can wake. */
        int ms = (pos - batch_start) * 1000 / 48000 + 30;   /* + margin */
        timer_wait((uint64_t)ms / 10 + 1);

        for (int i = 0; i < 200000 && !(inb(nabm + PO_SR) & SR_DCH); i++) { }
        outb(nabm + PO_CR, 0);                        /* stop */
    }
}

/* little-endian helpers over a byte buffer */
static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1]<<8); }

int ac97_play_wav(const uint8_t *d, int len) {
    if (!inited || len < 44) return -1;
    if (d[0]!='R'||d[1]!='I'||d[2]!='F'||d[3]!='F'||d[8]!='W'||d[9]!='A'||d[10]!='V'||d[11]!='E')
        return -1;

    int channels = 0, rate = 0, bits = 0;
    const uint8_t *pcm = 0; long pcmlen = 0;
    int off = 12;
    while (off + 8 <= len) {                          /* walk the RIFF chunks */
        const uint8_t *id = d + off;
        uint32_t csz = rd32(d + off + 4);
        const uint8_t *body = d + off + 8;
        if (off + 8 + (long)csz > len) csz = (uint32_t)(len - (off + 8));   /* clamp to buffer */
        if (id[0]=='f'&&id[1]=='m'&&id[2]=='t'&&id[3]==' ' && csz >= 16) {
            channels = rd16(body + 2); rate = (int)rd32(body + 4); bits = rd16(body + 14);
        } else if (id[0]=='d'&&id[1]=='a'&&id[2]=='t'&&id[3]=='a') {
            pcm = body; pcmlen = csz;
        }
        off += 8 + csz + (csz & 1);                   /* chunks are word-aligned */
    }
    if (!pcm || bits != 16 || channels < 1 || channels > 2 || rate <= 0) return -1;

    long in_frames = pcmlen / (2 * channels);
    const int16_t *in = (const int16_t *)pcm;
    long out_frames = in_frames * 48000 / rate;       /* resample to the AC'97's 48 kHz */
    if (out_frames <= 0) return -1;
    int16_t *out = kmalloc((size_t)out_frames * 4);
    if (!out) return -1;
    for (long i = 0; i < out_frames; i++) {
        long si = i * rate / 48000;                   /* nearest-neighbour */
        if (si >= in_frames) si = in_frames - 1;
        if (channels == 2) { out[i*2] = in[si*2]; out[i*2+1] = in[si*2+1]; }
        else               { out[i*2] = out[i*2+1] = in[si]; }
    }
    ac97_play(out, (int)out_frames);
    kfree(out);
    return 0;
}
