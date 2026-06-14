/*
 * elf.c — load an ELF64 executable into the current address space.
 *
 * An ELF file starts with a header that, among other things, gives the entry
 * point and the location of the "program headers." Each program header of type
 * PT_LOAD describes a chunk that must be copied into memory at a given virtual
 * address: `filesz` bytes come from the file, and any extra up to `memsz` is
 * zero (that's the .bss). We allocate frames, map them as user pages, and copy.
 *
 * This is the bridge between "a blob of bytes" and "a running program."
 *
 * Inputs may be untrusted (an ELF loaded from disk), so every header field is
 * validated against the image size and the user address range before use.
 */
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

#define PT_LOAD 1

static inline uint64_t page_down(uint64_t x) { return x & ~(uint64_t)(PAGE_SIZE - 1); }
static inline uint64_t page_up(uint64_t x)   { return (x + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1); }

/* User segments must sit in the low range, below the user stack — this also
 * rejects any p_vaddr aimed at kernel/higher-half memory. */
#define ELF_VADDR_MAX 0x50000000ull

uint64_t elf_load(const void *image, uint64_t maxsz) {
    const Elf64_Ehdr *eh = image;

    if (maxsz < sizeof(Elf64_Ehdr)) return 0;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
        return 0;
    /* the program-header table must lie within the image */
    if (eh->e_phentsize < sizeof(Elf64_Phdr)) return 0;
    if (eh->e_phoff > maxsz ||
        (uint64_t)eh->e_phnum * eh->e_phentsize > maxsz - eh->e_phoff)
        return 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            ((const uint8_t *)image + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD)
            continue;

        /* validate the segment against the image and the user address range */
        if (ph->p_offset > maxsz || ph->p_filesz > maxsz - ph->p_offset) return 0;
        if (ph->p_memsz < ph->p_filesz) return 0;
        if (ph->p_vaddr < PAGE_SIZE) return 0;                         /* no null page */
        if (ph->p_memsz > ELF_VADDR_MAX || ph->p_vaddr > ELF_VADDR_MAX - ph->p_memsz)
            return 0;                                                  /* fits below the stack, no overflow */

        /* Map every page this segment touches as a user page (once). */
        uint64_t start = page_down(ph->p_vaddr);
        uint64_t end   = page_up(ph->p_vaddr + ph->p_memsz);
        for (uint64_t v = start; v < end; v += PAGE_SIZE) {
            if (!vmm_translate(v)) {
                uint64_t frame = pmm_alloc_frame();
                vmm_map(v, frame, PTE_WRITABLE | PTE_USER);
                memset((void *)v, 0, PAGE_SIZE);   /* zero -> covers .bss */
            }
        }

        /* Copy the file-backed part of the segment into place. */
        memcpy((void *)ph->p_vaddr,
               (const uint8_t *)image + ph->p_offset,
               ph->p_filesz);
    }

    return eh->e_entry;
}
