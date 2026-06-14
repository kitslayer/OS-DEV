/* pci.h — PCI bus enumeration and configuration access. */
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_id, subclass;
    int      valid;
} pci_device_t;

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v);

/* Find the first device matching vendor:device. Result .valid == 0 if absent. */
pci_device_t pci_find(uint16_t vendor, uint16_t device);

uint32_t pci_bar(const pci_device_t *d, int bar);  /* BAR address, flags masked off */
uint8_t  pci_irq_line(const pci_device_t *d);
void     pci_enable_bus_master(const pci_device_t *d);

void     pci_enumerate(void);   /* print every device (for inspection) */
