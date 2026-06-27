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
/* Executable scratch for the loaded modules. Placed in the .jitexec section, which
 * W^X (vmm_harden_kernel) keeps RWX while the rest of .bss is no-execute, so code
 * copied here can run. A bump allocator hands each loaded module a region; freed
 * wholesale once all modules are unloaded (so insmod/rmmod cycles don't leak it). */
static uint8_t  mod_area[MOD_AREA_SZ] __attribute__((aligned(64), section(".jitexec")));
static uint32_t mod_used;                              /* bump pointer into mod_area */

/* The module registry — what /proc/modules lists and rmmod tears down. */
#define MOD_SLOTS 8
static struct {
    int      used;
    char     name[32];
    uint32_t size;                                     /* bytes of mod_area this module occupies */
    uint64_t exit_addr;                                /* mod_exit(), or 0 if the module has none */
} mods[MOD_SLOTS];

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int mod_count(void) { int n = 0; for (int i = 0; i < MOD_SLOTS; i++) if (mods[i].used) n++; return n; }

/* Load an ET_REL module image (len bytes) under `name`: relocate it, resolve its
 * kernel imports, register it, call mod_init(), and return mod_init's value (or a
 * negative error). */
static long mod_do_load(const void *image, unsigned long len, const char *name) {
    int slot = -1;
    for (int i = 0; i < MOD_SLOTS; i++) if (!mods[i].used) { slot = i; break; }
    if (slot < 0) return -1;                           /* registry full */
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

    /* 1) place + copy each allocatable section into mod_area (0 = not loaded).
     * Start at the global bump pointer so this module sits after any already
     * loaded ones. */
    static uint64_t secbase[MOD_MAX_SEC];
    for (int i = 0; i < nsh; i++) secbase[i] = 0;
    uint32_t region_start = mod_used;
    uint32_t used = mod_used;
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

    /* 4) find mod_init (required) + mod_exit (optional). */
    uint64_t init = 0, exit = 0;
    for (int i = 0; i < nsym; i++) {
        if (syms[i].st_name == 0 || syms[i].st_shndx == SHN_UNDEF) continue;
        if (syms[i].st_shndx >= (uint32_t)nsh || !secbase[syms[i].st_shndx]) continue;
        const char *nm = strtab + syms[i].st_name;
        uint64_t a = secbase[syms[i].st_shndx] + syms[i].st_value;
        if (streq(nm, "mod_init")) init = a;
        else if (streq(nm, "mod_exit")) exit = a;
    }
    if (!init) return -18;

    /* commit: register the slot + advance the bump pointer, THEN call mod_init. */
    mod_used = used;
    mods[slot].used = 1;
    mods[slot].size = used - region_start;
    mods[slot].exit_addr = exit;
    int k = 0; if (name) { while (name[k] && k < (int)sizeof(mods[slot].name) - 1) { mods[slot].name[k] = name[k]; k++; } }
    mods[slot].name[k] = 0;

    int (*entry)(void) = (int (*)(void))(uintptr_t)init;
    return (long)entry();
}

long module_load(const void *image, unsigned long len) {
    return mod_do_load(image, len, "module");
}

/* rmmod: call the module's mod_exit (if any) and free its registry slot. When
 * the last module is unloaded, reset the bump allocator so the area is reusable.
 * Returns 0, or -1 if no such module is loaded. */
int module_unload(const char *name) {
    for (int i = 0; i < MOD_SLOTS; i++) {
        if (mods[i].used && streq(mods[i].name, name)) {
            if (mods[i].exit_addr) { int (*ex)(void) = (int (*)(void))(uintptr_t)mods[i].exit_addr; ex(); }
            mods[i].used = 0;
            if (mod_count() == 0) mod_used = 0;        /* all gone -> reclaim the whole area */
            return 0;
        }
    }
    return -1;
}

/* /proc/modules: one "name size" line per loaded module (lsmod-ish). */
int module_list(char *buf, int max) {
    int p = 0;
    for (int i = 0; i < MOD_SLOTS && p < max - 48; i++) {
        if (!mods[i].used) continue;
        for (const char *s = mods[i].name; *s && p < max - 1; s++) buf[p++] = *s;
        if (p < max - 1) buf[p++] = ' ';
        char t[12]; int n = 0; uint32_t v = mods[i].size;
        if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
        while (n && p < max - 1) buf[p++] = t[--n];
        if (p < max - 1) buf[p++] = '\n';
    }
    if (p < max) buf[p] = 0;
    return p;
}

/* The kernel's built-in demo module: build/testmod.ko, incbin'd by mod_blob.asm. */
extern const uint8_t testmod_ko_start[];
extern const uint8_t testmod_ko_end[];

long module_load_builtin(void) {
    return mod_do_load(testmod_ko_start, (unsigned long)(testmod_ko_end - testmod_ko_start), "testmod");
}
