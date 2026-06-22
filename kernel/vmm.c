/*
 * vmm.c — walk and edit the x86_64 page tables.
 *
 * A 48-bit virtual address is sliced into four 9-bit indices plus a 12-bit
 * offset:
 *
 *   [ 47 .. 39 | 38 .. 30 | 29 .. 21 | 20 .. 12 | 11 .. 0 ]
 *       PML4       PDPT        PD         PT       offset
 *
 * To map a page we follow those indices from the PML4 (in CR3) down, creating
 * any missing intermediate table along the way, and write the final entry.
 *
 * We reach the tables themselves through the low 1 GiB identity map: every
 * table frame comes from the PMM, which only returns low physical RAM, so its
 * physical address doubles as a usable virtual address. (Once we have the
 * HHDM we could use that instead; identity is simplest while it covers RAM.)
 */
#include "vmm.h"
#include "pmm.h"
#include "string.h"

#define PML4_IDX(v) (((v) >> 39) & 0x1FF)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FF)
#define PD_IDX(v)   (((v) >> 21) & 0x1FF)
#define PT_IDX(v)   (((v) >> 12) & 0x1FF)

#define ADDR_MASK   0x000FFFFFFFFFF000ull   /* frame address bits of an entry */

static uint64_t kernel_pml4;   /* the kernel's own PML4 — empty user region */

static uint64_t *phys_to_table(uint64_t phys) {
    return (uint64_t *)(uintptr_t)phys;     /* identity map */
}

static uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static void invlpg(uint64_t virt) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

/* Return the next-level table, allocating + zeroing it if not present.
 * Returns NULL if the PMM is out of frames: the alternative — mapping the
 * frame-0 sentinel as a PRESENT page table — would silently alias whatever
 * lives at physical 0 (the IVT / boot stubs) as live page tables. Callers must
 * propagate the failure rather than walk into a half-built mapping. */
static uint64_t *next_table(uint64_t *table, uint64_t idx, uint64_t flags) {
    if (!(table[idx] & PTE_PRESENT)) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) return 0;            /* OOM — do not map frame 0 */
        memset(phys_to_table(frame), 0, PAGE_SIZE);
        /* Intermediate entries must allow the most permissive access any leaf
         * under them needs — so propagate USER, always allow WRITABLE. */
        table[idx] = frame | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    } else if (flags & PTE_USER) {
        /* The table already exists (e.g. boot's supervisor-only pml4[0]). A
         * user mapping underneath it is only reachable if USER is set at EVERY
         * level, so upgrade this entry. This never weakens kernel pages: their
         * leaf/huge entries still lack USER, and the effective permission is
         * the AND across all levels. */
        table[idx] |= PTE_USER;
    }
    return phys_to_table(table[idx] & ADDR_MASK);
}

static int do_map(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pml4 = phys_to_table(pml4_phys);
    uint64_t *pdpt = next_table(pml4, PML4_IDX(virt), flags);
    if (!pdpt) return -1;
    uint64_t *pd   = next_table(pdpt, PDPT_IDX(virt), flags);
    if (!pd) return -1;
    uint64_t *pt   = next_table(pd,   PD_IDX(virt),   flags);
    if (!pt) return -1;

    pt[PT_IDX(virt)] = (phys & ADDR_MASK) | PTE_PRESENT | flags;
    invlpg(virt);
    return 0;
}

int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    return do_map(read_cr3() & ADDR_MASK, virt, phys, flags);
}

int vmm_map_to(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    return do_map(pml4_phys & ADDR_MASK, virt, phys, flags);
}

/*
 * Build a new address space. The new PML4 shares everything the kernel needs:
 *  - the entire higher half (PML4[256..511]): HHDM, kernel heap, etc.
 *  - the kernel's low mappings (identity RAM + device MMIO) by copying the
 *    boot PML4[0]'s PDPT entries into a *new* PDPT.
 * The new PDPT is private, so per-process user mappings added under PML4[0]
 * (e.g. at 0x40000000) don't leak between address spaces.
 */
uint64_t vmm_create_address_space(void) {
    /* Always derive from the kernel PML4, NOT the current one: a caller running
     * in some process's address space has user mappings in its low region that
     * must NOT leak into the new space. The kernel PML4's user region is empty,
     * while its kernel/heap/MMIO mappings (shared, by pointer) stay live. */
    uint64_t *bpml4 = phys_to_table(kernel_pml4);

    uint64_t newp = pmm_alloc_frame();
    if (!newp) return 0;                   /* OOM */
    uint64_t *npml4 = phys_to_table(newp);
    memset(npml4, 0, PAGE_SIZE);

    for (int i = 256; i < 512; i++)        /* share the higher half */
        npml4[i] = bpml4[i];

    uint64_t *bpdpt = phys_to_table(bpml4[0] & ADDR_MASK);
    uint64_t newpdpt = pmm_alloc_frame();
    if (!newpdpt) { pmm_free_frame(newp); return 0; }   /* OOM — undo the PML4 */
    uint64_t *npdpt = phys_to_table(newpdpt);
    for (int i = 0; i < 512; i++)          /* share kernel identity + MMIO PDs */
        npdpt[i] = bpdpt[i];

    npml4[0] = newpdpt | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    return newp;
}

/*
 * Tear down an address space built by vmm_create_address_space: free the app's
 * private user mappings (frames + their page tables) and the private PML4/PDPT
 * pages, leaving the shared kernel identity map and the higher half intact.
 *
 * Safety rests on a property of vmm_create_address_space + next_table:
 * vmm_create copies boot's PDPT entries verbatim, and next_table only ever
 * *writes* a PDPT/PD slot when it was not-present (allocating a fresh table).
 * So a PDPT entry that DIFFERS from boot's is provably a private, app-allocated
 * PD — the shared kernel PDs are byte-identical copies and are skipped. User
 * mappings all live above boot's low identity map (user.ld bases apps at 1 GiB,
 * stack at 0x50000000), i.e. under PDPT slots boot leaves empty, so the whole
 * user footprint is reclaimed. Only call on a NON-ACTIVE space (asserted via the
 * CR3 guard below); the desktop reaper that calls this runs in the kernel PML4.
 */
void vmm_destroy_address_space(uint64_t cr3) {
    cr3 &= ADDR_MASK;
    if (!cr3 || cr3 == kernel_pml4) return;          /* never the kernel's own */
    if (cr3 == (read_cr3() & ADDR_MASK)) return;     /* never the active space  */

    uint64_t *pml4  = phys_to_table(cr3);
    uint64_t *bpml4 = phys_to_table(kernel_pml4);

    /* The user/private region lives only under PML4[0]; [1..255] are zero and
     * [256..511] are the shared higher half — never touched. */
    uint64_t pml4e = pml4[0];
    if ((pml4e & PTE_PRESENT) && (pml4e & ADDR_MASK) != (bpml4[0] & ADDR_MASK)) {
        uint64_t pdpt_phys = pml4e & ADDR_MASK;
        uint64_t *pdpt  = phys_to_table(pdpt_phys);
        uint64_t *bpdpt = phys_to_table(bpml4[0] & ADDR_MASK);
        for (int i = 0; i < 512; i++) {
            if (!(pdpt[i] & PTE_PRESENT)) continue;
            if ((pdpt[i] & ADDR_MASK) == (bpdpt[i] & ADDR_MASK)) continue;  /* shared boot PD */
            if (pdpt[i] & PTE_HUGE) continue;                              /* 1 GiB page (n/a for user) */
            uint64_t *pd = phys_to_table(pdpt[i] & ADDR_MASK);
            for (int j = 0; j < 512; j++) {
                if (!(pd[j] & PTE_PRESENT)) continue;
                if (pd[j] & PTE_HUGE) continue;                            /* 2 MiB page (n/a for user) */
                uint64_t *pt = phys_to_table(pd[j] & ADDR_MASK);
                for (int k = 0; k < 512; k++)
                    if (pt[k] & PTE_PRESENT) pmm_free_frame(pt[k] & ADDR_MASK);  /* user frame */
                pmm_free_frame(pd[j] & ADDR_MASK);                         /* the PT page */
            }
            pmm_free_frame(pdpt[i] & ADDR_MASK);                          /* the PD page */
        }
        pmm_free_frame(pdpt_phys);                                       /* the private PDPT page */
    }
    pmm_free_frame(cr3);                                                 /* the PML4 page */
}

int vmm_map_huge(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pml4 = phys_to_table(read_cr3() & ADDR_MASK);
    uint64_t *pdpt = next_table(pml4, PML4_IDX(virt), flags);
    if (!pdpt) return -1;
    uint64_t *pd   = next_table(pdpt, PDPT_IDX(virt), flags);
    if (!pd) return -1;

    pd[PD_IDX(virt)] = (phys & ~0x1FFFFFull) | PTE_PRESENT | PTE_HUGE | flags;
    invlpg(virt);
    return 0;
}

void vmm_unmap(uint64_t virt) {
    uint64_t *pml4 = phys_to_table(read_cr3() & ADDR_MASK);
    if (!(pml4[PML4_IDX(virt)] & PTE_PRESENT)) return;
    uint64_t *pdpt = phys_to_table(pml4[PML4_IDX(virt)] & ADDR_MASK);
    if (!(pdpt[PDPT_IDX(virt)] & PTE_PRESENT)) return;
    uint64_t *pd = phys_to_table(pdpt[PDPT_IDX(virt)] & ADDR_MASK);
    if (!(pd[PD_IDX(virt)] & PTE_PRESENT)) return;
    uint64_t *pt = phys_to_table(pd[PD_IDX(virt)] & ADDR_MASK);

    pt[PT_IDX(virt)] = 0;
    invlpg(virt);
}

uint64_t vmm_translate(uint64_t virt) {
    uint64_t *pml4 = phys_to_table(read_cr3() & ADDR_MASK);
    if (!(pml4[PML4_IDX(virt)] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = phys_to_table(pml4[PML4_IDX(virt)] & ADDR_MASK);
    if (!(pdpt[PDPT_IDX(virt)] & PTE_PRESENT)) return 0;
    uint64_t *pd = phys_to_table(pdpt[PDPT_IDX(virt)] & ADDR_MASK);
    if (!(pd[PD_IDX(virt)] & PTE_PRESENT)) return 0;
    if (pd[PD_IDX(virt)] & PTE_HUGE)
        return (pd[PD_IDX(virt)] & ~0x1FFFFFull) | (virt & 0x1FFFFF);
    uint64_t *pt = phys_to_table(pd[PD_IDX(virt)] & ADDR_MASK);
    if (!(pt[PT_IDX(virt)] & PTE_PRESENT)) return 0;
    return (pt[PT_IDX(virt)] & ADDR_MASK) | (virt & 0xFFF);
}

/*
 * Is the whole range [ptr, ptr+len) accessible to the CURRENT (user) address
 * space — i.e. mapped and PTE_USER at every paging level? A ring-0 syscall
 * handler runs with the calling app's CR3 active, where the kernel's higher
 * half and low identity map are mapped (and writable in ring 0) but NOT marked
 * USER. So validating PTE_USER on every page an app hands to a syscall stops it
 * from steering the kernel into reading/writing kernel memory through a forged
 * pointer. Returns 1 if every page is user-accessible, 0 otherwise (unmapped,
 * supervisor-only, or a length that wraps the address space).
 */
int vmm_user_ok(uint64_t ptr, uint64_t len) {
    if (len == 0) return 1;                       /* empty range touches nothing */
    uint64_t end = ptr + len;
    if (end < ptr) return 0;                      /* address wrap */
    uint64_t *pml4 = phys_to_table(read_cr3() & ADDR_MASK);
    for (uint64_t v = ptr & ~(uint64_t)(PAGE_SIZE - 1); v < end; v += PAGE_SIZE) {
        uint64_t e = pml4[PML4_IDX(v)];
        if (!(e & PTE_PRESENT) || !(e & PTE_USER)) return 0;
        uint64_t *pdpt = phys_to_table(e & ADDR_MASK);
        e = pdpt[PDPT_IDX(v)];
        if (!(e & PTE_PRESENT) || !(e & PTE_USER)) return 0;
        if (e & PTE_HUGE) continue;               /* 1 GiB user page covers v */
        uint64_t *pd = phys_to_table(e & ADDR_MASK);
        e = pd[PD_IDX(v)];
        if (!(e & PTE_PRESENT) || !(e & PTE_USER)) return 0;
        if (e & PTE_HUGE) continue;               /* 2 MiB user page covers v */
        uint64_t *pt = phys_to_table(e & ADDR_MASK);
        e = pt[PT_IDX(v)];
        if (!(e & PTE_PRESENT) || !(e & PTE_USER)) return 0;
    }
    return 1;
}

/*
 * Is the NUL-terminated string at `ptr` entirely within the current space's
 * user pages, up to and including its terminator? Validates each page (via
 * vmm_user_ok) before reading any byte of it, so the scan itself can't fault
 * or run into kernel memory. Scans at most `max` bytes — a string with no NUL
 * in range, or one that crosses into a non-user page, is rejected (returns 0).
 * For syscall string arguments (filenames, hostnames) whose length isn't known
 * up front.
 */
int vmm_user_str_ok(uint64_t ptr, uint64_t max) {
    uint64_t scanned = 0;
    while (scanned < max) {
        uint64_t page = (ptr + scanned) & ~(uint64_t)(PAGE_SIZE - 1);
        if (!vmm_user_ok(page, PAGE_SIZE)) return 0;       /* page not user-accessible */
        for (uint64_t a = ptr + scanned; a < page + PAGE_SIZE && scanned < max; a++, scanned++)
            if (*(const char *)a == 0) return 1;           /* terminator reached, every byte was user */
    }
    return 0;   /* no terminator within `max`: reject rather than read unbounded */
}

/* Build the higher-half direct map: map all physical RAM at HHDM_BASE using
 * cheap 2 MiB pages, so the kernel can touch any frame via hhdm(phys). */
void vmm_init(void) {
    kernel_pml4 = read_cr3() & ADDR_MASK;   /* boot PML4: kernel-only mappings */
    uint64_t total = pmm_total_bytes();
    for (uint64_t phys = 0; phys < total; phys += 0x200000)
        vmm_map_huge(HHDM_BASE + phys, phys, PTE_WRITABLE);
}
