/* ioapic.h — I/O APIC interrupt routing (M1856, completed M1890). See ioapic.c.
 * Entries start masked; each IRQ is routed over from the 8259 PIC explicitly
 * (see irq_route_ioapic / irq_route_ioapic_pci in interrupts.h). As of M1890 the
 * PIT, keyboard, serial, PS/2 mouse and the NIC's PCI line are all delivered
 * here, with polarity/trigger taken from the ACPI MADT rather than assumed. */
#ifndef IOAPIC_H
#define IOAPIC_H
#include <stdint.h>

void     ioapic_init(void);        /* locate + map the I/O APIC (no-op if absent) */
int      ioapic_present(void);
uint32_t ioapic_gsi_base(void);
uint32_t ioapic_num_redir(void);
/* Route GSI `gsi` -> `vector` on local APIC `apic_id` (fixed/physical/edge), (un)masked. */
void     ioapic_route(uint8_t gsi, uint8_t vector, uint8_t apic_id, int masked);
/* As ioapic_route, but with an explicit electrical configuration (M1890):
 * `active_low` sets polarity (RTE bit 13) and `level` sets level-triggered
 * delivery (RTE bit 15). PCI interrupt lines REQUIRE level + active-low; ISA
 * lines default to edge + active-high unless a MADT override says otherwise. */
void     ioapic_route_ex(uint8_t gsi, uint8_t vector, uint8_t apic_id, int masked,
                         int active_low, int level);
void     ioapic_mask(uint8_t gsi);
void     ioapic_unmask(uint8_t gsi);

#endif /* IOAPIC_H */
