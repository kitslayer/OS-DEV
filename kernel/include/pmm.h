/* pmm.h — physical memory manager: hands out 4 KiB physical page frames. */
#pragma once
#include <stdint.h>

#define PAGE_SIZE 4096

void     pmm_init(uint64_t multiboot_info_phys);
uint64_t pmm_alloc_frame(void);          /* returns a physical address, or 0 */
void     pmm_free_frame(uint64_t phys);  /* drops one reference; releases the frame at the last */
void     pmm_addref(uint64_t phys);      /* add an extra reference (a frame mapped more than once) */

uint64_t pmm_total_bytes(void);
uint64_t pmm_free_bytes(void);
