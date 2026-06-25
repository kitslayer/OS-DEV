/*
 * msi.c — PCIe MSI-X message-signaled interrupt programming (M1288).
 *
 * See kernel/include/msi.h for the model. In brief: an MSI-X-capable PCI device
 * carries a capability (id 0x11) that points at a "vector table" living in one
 * of its memory BARs. Each 16-byte table entry holds a message address, message
 * data, and a mask bit. When the device wants to interrupt for that entry it
 * writes `data` to `address` as a normal memory transaction; the Local APIC
 * decodes 0xFEE00000-range writes and raises the encoded vector on the encoded
 * CPU. We:
 *
 *   1. walk the capability list (pci_find_cap) to the MSI-X capability;
 *   2. read its table BIR + offset, map that BAR page (uncached MMIO);
 *   3. write entry: addr = 0xFEE00000 | (boot APIC id << 12), data = vector,
 *      vector-control = 0 (unmasked);
 *   4. set MSI-X Enable in the capability's Message Control word.
 *
 * The vector itself comes from msi_alloc_vector(), which hands out one of the
 * reserved MSI vectors (kernel/asm/isr_stubs.asm) and registers the handler
 * with the dispatcher in kernel/interrupts.c. Purely additive: nothing routes a
 * device here unless its driver opts in (see kernel/virtio_rng.c).
 */
#include "msi.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"          /* PAGE_SIZE */
#include "smp.h"          /* smp_current_cpu -> destination APIC id */
#include "interrupts.h"   /* msi_install_handler */

/* PCI MSI-X capability layout (offsets from the capability's config offset). */
#define MSIX_MSG_CTRL   0x02   /* u16: bit15 Enable, bit14 Func Mask, b10:0 size-1 */
#define MSIX_TABLE_OFF  0x04   /* u32: bits2:0 = BAR index (BIR), bits31:3 = offset */
#define MSIX_CTRL_ENABLE   (1u << 15)
#define MSIX_CTRL_FUNCMASK (1u << 14)
#define MSIX_CAP_ID     0x11

/* MSI-X table entry (16 bytes), as four little-endian dwords in the BAR. */
#define MSIX_ENT_ADDR_LO  0
#define MSIX_ENT_ADDR_HI  1
#define MSIX_ENT_DATA     2
#define MSIX_ENT_VCTRL    3
#define MSIX_VCTRL_MASK   (1u << 0)

/* Linear allocator over the reserved MSI vector block. */
static int next_vec = MSI_VEC_BASE;

int msi_alloc_vector(irq_handler_fn handler) {
    if (next_vec >= MSI_VEC_BASE + MSI_VEC_COUNT)
        return -1;                       /* pool exhausted */
    int v = next_vec++;
    msi_install_handler((uint8_t)v, handler);
    return v;
}

int msi_x_available(const pci_device_t *dev, int *table_size) {
    uint8_t cap = pci_find_cap(dev, MSIX_CAP_ID);
    if (!cap)
        return 0;
    if (table_size) {
        uint16_t mc = pci_read16(dev->bus, dev->slot, dev->func, cap + MSIX_MSG_CTRL);
        *table_size = (mc & 0x7FF) + 1;
    }
    return 1;
}

int msi_x_route(const pci_device_t *dev, int entry, uint8_t vector) {
    uint8_t cap = pci_find_cap(dev, MSIX_CAP_ID);
    if (!cap)
        return -1;

    uint16_t mc = pci_read16(dev->bus, dev->slot, dev->func, cap + MSIX_MSG_CTRL);
    int tsize = (mc & 0x7FF) + 1;
    if (entry < 0 || entry >= tsize)
        return -1;

    /* Locate the vector table: which BAR (BIR) + byte offset within it. */
    uint32_t toff = pci_read32(dev->bus, dev->slot, dev->func, cap + MSIX_TABLE_OFF);
    int      bir  = toff & 0x7;
    uint32_t off  = toff & ~0x7u;
    uint64_t bar  = pci_bar(dev, bir);    /* 32-bit BAR base; QEMU maps these <4GiB */
    if (!bar)
        return -1;

    /* Map the page(s) holding our entry as uncached MMIO (identity), the way the
     * other MMIO drivers map their register BARs. One spillover page covers an
     * entry that straddles a page boundary. */
    uint64_t ent_phys = bar + off + (uint64_t)entry * 16;
    uint64_t page     = ent_phys & ~(uint64_t)(PAGE_SIZE - 1);
    vmm_map(page, page, PTE_WRITABLE | PTE_PCD);
    vmm_map(page + PAGE_SIZE, page + PAGE_SIZE, PTE_WRITABLE | PTE_PCD);

    volatile uint32_t *te = (volatile uint32_t *)(uintptr_t)ent_phys;
    uint32_t apic = (uint32_t)smp_current_cpu();    /* deliver to the boot CPU */

    /* Configure the entry while it is still masked (vctrl reset has mask=0, but
     * the device cannot signal until we set the global Enable below, so the
     * device is quiescent here regardless). */
    te[MSIX_ENT_ADDR_LO] = MSI_ADDR_BASE | (apic << 12);
    te[MSIX_ENT_ADDR_HI] = 0;
    te[MSIX_ENT_DATA]    = vector;        /* fixed delivery, edge: data == vector */
    __asm__ volatile("" ::: "memory");
    te[MSIX_ENT_VCTRL]   = 0;             /* clear bit0 => UNMASK this entry */
    __asm__ volatile("" ::: "memory");

    /* Globally enable MSI-X (bit15) and clear the function-wide mask (bit14).
     * The table-size bits are read-only, so writing them back is harmless. */
    mc = (uint16_t)((mc | MSIX_CTRL_ENABLE) & ~MSIX_CTRL_FUNCMASK);
    pci_write16(dev->bus, dev->slot, dev->func, cap + MSIX_MSG_CTRL, mc);
    return 0;
}
