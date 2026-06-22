/* pci.h — PCI bus enumeration and configuration access. */
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_id, subclass, prog_if;
    int      valid;
} pci_device_t;

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v);

/* Find the first device matching vendor:device. Result .valid == 0 if absent. */
pci_device_t pci_find(uint16_t vendor, uint16_t device);

/* Find the first device matching a class / subclass / programming-interface
 * triple (the top three bytes of the 0x08 register) — used to locate a device
 * by *function* rather than a known vendor:device id, e.g. an AHCI HBA
 * (0x01, 0x06, 0x01). Result .valid == 0 if none present. */
pci_device_t pci_find_class(uint8_t class_id, uint8_t subclass, uint8_t prog_if);

uint32_t pci_bar(const pci_device_t *d, int bar);  /* BAR address, flags masked off */
uint8_t  pci_irq_line(const pci_device_t *d);
void     pci_enable_bus_master(const pci_device_t *d);

void     pci_enumerate(void);   /* print every device (for inspection) */
