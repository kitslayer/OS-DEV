/*
 * swap.c — back anonymous pages with a disk. See swap.h. A bitmap of fixed
 * page-sized slots at the high end of a writable non-boot device; swap_out
 * writes a frame to a free slot, swap_in reads it back. The slot index lives in
 * the swapped page's not-present PTE (kernel/app.c handles the encoding + the
 * fault-in). Built on the uniform blockdev_read/write (M1095).
 */
#include "swap.h"
#include "blockdev.h"
#include "vmm.h"        /* hhdm() */
#include "pmm.h"        /* PAGE_SIZE */
#include "console.h"    /* kprintf */
#include <stdint.h>

#define SWAP_SLOTS    8192            /* 8192 pages = 32 MiB of swap */
#define SECT_PER_PAGE (PAGE_SIZE / 512)

static int      swap_dev = -1;        /* blockdev index, -1 = inactive */
static uint64_t swap_base;            /* first swap sector on that device */
static uint8_t  used[SWAP_SLOTS];
static uint64_t g_out, g_in, g_cur;

int swap_active(void) {
    if (swap_dev >= 0) return 1;
    int n = blockdev_count();
    for (int i = 0; i < n; i++) {
        blockdev_t *d = blockdev_get(i);
        if (!d || !d->name || !d->write) continue;             /* writable devices only */
        const char *nm = d->name;
        if (nm[0]=='a' && nm[1]=='t' && nm[2]=='a' && nm[3]=='0') continue;  /* never the boot disk */
        uint64_t need = (uint64_t)SWAP_SLOTS * SECT_PER_PAGE;
        if (d->sectors && d->sectors < need + 4096) continue;  /* too small */
        swap_base = d->sectors ? d->sectors - need : 200000;   /* high end (or a fixed high LBA) */
        swap_dev = i;
        kprintf("[swap] active on %s: %d slots (%d MiB) at lba %lu\n",
                nm, SWAP_SLOTS, SWAP_SLOTS * 4 / 1024, (unsigned long)swap_base);
        return 1;
    }
    return 0;
}

int swap_out(uint64_t phys) {
    if (!swap_active()) return -1;
    int s = -1;
    for (int i = 0; i < SWAP_SLOTS; i++) if (!used[i]) { s = i; break; }
    if (s < 0) return -1;                                      /* swap full */
    if (blockdev_write(swap_dev, swap_base + (uint64_t)s * SECT_PER_PAGE, SECT_PER_PAGE, hhdm(phys)) < 0)
        return -1;
    used[s] = 1; g_out++; g_cur++;
    return s;
}

int swap_in(int slot, uint64_t phys) {
    if (swap_dev < 0 || slot < 0 || slot >= SWAP_SLOTS || !used[slot]) return -1;
    if (blockdev_read(swap_dev, swap_base + (uint64_t)slot * SECT_PER_PAGE, SECT_PER_PAGE, hhdm(phys)) < 0)
        return -1;
    g_in++;
    return 0;
}

void swap_release(int slot) {
    if (slot >= 0 && slot < SWAP_SLOTS && used[slot]) { used[slot] = 0; g_cur--; }
}

static int sp_put(char *b, int p, int max, const char *s) { while (*s && p + 1 < max) b[p++] = *s++; return p; }
static int sp_num(char *b, int p, int max, uint64_t v) {
    char t[24]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p + 1 < max) b[p++] = t[--n];
    return p;
}

int swap_format(char *out, int max) {
    int p = 0;
    if (swap_dev < 0) { p = sp_put(out, p, max, "swap: inactive (no writable non-boot device)\n"); if (p < max) out[p] = 0; return p; }
    blockdev_t *d = blockdev_get(swap_dev);
    p = sp_put(out, p, max, "swap device:\t"); p = sp_put(out, p, max, d && d->name ? d->name : "?"); p = sp_put(out, p, max, "\n");
    p = sp_put(out, p, max, "Slots:\t");     p = sp_num(out, p, max, SWAP_SLOTS);          p = sp_put(out, p, max, " (32 MiB)\n");
    p = sp_put(out, p, max, "Used:\t");      p = sp_num(out, p, max, g_cur);               p = sp_put(out, p, max, " pages\n");
    p = sp_put(out, p, max, "PagesOut:\t");  p = sp_num(out, p, max, g_out);               p = sp_put(out, p, max, "\n");
    p = sp_put(out, p, max, "PagesIn:\t");   p = sp_num(out, p, max, g_in);                p = sp_put(out, p, max, "\n");
    if (p < max) out[p] = 0;
    return p;
}
