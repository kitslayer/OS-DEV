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

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(PCI_CONFIG_ADDRESS, addr(bus, slot, func, off));
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v) {
    outl(PCI_CONFIG_ADDRESS, addr(bus, slot, func, off));
    outl(PCI_CONFIG_DATA, v);
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
