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
#define PTE_ACCESSED  (1ull << 5)   /* CPU sets this on any access (read/write/exec) */
#define PTE_DIRTY     (1ull << 6)   /* CPU sets this on a write */
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
int      vmm_protect(uint64_t virt, uint64_t flags);  /* rewrite a mapped page's flags (mprotect); 0/-1 */
int      vmm_user_ok(uint64_t ptr, uint64_t len);  /* is [ptr,ptr+len) user-accessible (PTE_USER) in the current space? for syscall arg validation */
int      vmm_user_str_ok(uint64_t ptr, uint64_t max);  /* is the NUL-terminated string at ptr entirely in user pages (<= max bytes)? */

/* Working-set introspection via the CPU-maintained Accessed/Dirty PTE bits.
 * vmm_wss walks every present ring-3 leaf in address space `cr3` and tallies
 * resident / referenced (A=1) / dirty (D=1) / writable page counts (safe on a
 * non-active space — reads tables through the HHDM). vmm_clear_accessed resets
 * the estimator window by clearing A on every such leaf (flushing the TLB if
 * `cr3` is the active space); returns the number of pages cleared. M1093. */
typedef struct { uint64_t resident, referenced, dirty, writable; } vmm_wss_t;
void     vmm_wss(uint64_t cr3, vmm_wss_t *out);
int      vmm_clear_accessed(uint64_t cr3);

/* Raw leaf-PTE access (M1105, swap): read/write the exact entry so a
 * not-present-but-swapped page (PTE_SWAP marker + a slot index) is distinct
 * from an unmapped one. vmm_set_raw requires the page table to already exist. */
#define PTE_SWAP (1ull << 9)   /* software bit: this not-present page is swapped out */
uint64_t vmm_pte_raw(uint64_t virt);
void     vmm_set_raw(uint64_t virt, uint64_t pte);

static inline void *hhdm(uint64_t phys) { return (void *)(HHDM_BASE + phys); }
