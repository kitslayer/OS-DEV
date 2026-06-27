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
#include "console.h"   /* kprintf — for the W^X self-check report */

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
                if (pd[j] & PTE_HUGE) { pmm_free_contiguous(pd[j] & ~0x1FFFFFull, 512); continue; }  /* user 2 MiB hugepage (M1155) */
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

/*
 * Copy-on-write clone the CURRENT (parent) address space into child_cr3, for
 * fork() (M1116). Walks the same private PML4[0] subtree as the teardown/wss
 * walks (a PDPT slot differing from boot's is app-private). For each present
 * user 4 KiB leaf:
 *   - a WRITABLE page becomes read-only + PTE_COW in BOTH parent and child, and
 *     the shared frame gets one extra reference (pmm_addref) — a later write in
 *     either process faults and copies (app_fault_handle).
 *   - a read-only page is shared as-is (one extra ref), still RO.
 * Page-TABLE pages are never shared: vmm_map_to builds fresh tables in the child
 * (so each address space frees its own hierarchy at teardown, no double-free).
 * Frames outside the refcount array (>1 GiB; impossible at 256 MiB) are eagerly
 * copied instead, so they're never double-freed. The parent's TLB is flushed
 * (CR3 reload) because we write-protected its live pages. Returns 0, or -1.
 */
int vmm_fork_cow(uint64_t child_cr3) {
    uint64_t cr3 = read_cr3() & ADDR_MASK;
    uint64_t *pml4  = phys_to_table(cr3);
    uint64_t *bpml4 = phys_to_table(kernel_pml4);
    uint64_t pml4e = pml4[0];
    if (!(pml4e & PTE_PRESENT) || (pml4e & ADDR_MASK) == (bpml4[0] & ADDR_MASK)) return 0;
    uint64_t *pdpt  = phys_to_table(pml4e & ADDR_MASK);
    uint64_t *bpdpt = phys_to_table(bpml4[0] & ADDR_MASK);
    int rc = 0;
    for (int i = 0; i < 512 && rc == 0; i++) {
        if (!(pdpt[i] & PTE_PRESENT)) continue;
        if ((pdpt[i] & ADDR_MASK) == (bpdpt[i] & ADDR_MASK)) continue;   /* shared boot PD */
        if (pdpt[i] & PTE_HUGE) continue;
        uint64_t *pd = phys_to_table(pdpt[i] & ADDR_MASK);
        for (int j = 0; j < 512 && rc == 0; j++) {
            if (!(pd[j] & PTE_PRESENT) || (pd[j] & PTE_HUGE)) continue;
            uint64_t *pt = phys_to_table(pd[j] & ADDR_MASK);
            for (int k = 0; k < 512; k++) {
                uint64_t e = pt[k];
                if (!(e & PTE_PRESENT) || !(e & PTE_USER)) continue;
                uint64_t phys = e & ADDR_MASK;
                uint64_t va = ((uint64_t)i << 30) | ((uint64_t)j << 21) | ((uint64_t)k << 12);  /* PML4 idx 0 */
                if (!pmm_refcountable(phys)) {            /* can't refcount -> eager private copy */
                    uint64_t nf = pmm_alloc_frame();
                    if (!nf) { rc = -1; break; }
                    uint8_t *s = hhdm(phys), *d = hhdm(nf);
                    for (int b = 0; b < PAGE_SIZE; b++) d[b] = s[b];
                    if (vmm_map_to(child_cr3, va, nf, e & (PTE_WRITABLE | PTE_USER | PTE_NX)) != 0) { pmm_free_frame(nf); rc = -1; break; }
                    continue;
                }
                uint64_t flags = e & (PTE_USER | PTE_NX);
                if (e & PTE_WRITABLE) {                   /* COW: write-protect both sides, mark COW */
                    pt[k] = (e & ~PTE_WRITABLE) | PTE_COW; /* parent now RO + COW */
                    flags |= PTE_COW;
                }
                if (vmm_map_to(child_cr3, va, phys, flags) != 0) { rc = -1; break; }
                pmm_addref(phys);                         /* one extra ref for the child's mapping */
            }
        }
    }
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");   /* flush parent TLB (we write-protected it) */
    return rc;
}

/*
 * Walk every present ring-3 (PTE_USER) 4 KiB leaf in address space `cr3`. This
 * is the read-only twin of vmm_destroy_address_space and rests on the same
 * invariant: the app's private user region lives only under PML4[0], and a PDPT
 * slot that DIFFERS from boot's is a private, app-allocated PD (the shared
 * kernel PDs are byte-identical copies, skipped). For `clear`==0 it tallies
 * into *w; for `clear`==1 it clears the Accessed bit on each leaf. Returns the
 * resident-leaf count. Caller flushes the TLB if needed (see vmm_clear_accessed).
 */
static uint64_t walk_user_leaves(uint64_t cr3, vmm_wss_t *w, int clear) {
    cr3 &= ADDR_MASK;
    if (!cr3) return 0;
    uint64_t *pml4  = phys_to_table(cr3);
    uint64_t *bpml4 = phys_to_table(kernel_pml4);
    uint64_t pml4e = pml4[0];
    if (!(pml4e & PTE_PRESENT) || (pml4e & ADDR_MASK) == (bpml4[0] & ADDR_MASK)) return 0;
    uint64_t *pdpt  = phys_to_table(pml4e & ADDR_MASK);
    uint64_t *bpdpt = phys_to_table(bpml4[0] & ADDR_MASK);
    uint64_t n = 0;
    for (int i = 0; i < 512; i++) {
        if (!(pdpt[i] & PTE_PRESENT)) continue;
        if ((pdpt[i] & ADDR_MASK) == (bpdpt[i] & ADDR_MASK)) continue;  /* shared boot PD */
        if (pdpt[i] & PTE_HUGE) continue;
        uint64_t *pd = phys_to_table(pdpt[i] & ADDR_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(pd[j] & PTE_PRESENT) || (pd[j] & PTE_HUGE)) continue;
            uint64_t *pt = phys_to_table(pd[j] & ADDR_MASK);
            for (int k = 0; k < 512; k++) {
                uint64_t e = pt[k];
                if (!(e & PTE_PRESENT) || !(e & PTE_USER)) continue;
                n++;
                if (clear) {
                    if (e & PTE_ACCESSED) pt[k] = e & ~PTE_ACCESSED;
                } else if (w) {
                    w->resident++;
                    if (e & PTE_ACCESSED) w->referenced++;
                    if (e & PTE_DIRTY)    w->dirty++;
                    if (e & PTE_WRITABLE) w->writable++;
                }
            }
        }
    }
    return n;
}

void vmm_wss(uint64_t cr3, vmm_wss_t *out) {
    if (!out) return;
    out->resident = out->referenced = out->dirty = out->writable = 0;
    walk_user_leaves(cr3, out, 0);
}

int vmm_clear_accessed(uint64_t cr3) {
    uint64_t n = walk_user_leaves(cr3, 0, 1);
    /* The CPU caches A=1 in the TLB; clearing the PTE alone won't make the next
     * access re-set it unless we flush. A CR3 reload flushes the whole
     * non-global TLB — needed only when clearing the ACTIVE space (self). For a
     * non-active target, the scheduler's CR3 reload on the next switch flushes it. */
    if ((cr3 & ADDR_MASK) == (read_cr3() & ADDR_MASK))
        __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    return (int)n;
}

/* Read the raw leaf PTE for `virt` in the active space (0 if no page table walks
 * there). Lets swap distinguish a not-present-but-swapped page (a software marker
 * + slot packed into the entry) from a genuinely unmapped one. M1105. */
uint64_t vmm_pte_raw(uint64_t virt) {
    uint64_t *pml4 = phys_to_table(read_cr3() & ADDR_MASK);
    if (!(pml4[PML4_IDX(virt)] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = phys_to_table(pml4[PML4_IDX(virt)] & ADDR_MASK);
    if (!(pdpt[PDPT_IDX(virt)] & PTE_PRESENT)) return 0;
    uint64_t *pd = phys_to_table(pdpt[PDPT_IDX(virt)] & ADDR_MASK);
    if (!(pd[PD_IDX(virt)] & PTE_PRESENT) || (pd[PD_IDX(virt)] & PTE_HUGE)) return 0;
    uint64_t *pt = phys_to_table(pd[PD_IDX(virt)] & ADDR_MASK);
    return pt[PT_IDX(virt)];
}
/* Set the raw leaf PTE for `virt` (the page table must already exist — true for
 * a page being evicted, which was present). Used to write the swapped encoding. */
void vmm_set_raw(uint64_t virt, uint64_t pte) {
    uint64_t *pml4 = phys_to_table(read_cr3() & ADDR_MASK);
    if (!(pml4[PML4_IDX(virt)] & PTE_PRESENT)) return;
    uint64_t *pdpt = phys_to_table(pml4[PML4_IDX(virt)] & ADDR_MASK);
    if (!(pdpt[PDPT_IDX(virt)] & PTE_PRESENT)) return;
    uint64_t *pd = phys_to_table(pdpt[PDPT_IDX(virt)] & ADDR_MASK);
    if (!(pd[PD_IDX(virt)] & PTE_PRESENT)) return;
    uint64_t *pt = phys_to_table(pd[PD_IDX(virt)] & ADDR_MASK);
    pt[PT_IDX(virt)] = pte;
    invlpg(virt);
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

/* Tear down a 2 MiB huge mapping: clear the PD entry directly (a huge PD entry
 * points at the 2 MiB data frame, NOT a page table — so vmm_unmap must not be
 * used on it). The caller frees the underlying contiguous run. (M1155) */
void vmm_unmap_huge(uint64_t virt) {
    uint64_t *pml4 = phys_to_table(read_cr3() & ADDR_MASK);
    if (!(pml4[PML4_IDX(virt)] & PTE_PRESENT)) return;
    uint64_t *pdpt = phys_to_table(pml4[PML4_IDX(virt)] & ADDR_MASK);
    if (!(pdpt[PDPT_IDX(virt)] & PTE_PRESENT)) return;
    uint64_t *pd = phys_to_table(pdpt[PDPT_IDX(virt)] & ADDR_MASK);
    pd[PD_IDX(virt)] = 0;
    invlpg(virt);
}

/* Physical address of the 4 KiB page table (PT) that maps `virt` in space
 * `cr3`, or 0 if there's no 4 KiB-level PT (absent, or a 2 MiB huge PD entry).
 * MADV_COLLAPSE uses it to free the page table it orphans when it overwrites
 * the PD entry with a hugepage (M1168). */
uint64_t vmm_pt_phys_in(uint64_t cr3, uint64_t virt) {
    uint64_t *pml4 = phys_to_table(cr3 & ADDR_MASK);
    if (!(pml4[PML4_IDX(virt)] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = phys_to_table(pml4[PML4_IDX(virt)] & ADDR_MASK);
    if (!(pdpt[PDPT_IDX(virt)] & PTE_PRESENT) || (pdpt[PDPT_IDX(virt)] & PTE_HUGE)) return 0;
    uint64_t *pd = phys_to_table(pdpt[PDPT_IDX(virt)] & ADDR_MASK);
    uint64_t e = pd[PD_IDX(virt)];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE)) return 0;
    return e & ADDR_MASK;
}

/* Rewrite the access flags of an already-mapped 4 KiB page, keeping its frame
 * (for mprotect / W^X). `flags` are the new low bits (e.g. PTE_USER, plus maybe
 * PTE_WRITABLE / PTE_NX); PTE_PRESENT is always set. Returns 0/-1. M1090. */
int vmm_protect(uint64_t virt, uint64_t flags) {
    uint64_t *pml4 = phys_to_table(read_cr3() & ADDR_MASK);
    if (!(pml4[PML4_IDX(virt)] & PTE_PRESENT)) return -1;
    uint64_t *pdpt = phys_to_table(pml4[PML4_IDX(virt)] & ADDR_MASK);
    if (!(pdpt[PDPT_IDX(virt)] & PTE_PRESENT)) return -1;
    uint64_t *pd = phys_to_table(pdpt[PDPT_IDX(virt)] & ADDR_MASK);
    if (!(pd[PD_IDX(virt)] & PTE_PRESENT) || (pd[PD_IDX(virt)] & PTE_HUGE)) return -1;
    uint64_t *pt = phys_to_table(pd[PD_IDX(virt)] & ADDR_MASK);
    uint64_t e = pt[PT_IDX(virt)];
    if (!(e & PTE_PRESENT)) return -1;
    pt[PT_IDX(virt)] = (e & ADDR_MASK) | PTE_PRESENT | flags;   /* keep the frame, replace the flags */
    invlpg(virt);
    return 0;
}

/* Split the 2 MiB huge page that maps `virt` (active space) into 512 individual
 * 4 KiB pages over the same physical 2 MiB, preserving the leaf flags. This is the
 * prerequisite for per-page permissions (W^X) on a region the boot trampoline
 * mapped with one huge page. Returns 0 if already 4 KiB-mapped (no-op) or after a
 * successful split; -1 on OOM or if there is no present PD entry. */
static int vmm_split_huge(uint64_t virt) {
    uint64_t *pml4 = phys_to_table(read_cr3() & ADDR_MASK);
    if (!(pml4[PML4_IDX(virt)] & PTE_PRESENT)) return -1;
    uint64_t *pdpt = phys_to_table(pml4[PML4_IDX(virt)] & ADDR_MASK);
    if (!(pdpt[PDPT_IDX(virt)] & PTE_PRESENT) || (pdpt[PDPT_IDX(virt)] & PTE_HUGE)) return -1;
    uint64_t *pd = phys_to_table(pdpt[PDPT_IDX(virt)] & ADDR_MASK);
    uint64_t e = pd[PD_IDX(virt)];
    if (!(e & PTE_PRESENT)) return -1;
    if (!(e & PTE_HUGE))    return 0;                  /* already a 4 KiB PT — nothing to split */

    uint64_t base   = e & ~0x1FFFFFull;                /* 2 MiB-aligned physical base */
    uint64_t lflags = e & (PTE_WRITABLE | PTE_USER | PTE_NX);   /* carry leaf perms, drop HUGE */
    uint64_t ptphys = pmm_alloc_frame();
    if (!ptphys) return -1;
    uint64_t *pt = phys_to_table(ptphys);
    for (int i = 0; i < 512; i++)
        pt[i] = (base + (uint64_t)i * PAGE_SIZE) | PTE_PRESENT | lflags;
    pd[PD_IDX(virt)] = ptphys | PTE_PRESENT | PTE_WRITABLE;     /* PD entry now points at the PT */
    uint64_t hbase = virt & ~0x1FFFFFull;
    for (uint64_t off = 0; off < 0x200000; off += PAGE_SIZE) invlpg(hbase + off);
    return 0;
}

/*
 * Enforce W^X on the kernel image. The boot trampoline (boot.asm) identity-maps the
 * low 1 GiB with 2 MiB huge pages, so the kernel's own code and data share huge,
 * writable, EXECUTABLE mappings — a kernel bug can overwrite kernel code, and any
 * writable page (stack, heap, data) is also executable. Here we split the huge pages
 * covering the kernel image into 4 KiB pages and tighten each section:
 *   .text            -> read-only + executable  (kernel code can no longer be patched)
 *   .rodata          -> read-only + no-execute   (constants + embedded app ELFs immutable)
 *   .data/.bss/stack -> writable  + no-execute   (defeats executing injected data/stack bytes)
 * The eBPF JIT + module loader emit code into the .jitexec section, which is re-marked
 * RWX after the blanket NX pass. EFER.NXE is enabled in boot.asm. Run once at boot,
 * single-threaded, in the kernel address space (before sched_init / any user CR3).
 */
void vmm_harden_kernel(void) {
    extern char _kimage_start[], _srodata[], _sdata[], kernel_end[];
    extern char _jitexec_start[], _jitexec_end[];
    uint64_t text_s = (uint64_t)_kimage_start;
    uint64_t ro_s   = (uint64_t)_srodata;
    uint64_t data_s = (uint64_t)_sdata;
    uint64_t end    = ((uint64_t)kernel_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t jit_s  = (uint64_t)_jitexec_start;
    uint64_t jit_e  = (uint64_t)_jitexec_end;

    /* 1. Split every 2 MiB huge page overlapping the kernel image into 4 KiB pages. */
    for (uint64_t v = text_s & ~0x1FFFFFull; v < end; v += 0x200000)
        vmm_split_huge(v);

    /* 2. Tighten per section (vmm_protect always re-sets PRESENT). */
    for (uint64_t v = text_s; v < ro_s;   v += PAGE_SIZE) vmm_protect(v, 0);                      /* RX, read-only */
    for (uint64_t v = ro_s;   v < data_s; v += PAGE_SIZE) vmm_protect(v, PTE_NX);                 /* R,  NX        */
    for (uint64_t v = data_s; v < end;    v += PAGE_SIZE) vmm_protect(v, PTE_WRITABLE | PTE_NX);  /* RW, NX        */

    /* 3. The JIT / module scratch must execute: re-mark it RWX (override the NX above). */
    for (uint64_t v = jit_s;  v < jit_e;  v += PAGE_SIZE) vmm_protect(v, PTE_WRITABLE);           /* RWX           */

    /* 4. Unmap the boot stack's guard page (boot.asm reserves it just below the
     *    stack): a boot-stack overflow now faults here instead of silently
     *    corrupting the page tables / multiboot info that sit beneath the stack. */
    extern char stack_guard[];
    vmm_unmap((uint64_t)stack_guard);

    /* Verify the protection actually took (read representative leaf PTEs straight from
     * the page tables) and report — turns "it booted" into a checked invariant. */
    uint64_t te = vmm_pte_raw(text_s), re = vmm_pte_raw(ro_s), de = vmm_pte_raw(data_s);
    kprintf("[ ok ] W^X: .text=%s .rodata=%s .data/.bss=%s (kernel image %lu KiB)\n",
            ((te & PTE_PRESENT) && !(te & PTE_WRITABLE) && !(te & PTE_NX)) ? "RO+X"  : "BAD",
            ((re & PTE_PRESENT) && !(re & PTE_WRITABLE) &&  (re & PTE_NX)) ? "RO+NX" : "BAD",
            ((de & PTE_PRESENT) &&  (de & PTE_WRITABLE) &&  (de & PTE_NX)) ? "RW+NX" : "BAD",
            (unsigned long)((end - text_s) / 1024));
    kprintf("[ ok ] stack guard: boot-stack guard page %s\n",
            (vmm_pte_raw((uint64_t)stack_guard) & PTE_PRESENT) ? "MAPPED (BAD)" : "unmapped");

    /* Flush the whole TLB (we downgraded huge->4 KiB and changed many leaf flags). */
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
}

/* Translate a virtual address in an ARBITRARY address space (walk `cr3`'s tables
 * via the HHDM, never loading CR3) — for inspecting another process's memory
 * (/proc/<pid>/mem, M1114). Returns the physical address, or 0 if unmapped. */
uint64_t vmm_translate_in(uint64_t cr3, uint64_t virt) {
    uint64_t *pml4 = phys_to_table(cr3 & ADDR_MASK);
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

/* The raw leaf PTE for `virt` in an arbitrary address space `cr3` (0 if the walk
 * can't reach a leaf). Unlike vmm_translate_in this returns the entry even when
 * PRESENT=0, so a caller can see the software PTE_SWAP marker + the A/D bits —
 * exactly what /proc/<pid>/smaps needs to classify a page (M1151). */
uint64_t vmm_pte_in(uint64_t cr3, uint64_t virt) {
    uint64_t *pml4 = phys_to_table(cr3 & ADDR_MASK);
    if (!(pml4[PML4_IDX(virt)] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = phys_to_table(pml4[PML4_IDX(virt)] & ADDR_MASK);
    if (!(pdpt[PDPT_IDX(virt)] & PTE_PRESENT)) return 0;
    uint64_t *pd = phys_to_table(pdpt[PDPT_IDX(virt)] & ADDR_MASK);
    if (!(pd[PD_IDX(virt)] & PTE_PRESENT)) return 0;
    if (pd[PD_IDX(virt)] & PTE_HUGE) return pd[PD_IDX(virt)];
    uint64_t *pt = phys_to_table(pd[PD_IDX(virt)] & ADDR_MASK);
    return pt[PT_IDX(virt)];   /* raw leaf: PRESENT/DIRTY/ACCESSED/SWAP bits all intact */
}

/* Set the 4 KiB leaf PTE for `virt` in an arbitrary address space `cr3`, ONLY if
 * the page tables down to the leaf already exist (the page is present) — it never
 * creates tables. Returns 0 on success, -1 if no leaf. Used by process_vm_write
 * to break COW in the target before poking it (M1165). On single-CPU the target
 * isn't running, and its next schedule reloads CR3 (flushing the TLB), so no
 * cross-AS invlpg is needed. */
int vmm_set_pte_in(uint64_t cr3, uint64_t virt, uint64_t pte) {
    uint64_t *pml4 = phys_to_table(cr3 & ADDR_MASK);
    if (!(pml4[PML4_IDX(virt)] & PTE_PRESENT)) return -1;
    uint64_t *pdpt = phys_to_table(pml4[PML4_IDX(virt)] & ADDR_MASK);
    if (!(pdpt[PDPT_IDX(virt)] & PTE_PRESENT)) return -1;
    uint64_t *pd = phys_to_table(pdpt[PDPT_IDX(virt)] & ADDR_MASK);
    if (!(pd[PD_IDX(virt)] & PTE_PRESENT) || (pd[PD_IDX(virt)] & PTE_HUGE)) return -1;
    uint64_t *pt = phys_to_table(pd[PD_IDX(virt)] & ADDR_MASK);
    pt[PT_IDX(virt)] = pte;
    return 0;
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
