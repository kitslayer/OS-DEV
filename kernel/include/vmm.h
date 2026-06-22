/*
 * vmm.h — virtual memory manager: manipulate the 4-level page tables.
 *
 * x86_64 paging translates a 48-bit virtual address through four tables —
 * PML4 -> PDPT -> PD -> PT — to a physical frame. Each table has 512 entries,
 * each 8 bytes, and an entry holds a physical frame address plus permission
 * bits. The VMM walks and edits these tables.
 */
#pragma once
#include <stdint.h>

/* Page-table entry flag bits. */
#define PTE_PRESENT   (1ull << 0)
#define PTE_WRITABLE  (1ull << 1)
#define PTE_USER      (1ull << 2)   /* accessible from ring 3 */
#define PTE_PWT       (1ull << 3)   /* write-through */
#define PTE_PCD       (1ull << 4)   /* cache-disable (for MMIO) */
#define PTE_HUGE      (1ull << 7)   /* 2 MiB page at the PD level */
#define PTE_NX        (1ull << 63)  /* no-execute */

/* Higher-half direct map: all physical RAM is also visible starting here, so
 * the kernel can reach any physical frame by adding this base. */
#define HHDM_BASE 0xFFFF800000000000ull

void     vmm_init(void);
int      vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);

/* Per-process address spaces. create returns the physical address of a fresh
 * PML4 that shares the kernel's mappings (code, heap, MMIO, higher half) but
 * has a private low user region. map_to maps into a specific address space. */
uint64_t vmm_create_address_space(void);
void     vmm_destroy_address_space(uint64_t cr3);  /* free a non-active app space (frames+tables) */
int      vmm_map_to(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);
int      vmm_map_huge(uint64_t virt, uint64_t phys, uint64_t flags);
void     vmm_unmap(uint64_t virt);
uint64_t vmm_translate(uint64_t virt);   /* physical address, or 0 if unmapped */
int      vmm_user_ok(uint64_t ptr, uint64_t len);  /* is [ptr,ptr+len) user-accessible (PTE_USER) in the current space? for syscall arg validation */

static inline void *hhdm(uint64_t phys) { return (void *)(HHDM_BASE + phys); }
