/* smp.h — symmetric multiprocessing: bring the application processors online
 * and run work on them.
 *
 * The kernel boots on the bootstrap processor (BSP); smp_init() enables the
 * local APIC, parses the ACPI MADT for the other cores, and brings each
 * application processor (AP) up through a real->long-mode trampoline
 * (kernel/asm/ap_trampoline.asm). Each AP adopts the kernel GDT+IDT and idles in
 * hlt until woken by an inter-processor interrupt (M1198). The BSP can then run
 * a pure-compute workload across all cores in parallel via smp_parallel_for().
 * The general scheduler still runs only on the BSP, so most of the kernel need
 * not be SMP-safe yet — only the small parallel pool here is concurrent.
 */
#pragma once
#include <stdint.h>

/* A parallel work function: process the half-open index range [lo, hi). It must
 * be PURE COMPUTE — no kmalloc / filesystem / other unsynchronised kernel state
 * (the pool gives no locking around it). */
typedef void (*smp_fn)(int lo, int hi, void *ctx);

extern int smp_cpu_count;          /* CPUs online: 1 (BSP) + each AP that started */
extern int smp_selftest_cores;     /* cores that ran a chunk in the boot self-test */
extern int smp_selftest_ok;        /* 1 = the boot parallel self-test matched */

void smp_init(void);               /* BSP: enable LAPIC, parse MADT, start the APs */
void ap_main(void);                /* AP 64-bit entry, called from the trampoline */
void lapic_enable_this_cpu(void);  /* software-enable the local APIC on this core */
void lapic_eoi(void);              /* end-of-interrupt to this core's local APIC */
void smp_parallel_for(int n, smp_fn fn, void *ctx);   /* run fn over [0,n) across all cores */
