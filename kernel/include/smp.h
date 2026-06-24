/* smp.h — symmetric multiprocessing: bring the application processors online.
 *
 * The kernel boots on the bootstrap processor (BSP) and runs uniprocessor; this
 * module enables the local APIC, parses the ACPI MADT for the other cores, and
 * brings each application processor (AP) up through a real->long-mode trampoline
 * (kernel/asm/ap_trampoline.asm) to a parked idle state. The scheduler still
 * runs only on the BSP — the APs sit in cli;hlt — so nothing else needs to be
 * SMP-safe yet; this is the foundational "the OS boots its other cores" slice.
 */
#pragma once
#include <stdint.h>

extern int smp_cpu_count;          /* CPUs online: 1 (BSP) + each AP that started */

void smp_init(void);               /* BSP: enable LAPIC, parse MADT, start the APs */
void ap_main(void);                /* AP 64-bit entry, called from the trampoline */
void lapic_enable_this_cpu(void);  /* software-enable the local APIC on this core */
