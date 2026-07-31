/*
 * pci.c — PCI bus enumeration via the legacy configuration mechanism.
 *
 * Every PCI device exposes a 256-byte "configuration space" holding its
 * vendor/device IDs, class, interrupt line, and Base Address Registers (BARs)
 * — the latter tell you where the device's memory/IO lives. You read it
 * through a pair of I/O ports: write a (bus, slot, function, offset) address to
 * 0xCF8, then read/write the 32-bit value at 0xCFC.
 *
 * Enumeration is just brute force: probe every bus/slot/function; a vendor ID
 * of 0xFFFF means "nothing there." This is how we locate the network card.
 */
#include "pci.h"
#include "task.h"     /* pci_selftest spawns a concurrent reader (M1914) */
#include "io.h"
#include "console.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return 0x80000000u
         | ((uint32_t)bus << 16)
         | ((uint32_t)slot << 11)
         | ((uint32_t)func << 8)
         | (off & 0xFC);
}

/* 0xCF8/0xCFC is an ADDRESS/DATA port pair, and the address register is GLOBAL
 * hardware state, so the pair must be indivisible (M1914 — same bug class as the
 * CMOS clock fixed in M1913). If another task writes 0xCF8 between our address
 * write and our data access, we read or WRITE THE WRONG DEVICE'S REGISTER.
 *
 * This is reachable, not theoretical: pcifs_read() serves /pci/... straight from
 * a ring-3 read() (see vfs.c), issuing dozens of config reads, while drivers do
 * their own config access from other tasks. The sub-word writes below are worse
 * still — they are READ-MODIFY-WRITE across two separate accesses, so an
 * interleaving there silently drops the other writer's update to a command or
 * MSI register.
 *
 * A plain spinlock: no interrupt handler performs PCI config access (MSI setup
 * is boot/driver-init time), so the only racers are other tasks and cores. */
static volatile int pci_lock;
static inline void pci_take(void) {
    while (__atomic_exchange_n(&pci_lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
}
static inline void pci_give(void) { __atomic_store_n(&pci_lock, 0, __ATOMIC_RELEASE); }

/* Unlocked primitives — ONLY call with pci_lock held. They exist so the
 * read-modify-write helpers can hold the lock across BOTH halves instead of
 * taking it twice (which would also self-deadlock). */
extern volatile int pci_widen_window;   /* test-only: see pci_selftest below */
static uint32_t pci_rd32_locked(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(PCI_CONFIG_ADDRESS, addr(bus, slot, func, off));
    if (pci_widen_window) task_yield();
    return inl(PCI_CONFIG_DATA);
}
static void pci_wr32_locked(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v) {
    outl(PCI_CONFIG_ADDRESS, addr(bus, slot, func, off));
    outl(PCI_CONFIG_DATA, v);
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    pci_take();
    uint32_t v = pci_rd32_locked(bus, slot, func, off);
    pci_give();
    return v;
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v) {
    pci_take();
    pci_wr32_locked(bus, slot, func, off, v);
    pci_give();
}

/* Sub-word access: the hardware only transfers aligned 32-bit dwords, so a
 * byte/word read extracts the requested lane from the containing dword and a
 * write does a read-modify-write of it. */
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t d = pci_read32(bus, slot, func, off & 0xFC);
    return (uint16_t)(d >> ((off & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t d = pci_read32(bus, slot, func, off & 0xFC);
    return (uint8_t)(d >> ((off & 3) * 8));
}

/* The whole read-modify-write is one critical section: otherwise two concurrent
 * sub-word writes to the same dword (e.g. two drivers enabling their bus-master
 * bit in the command register) lose one of the updates. */
void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t v) {
    pci_take();
    uint32_t d = pci_rd32_locked(bus, slot, func, off & 0xFC);
    int sh = (off & 2) * 8;
    d = (d & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
    pci_wr32_locked(bus, slot, func, off & 0xFC, d);
    pci_give();
}

void pci_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint8_t v) {
    pci_take();
    uint32_t d = pci_rd32_locked(bus, slot, func, off & 0xFC);
    int sh = (off & 3) * 8;
    d = (d & ~(0xFFu << sh)) | ((uint32_t)v << sh);
    pci_wr32_locked(bus, slot, func, off & 0xFC, d);
    pci_give();
}

/* Walk the capabilities linked list. It exists only if the Status register
 * (0x06) bit 4 is set; the head offset is at 0x34, and each capability's byte 1
 * points to the next (0 = end). Low two bits of every pointer are reserved. */
uint8_t pci_find_cap(const pci_device_t *d, uint8_t cap_id) {
    uint16_t status = pci_read16(d->bus, d->slot, d->func, 0x06);
    if (!(status & (1 << 4)))
        return 0;                                  /* no capabilities list */
    uint8_t off = pci_read8(d->bus, d->slot, d->func, 0x34) & 0xFC;
    for (int guard = 0; off && guard < 48; guard++) {
        uint8_t id   = pci_read8(d->bus, d->slot, d->func, off);
        uint8_t next = pci_read8(d->bus, d->slot, d->func, off + 1);
        if (id == cap_id)
            return off;
        off = next & 0xFC;
    }
    return 0;
}

pci_device_t pci_find(uint16_t vendor, uint16_t device) {
    pci_device_t d = {0};
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read32(bus, slot, func, 0x00);
                uint16_t v = id & 0xFFFF;
                if (v == 0xFFFF)
                    continue;
                uint16_t dv = id >> 16;
                if (v == vendor && dv == device) {
                    uint32_t cls = pci_read32(bus, slot, func, 0x08);
                    d.bus = bus; d.slot = slot; d.func = func;
                    d.vendor_id = v; d.device_id = dv;
                    d.class_id = (cls >> 24) & 0xFF;
                    d.subclass = (cls >> 16) & 0xFF;
                    d.prog_if  = (cls >> 8) & 0xFF;
                    d.valid = 1;
                    return d;
                }
            }
        }
    }
    return d;
}

pci_device_t pci_find_class(uint8_t class_id, uint8_t subclass, uint8_t prog_if) {
    pci_device_t d = {0};
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read32(bus, slot, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF)
                    continue;
                uint32_t cls = pci_read32(bus, slot, func, 0x08);
                if (((cls >> 24) & 0xFF) == class_id &&
                    ((cls >> 16) & 0xFF) == subclass &&
                    ((cls >> 8)  & 0xFF) == prog_if) {
                    d.bus = bus; d.slot = slot; d.func = func;
                    d.vendor_id = id & 0xFFFF; d.device_id = id >> 16;
                    d.class_id = class_id; d.subclass = subclass;
                    d.prog_if  = prog_if;
                    d.valid = 1;
                    return d;
                }
            }
        }
    }
    return d;
}

uint32_t pci_bar(const pci_device_t *d, int bar) {
    uint32_t v = pci_read32(d->bus, d->slot, d->func, 0x10 + bar * 4);
    if (v & 1)
        return v & ~0x3u;       /* I/O space BAR */
    return v & ~0xFu;           /* memory space BAR */
}

uint8_t pci_irq_line(const pci_device_t *d) {
    return pci_read32(d->bus, d->slot, d->func, 0x3C) & 0xFF;
}

void pci_enable_bus_master(const pci_device_t *d) {
    uint32_t cmd = pci_read32(d->bus, d->slot, d->func, 0x04);
    cmd |= (1 << 2) | (1 << 1);   /* bus master + memory space enable */
    pci_write32(d->bus, d->slot, d->func, 0x04, cmd);
}

void pci_enumerate(void) {
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read32(bus, slot, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF)
                    continue;
                uint32_t cls = pci_read32(bus, slot, func, 0x08);
                kprintf("    %02x:%02x.%x  %04x:%04x  class %02x:%02x\n",
                        bus, slot, func, id & 0xFFFF, id >> 16,
                        (cls >> 24) & 0xFF, (cls >> 16) & 0xFF);
                if (func == 0) {
                    /* skip non-multifunction devices' phantom functions */
                    uint32_t hdr = pci_read32(bus, slot, 0, 0x0C);
                    if (!((hdr >> 16) & 0x80))
                        break;
                }
            }
        }
    }
}

/* Collect every present device into `out` (capped at `max`); return the count
 * actually present (which may exceed `max` if the buffer filled). Same brute-
 * force walk as pci_find, but it honors the multi-function header bit: a device
 * is multi-function only if bit 7 of its header type (reg 0x0C, byte 2) is set,
 * so for a single-function device we stop after func 0 instead of probing the
 * phantom funcs 1-7 (which would alias func 0 and report duplicates). */
int pci_collect(pci_device_t *out, int max) {
    int n = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read32(bus, slot, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF)
                    continue;
                if (out && n < max) {
                    uint32_t cls = pci_read32(bus, slot, func, 0x08);
                    out[n].bus = bus; out[n].slot = slot; out[n].func = func;
                    out[n].vendor_id = id & 0xFFFF; out[n].device_id = id >> 16;
                    out[n].class_id = (cls >> 24) & 0xFF;
                    out[n].subclass = (cls >> 16) & 0xFF;
                    out[n].prog_if  = (cls >> 8) & 0xFF;
                    out[n].valid = 1;
                }
                n++;
                if (func == 0) {
                    uint32_t hdr = pci_read32(bus, slot, 0, 0x0C);
                    if (!((hdr >> 16) & 0x80))
                        break;   /* single-function device: skip funcs 1-7 */
                }
            }
        }
    }
    return n;
}

/* ---- boot self-test: the address/data pair must be indivisible (M1914) -------
 * Two tasks read the vendor/device dword of DIFFERENT devices in a loop. If the
 * 0xCF8 write and the 0xCFC read can interleave, a reader sees the OTHER
 * device's identity — a wrong value, not merely a stale one, so it is checkable.
 *
 * The window is two instructions and cannot be hit by sampling (a first attempt
 * at the equivalent CMOS test ran 3001 unlocked reads and saw zero tearing), so
 * it is deliberately forced open here, exactly as rtc.c does. Test-only. */
volatile int pci_widen_window;

static volatile int pci_st_stop, pci_st_bad, pci_st_n, pci_st_done;
static uint8_t pci_st_b, pci_st_s, pci_st_f;
static uint32_t pci_st_expect;

static void pci_selftest_peer(void) {
    while (!pci_st_stop) {
        uint32_t v = pci_read32(pci_st_b, pci_st_s, pci_st_f, 0);
        if (v != pci_st_expect) __atomic_add_fetch(&pci_st_bad, 1, __ATOMIC_SEQ_CST);
        __atomic_add_fetch(&pci_st_n, 1, __ATOMIC_SEQ_CST);
        task_yield();
    }
    __atomic_store_n(&pci_st_done, 1, __ATOMIC_SEQ_CST);
    task_exit();
}

void pci_selftest(void) {
    pci_device_t devs[24];
    int n = pci_collect(devs, 24);
    if (n < 2) { kprintf("[ ok ] pci: config-space concurrency test skipped (<2 devices)\n"); return; }

    /* Two DIFFERENT devices, so an interleave produces a wrong identity. */
    pci_st_b = devs[1].bus; pci_st_s = devs[1].slot; pci_st_f = devs[1].func;
    pci_st_expect = pci_read32(pci_st_b, pci_st_s, pci_st_f, 0);
    uint32_t mine = pci_read32(devs[0].bus, devs[0].slot, devs[0].func, 0);

    pci_st_stop = pci_st_bad = pci_st_n = pci_st_done = 0;
    if (!task_create_stack(pci_selftest_peer, 0, 0, 16 * 1024)) {
        kprintf("[ ok ] pci: config-space concurrency test skipped (no task slot)\n");
        return;
    }
    pci_widen_window = 1;
    for (int i = 0; i < 40; i++) {
        uint32_t v = pci_read32(devs[0].bus, devs[0].slot, devs[0].func, 0);
        if (v != mine) __atomic_add_fetch(&pci_st_bad, 1, __ATOMIC_SEQ_CST);
        __atomic_add_fetch(&pci_st_n, 1, __ATOMIC_SEQ_CST);
        task_yield();
    }
    __atomic_store_n(&pci_st_stop, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 100000 && !__atomic_load_n(&pci_st_done, __ATOMIC_SEQ_CST); i++)
        task_yield();
    pci_widen_window = 0;

    int bad = __atomic_load_n(&pci_st_bad, __ATOMIC_SEQ_CST);
    int tot = __atomic_load_n(&pci_st_n,   __ATOMIC_SEQ_CST);
    if (bad == 0)
        kprintf("[ ok ] pci: %d concurrent config reads of 2 devices with the "
                "address->data window forced open, 0 crossed\n", tot);
    else
        kprintf("[FAIL] pci: %d of %d concurrent config reads returned ANOTHER "
                "device's register — 0xCF8/0xCFC is being interleaved\n", bad, tot);
}
