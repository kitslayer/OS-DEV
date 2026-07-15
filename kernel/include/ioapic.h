/* ioapic.h — I/O APIC interrupt routing (M1856). See ioapic.c.
 * Additive foundation for interrupt-driven I/O; entries start masked, the 8259
 * PIC still owns live IRQs until a later step routes one over. */
#ifndef IOAPIC_H
#define IOAPIC_H
#include <stdint.h>

void     ioapic_init(void);        /* locate + map the I/O APIC (no-op if absent) */
int      ioapic_present(void);
uint32_t ioapic_gsi_base(void);
uint32_t ioapic_num_redir(void);
/* Route GSI `gsi` -> `vector` on local APIC `apic_id` (fixed/physical/edge), (un)masked. */
void     ioapic_route(uint8_t gsi, uint8_t vector, uint8_t apic_id, int masked);
void     ioapic_mask(uint8_t gsi);
void     ioapic_unmask(uint8_t gsi);

#endif /* IOAPIC_H */
