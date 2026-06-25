/*
 * msi.h — PCIe MSI / MSI-X message-signaled interrupts (M1288).
 *
 * A legacy PCI device raises an INTx pin wired (through the PIC / IOAPIC) to a
 * fixed interrupt line, shared with other devices. A modern PCIe device instead
 * *signals* an interrupt by performing an ordinary memory WRITE to a magic
 * address in the 0xFEE00000 range; the chipset/Local-APIC decodes that write as
 * "deliver interrupt vector N to CPU with APIC id D". No shared lines, a
 * private vector per source, and the only model multi-queue NICs / NVMe /
 * virtio actually use.
 *
 * This is the from-scratch MSI-X path: find a device's MSI-X capability, map
 * its in-BAR vector table, and route one table entry to a dynamically
 * allocated CPU vector delivered to the boot CPU's LAPIC. The handler runs from
 * isr_dispatch's MSI branch, which acks the LAPIC (MSI bypasses the PIC).
 *
 * The dispatch + per-vector handler registry live in kernel/interrupts.c
 * (msi_install_handler / msi_irq_count, declared in interrupts.h); this file is
 * the PCI-side programming.
 */
#pragma once
#include <stdint.h>
#include "pci.h"
#include "interrupts.h"   /* irq_handler_fn, struct registers, msi_irq_count */

/* The contiguous block of x86 interrupt vectors reserved for MSI/MSI-X
 * delivery. MUST match the stub block in kernel/asm/isr_stubs.asm. 0x90..0x9F. */
#define MSI_VEC_BASE   144
#define MSI_VEC_COUNT  16

/* x86 MSI message encoding (Intel SDM vol 3, "Message Address/Data Register").
 * Address: 0xFEE0_0000 | (destination APIC id << 12); physical mode, no
 * redirection. Data: low 8 bits = vector; fixed delivery + edge trigger = 0. */
#define MSI_ADDR_BASE  0xFEE00000u

/* True (1) if `dev` exposes an MSI-X capability (cap id 0x11). If `table_size`
 * is non-NULL it receives the number of table entries. */
int  msi_x_available(const pci_device_t *dev, int *table_size);

/* Claim a free MSI vector and install `handler` (NULL is allowed — delivery is
 * still tallied) for it. Returns the x86 vector (MSI_VEC_BASE..), or -1 if the
 * pool is exhausted. */
int  msi_alloc_vector(irq_handler_fn handler);

/* Program MSI-X table `entry` of `dev` to deliver `vector` to the boot CPU,
 * unmask that entry, and set the device's global MSI-X Enable. Returns 0, or -1
 * (no MSI-X cap / entry out of range / unmappable table BAR). */
int  msi_x_route(const pci_device_t *dev, int entry, uint8_t vector);
