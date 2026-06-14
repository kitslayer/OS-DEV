/* gdt.h — Global Descriptor Table + Task State Segment. */
#pragma once
#include <stdint.h>

/* Segment selectors (offsets into the GDT). The low 2 bits are the requested
 * privilege level; user segments are OR'd with 3. */
#define KERNEL_CS  0x08
#define KERNEL_DS  0x10
#define USER_CS    (0x18 | 3)
#define USER_DS    (0x20 | 3)
#define TSS_SEL    0x28

void gdt_init(void);

/* Set the kernel stack the CPU switches to on a ring 3 -> ring 0 trap. The
 * scheduler updates this per task so each user process traps onto its own
 * kernel stack. */
void tss_set_rsp0(uint64_t rsp0);
