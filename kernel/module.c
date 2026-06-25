/*
 * module.c — loadable kernel module loader (M1261). See module.h.
 *
 * An ET_REL object (.ko) is a relocatable ELF: its sections have no final
 * addresses and its references to other symbols are left as RELOCATIONS to be
 * patched once we choose where each section lives. This loader does exactly what
 * a kernel's insmod does:
 *
 *   1. Place each allocatable section (.text/.data/.rodata/.bss) at a chosen
 *      address inside a static executable area (the kernel image is mapped RWX,
 *      so its BSS is executable — no separate W^X dance needed).
 *   2. Resolve symbols: a module symbol defined in one of its own sections gets
 *      section_base + st_value; an UNDEFINED symbol (e.g. timer_ms) is looked up
 *      by NAME in the kernel symbol table (ksym_addr) — that's how a module calls
 *      into the kernel.
 *   3. Apply each SHT_RELA entry: compute S (symbol addr), A (addend), P (patch
 *      site) and write S+A (absolute) or S+A-P (PC-relative) per the type.
 *   4. Find `mod_init`, cast its address to a function pointer, and call it.
 *
 * Bounds are checked against the image length throughout — a .ko is trusted
 * (it's built into the kernel) but the parser stays defensive so a malformed one
 * fails cleanly rather than scribbling memory.
 */
#include "module.h"
#include "ksyms.h"
#include "console.h"
#include <stdint.h>

/* --- minimal ELF64 (relocatable) ----------------------------------------- */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} Elf64_Shdr;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} Elf64_Sym;

typedef struct { uint64_t r_offset; uint64_t r_info; int64_t r_addend; } Elf64_Rela;

#define ET_REL        1
#define EM_X86_64     62
#define SHT_PROGBITS  1
#define SHT_SYMTAB    2
#define SHT_RELA      4
#define SHT_NOBITS    8
#define SHF_ALLOC     0x2
#define SHN_UNDEF     0
#define STT_SECTION   3
#define ELF64_R_SYM(i)   ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i)  ((uint32_t)((i) & 0xffffffffu))
#define ELF64_ST_TYPE(i) ((i) & 0xf)
#define R_X86_64_64    1
#define R_X86_64_PC32  2
#define R_X86_64_PLT32 4
#define R_X86_64_32    10
#define R_X86_64_32S   11

#define MOD_AREA_SZ 65536
#define MOD_MAX_SEC 64
/* Executable scratch for the loaded module. In the kernel image's BSS, which is
 * mapped RWX (the kernel LOAD segment is RWX), so code copied here can run. */
static uint8_t mod_area[MOD_AREA_SZ] __attribute__((aligned(64)));
static int     mod_loaded;

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

long module_load(const void *image, unsigned long len) {
    if (mod_loaded) return -1;                         /* one module slot (MVP) */
    const uint8_t *base = (const uint8_t *)image;
    if (len < sizeof(Elf64_Ehdr)) return -2;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
    if (!(eh->e_ident[0] == 0x7f && eh->e_ident[1] == 'E'
       && eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F')) return -3;
    if (eh->e_type != ET_REL || eh->e_machine != EM_X86_64) return -4;
    if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf64_Shdr)) return -5;
    int nsh = eh->e_shnum;
    if (nsh <= 0 || nsh > MOD_MAX_SEC) return -6;
    if (eh->e_shoff + (uint64_t)nsh * sizeof(Elf64_Shdr) > len) return -7;
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(base + eh->e_shoff);

    /* 1) place + copy each allocatable section into mod_area (0 = not loaded). */
    static uint64_t secbase[MOD_MAX_SEC];
    for (int i = 0; i < nsh; i++) secbase[i] = 0;
    uint32_t used = 0;
    for (int i = 0; i < nsh; i++) {
        if (!(sh[i].sh_flags & SHF_ALLOC)) continue;
        if (sh[i].sh_type != SHT_PROGBITS && sh[i].sh_type != SHT_NOBITS) continue;
        uint64_t align = sh[i].sh_addralign ? sh[i].sh_addralign : 1;
        used = (uint32_t)((used + align - 1) & ~(align - 1));
        if ((uint64_t)used + sh[i].sh_size > MOD_AREA_SZ) return -8;
        uint8_t *dst = mod_area + used;
        if (sh[i].sh_type == SHT_PROGBITS) {
            if (sh[i].sh_offset + sh[i].sh_size > len) return -9;
            for (uint64_t b = 0; b < sh[i].sh_size; b++) dst[b] = base[sh[i].sh_offset + b];
        } else {
            for (uint64_t b = 0; b < sh[i].sh_size; b++) dst[b] = 0;   /* .bss */
        }
        secbase[i] = (uint64_t)(uintptr_t)dst;
        used += (uint32_t)sh[i].sh_size;
    }

    /* find the symbol table + its string table. */
    int symi = -1;
    for (int i = 0; i < nsh; i++) if (sh[i].sh_type == SHT_SYMTAB) { symi = i; break; }
    if (symi < 0) return -10;
    if (sh[symi].sh_offset + sh[symi].sh_size > len || sh[symi].sh_entsize != sizeof(Elf64_Sym)) return -11;
    const Elf64_Sym *syms = (const Elf64_Sym *)(base + sh[symi].sh_offset);
    int nsym = (int)(sh[symi].sh_size / sizeof(Elf64_Sym));
    uint32_t stri = sh[symi].sh_link;
    if (stri >= (uint32_t)nsh) return -12;
    const char *strtab = (const char *)(base + sh[stri].sh_offset);

    /* 2+3) apply relocations. */
    for (int i = 0; i < nsh; i++) {
        if (sh[i].sh_type != SHT_RELA) continue;
        uint32_t tgt = sh[i].sh_info;                  /* section these relocs patch */
        if (tgt >= (uint32_t)nsh || secbase[tgt] == 0) continue;   /* target not loaded (e.g. .eh_frame) */
        if (sh[i].sh_offset + sh[i].sh_size > len || sh[i].sh_entsize != sizeof(Elf64_Rela)) return -13;
        const Elf64_Rela *rel = (const Elf64_Rela *)(base + sh[i].sh_offset);
        int nrel = (int)(sh[i].sh_size / sizeof(Elf64_Rela));
        for (int r = 0; r < nrel; r++) {
            uint32_t si  = ELF64_R_SYM(rel[r].r_info);
            uint32_t typ = ELF64_R_TYPE(rel[r].r_info);
            if (si >= (uint32_t)nsym) return -14;
            const Elf64_Sym *s = &syms[si];
            uint64_t S;
            if (s->st_shndx == SHN_UNDEF) {            /* import: resolve by name in the kernel */
                const char *nm = strtab + s->st_name;
                S = ksym_addr(nm);
                if (!S) { kprintf("[module] unresolved symbol: %s\n", nm); return -15; }
            } else if (ELF64_ST_TYPE(s->st_info) == STT_SECTION) {
                if (s->st_shndx >= (uint32_t)nsh || secbase[s->st_shndx] == 0) return -16;
                S = secbase[s->st_shndx];
            } else if (s->st_shndx < (uint32_t)nsh && secbase[s->st_shndx]) {
                S = secbase[s->st_shndx] + s->st_value;
            } else {
                S = s->st_value;                       /* SHN_ABS / non-alloc */
            }
            uint64_t P = secbase[tgt] + rel[r].r_offset;
            int64_t  A = rel[r].r_addend;
            switch (typ) {
                case R_X86_64_64:   *(uint64_t *)(uintptr_t)P = S + (uint64_t)A; break;
                case R_X86_64_PC32:
                case R_X86_64_PLT32:*(int32_t  *)(uintptr_t)P = (int32_t)((int64_t)(S + (uint64_t)A) - (int64_t)P); break;
                case R_X86_64_32:   *(uint32_t *)(uintptr_t)P = (uint32_t)(S + (uint64_t)A); break;
                case R_X86_64_32S:  *(int32_t  *)(uintptr_t)P = (int32_t)(int64_t)(S + (uint64_t)A); break;
                default: kprintf("[module] unhandled reloc type %u\n", typ); return -17;
            }
        }
    }

    /* 4) find mod_init and call it. */
    uint64_t init = 0;
    for (int i = 0; i < nsym; i++) {
        if (syms[i].st_name == 0 || syms[i].st_shndx == SHN_UNDEF) continue;
        const char *nm = strtab + syms[i].st_name;
        if (streq(nm, "mod_init") && syms[i].st_shndx < (uint32_t)nsh && secbase[syms[i].st_shndx]) {
            init = secbase[syms[i].st_shndx] + syms[i].st_value;
            break;
        }
    }
    if (!init) return -18;
    mod_loaded = 1;
    int (*entry)(void) = (int (*)(void))(uintptr_t)init;
    return (long)entry();
}

/* The kernel's built-in demo module: build/testmod.ko, incbin'd by mod_blob.asm. */
extern const uint8_t testmod_ko_start[];
extern const uint8_t testmod_ko_end[];

long module_load_builtin(void) {
    return module_load(testmod_ko_start, (unsigned long)(testmod_ko_end - testmod_ko_start));
}
