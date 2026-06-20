/*
 * elf_test.c — host-side regression + fuzz test of the ELF64 loader
 * (ASan + UBSan). It #includes elf.c to reach the loader and its static
 * validators (elf_check_header / elf_check_phdr), and stubs elf.c's only
 * external deps — the VMM/PMM — with an mmap-backed "guest memory" model so the
 * loader's writes to guest virtual addresses land in real pages.
 *
 * Two halves:
 *   1. Regression: build one known-good minimal ELF and confirm the validators
 *      report it correctly AND a full elf_load round-trips — the entry point is
 *      right, the file bytes land at p_vaddr, and the .bss tail is zeroed.
 *   2. Fuzz: feed the validators every truncated prefix, every single-byte
 *      corruption, and many random buffers of the known-good image. A malformed
 *      ELF must never out-of-bounds read (ASan catches that) and must never be
 *      mis-accepted with a segment that escapes the image or the user range.
 *
 * The fuzz half calls only the pure validators (no memory is mapped), so it is
 * safe to throw arbitrary bytes at it; the write path is exercised only by the
 * deterministic known-good load. Exit 0 = pass.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

/* ---- stubs for elf.c's external deps: an mmap-backed guest-memory model ----
 * vmm_map mmaps the page at `virt` (MAP_FIXED) so elf.c's memset/memcpy to a
 * guest address hit real memory; vmm_translate reports whether we've mapped it;
 * pmm_alloc_frame hands out dummy non-zero physical addresses. */
#define PG 4096ull
#define MAXMAP 8192
static uint64_t g_mapped[MAXMAP];
static int      g_nmapped;

static int is_mapped(uint64_t pg) {
    for (int i = 0; i < g_nmapped; i++) if (g_mapped[i] == pg) return 1;
    return 0;
}
uint64_t vmm_translate(uint64_t virt) {
    uint64_t pg = virt & ~(PG - 1);
    return is_mapped(pg) ? pg : 0;     /* non-zero physaddr if mapped */
}
int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)phys; (void)flags;
    uint64_t pg = virt & ~(PG - 1);
    /* Re-mapping an already-mapped page (e.g. the loader's W^X re-protect pass)
     * only rewrites the PTE flags in the real VMM — it never re-allocates or
     * zeroes the frame. Model that by keeping the existing page contents. */
    if (is_mapped(pg)) return 0;
    void *p = mmap((void *)pg, PG, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); abort(); }
    if (g_nmapped < MAXMAP) g_mapped[g_nmapped++] = pg;
    return 0;
}
static uint64_t g_frame = 0x100000;
uint64_t pmm_alloc_frame(void) { g_frame += PG; return g_frame; }

static void unmap_all(void) {
    for (int i = 0; i < g_nmapped; i++) munmap((void *)g_mapped[i], PG);
    g_nmapped = 0;
}

#include "elf.c"

/* ---- build a known-good minimal ELF64 in `buf` (returns total size) ----
 * One PT_LOAD segment: 16 bytes of file data at p_vaddr=0x40000000 with a
 * larger p_memsz so the loader must zero a .bss tail. */
#define LOAD_VADDR 0x40000000ull
#define DATA_OFF   0x1000
#define DATA_LEN   16
#define SEG_MEMSZ  0x2000          /* spans 2 pages; tail past DATA_LEN is .bss */

static uint64_t build_good_elf(uint8_t *buf, uint64_t cap) {
    uint64_t total = DATA_OFF + DATA_LEN;
    if (cap < total) abort();
    memset(buf, 0, total);

    Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
    eh->e_ident[0] = 0x7F; eh->e_ident[1] = 'E';
    eh->e_ident[2] = 'L';  eh->e_ident[3] = 'F';
    eh->e_ident[4] = 2;    /* ELFCLASS64 */
    eh->e_ident[5] = 1;    /* little-endian */
    eh->e_ident[6] = 1;    /* version */
    eh->e_type = 2;        /* ET_EXEC */
    eh->e_machine = 0x3E;  /* x86-64 */
    eh->e_version = 1;
    eh->e_entry = LOAD_VADDR;
    eh->e_phoff = sizeof(Elf64_Ehdr);
    eh->e_ehsize = sizeof(Elf64_Ehdr);
    eh->e_phentsize = sizeof(Elf64_Phdr);
    eh->e_phnum = 1;

    Elf64_Phdr *ph = (Elf64_Phdr *)(buf + eh->e_phoff);
    ph->p_type = PT_LOAD;
    ph->p_flags = 5;       /* R+X */
    ph->p_offset = DATA_OFF;
    ph->p_vaddr = LOAD_VADDR;
    ph->p_paddr = LOAD_VADDR;
    ph->p_filesz = DATA_LEN;
    ph->p_memsz = SEG_MEMSZ;
    ph->p_align = PG;

    for (int i = 0; i < DATA_LEN; i++) buf[DATA_OFF + i] = (uint8_t)(i + 1);
    return total;
}

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", (msg)); fails++; } } while (0)

static void test_regression(void) {
    uint8_t buf[DATA_OFF + DATA_LEN];
    uint64_t sz = build_good_elf(buf, sizeof(buf));

    /* validators report it correctly */
    uint64_t phoff, entry; uint16_t phnum, phentsize;
    CHECK(elf_check_header(buf, sz, &phoff, &phnum, &phentsize, &entry) == 1, "header rejected");
    CHECK(entry == LOAD_VADDR, "wrong entry from header");
    CHECK(phnum == 1, "wrong phnum");
    elf_seg_t seg;
    CHECK(elf_check_phdr(buf, sz, phoff, phentsize, 0, &seg) == 1, "phdr rejected");
    CHECK(seg.vaddr == LOAD_VADDR && seg.memsz == SEG_MEMSZ &&
          seg.file_off == DATA_OFF && seg.filesz == DATA_LEN, "wrong segment fields");

    /* full load round-trips: entry, copied bytes, zeroed .bss */
    uint64_t e = elf_load(buf, sz);
    CHECK(e == LOAD_VADDR, "elf_load returned wrong entry");
    const uint8_t *mem = (const uint8_t *)LOAD_VADDR;
    int data_ok = 1;
    for (int i = 0; i < DATA_LEN; i++) if (mem[i] != (uint8_t)(i + 1)) data_ok = 0;
    CHECK(data_ok, "segment file bytes not copied to p_vaddr");
    int bss_ok = 1;
    for (uint64_t i = DATA_LEN; i < SEG_MEMSZ; i++) if (mem[i] != 0) bss_ok = 0;
    CHECK(bss_ok, ".bss tail not zeroed");
    unmap_all();

    /* obvious rejects */
    CHECK(elf_load(buf, 4) == 0, "tiny image accepted");
    uint8_t bad[64]; memset(bad, 0, sizeof(bad));
    CHECK(elf_load(bad, sizeof(bad)) == 0, "non-ELF magic accepted");

    printf("regression: %s\n", fails ? "FAILURES" : "ok (validators + load round-trip)");
}

/* Run both validators over an (image,len); for a fuzz body this must never
 * OOB-read (ASan) and any accepted segment must stay within the image+range. */
static void exercise(const uint8_t *img, uint64_t len) {
    uint64_t phoff, entry; uint16_t phnum, phentsize;
    if (elf_check_header(img, len, &phoff, &phnum, &phentsize, &entry) != 1) return;
    /* header promised the table is in-bounds */
    if (phoff > len || (uint64_t)phnum * phentsize > len - phoff) { printf("  FAIL: header table OOB\n"); fails++; return; }
    for (uint16_t i = 0; i < phnum; i++) {
        elf_seg_t s;
        int r = elf_check_phdr(img, len, phoff, phentsize, i, &s);
        if (r != 1) continue;
        /* an accepted segment must be readable from the image and inside the user range */
        CHECK(s.file_off <= len && s.filesz <= len - s.file_off, "accepted segment reads past image");
        CHECK(s.vaddr >= PG && s.memsz <= ELF_VADDR_MAX && s.vaddr <= ELF_VADDR_MAX - s.memsz,
              "accepted segment escapes user range");
        CHECK(s.filesz <= s.memsz, "accepted segment filesz > memsz");
    }
}

static void test_fuzz(void) {
    uint8_t good[DATA_OFF + DATA_LEN];
    uint64_t sz = build_good_elf(good, sizeof(good));

    /* every truncated prefix — allocate exactly `len` so ASan red-zones any
     * read past the end of a short image */
    for (uint64_t len = 0; len <= sz; len++) {
        uint8_t *t = malloc(len ? len : 1);
        memcpy(t, good, len);
        exercise(t, len);
        free(t);
    }

    /* every single-byte corruption of the header + program-header region */
    uint64_t hdr = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
    for (uint64_t pos = 0; pos < hdr; pos++) {
        for (int v = 1; v <= 256; v++) {
            uint8_t *t = malloc(sz);
            memcpy(t, good, sz);
            t[pos] ^= (uint8_t)v;
            exercise(t, sz);
            free(t);
        }
    }

    /* random buffers of assorted sizes */
    srand(1234);
    for (int trial = 0; trial < 200000; trial++) {
        uint64_t len = (uint64_t)(rand() % 200);
        uint8_t *t = malloc(len ? len : 1);
        for (uint64_t i = 0; i < len; i++) t[i] = (uint8_t)rand();
        /* make ~half look like real ELFs so the phdr path is reached */
        if ((trial & 1) && len >= 4) { t[0]=0x7F; t[1]='E'; t[2]='L'; t[3]='F'; }
        exercise(t, len);
        free(t);
    }

    printf("fuzz: truncations + single-byte corruptions + 200000 random buffers -> %s\n",
           fails ? "FAILURES" : "all clean");
}

/* Load a real shipped app binary through the kernel's loader and confirm it is
 * accepted and laid out in the user range — a regression guard that every ELF
 * the OS ships stays loadable (catches a linker-script/toolchain change that
 * would push a segment out of range or otherwise trip the validators). */
static void test_real_elf(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  (skip %s: %s)\n", path, "cannot open"); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); printf("  (skip %s: empty)\n", path); return; }
    uint8_t *buf = malloc((size_t)sz);          /* exact size: ASan red-zones overreads */
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return; }
    fclose(f);

    uint64_t entry = elf_load(buf, (uint64_t)sz);
    char msg[256];
    snprintf(msg, sizeof(msg), "%s rejected by elf_load", path);
    CHECK(entry != 0, msg);
    snprintf(msg, sizeof(msg), "%s entry out of user range", path);
    CHECK(entry >= PG && entry < ELF_VADDR_MAX, msg);

    unmap_all();   /* release this app's pages before the next (all load at 0x40000000) */
    free(buf);
}

int main(int argc, char **argv) {
    test_regression();
    test_fuzz();
    int reals = 0;
    for (int i = 1; i < argc; i++) { test_real_elf(argv[i]); reals++; }
    if (reals) printf("real binaries: loaded %d shipped app ELF(s) through elf_load\n", reals);
    if (fails) { printf("FAIL: %d check(s) failed\n", fails); return 1; }
    printf("PASS: ELF64 loader (validators + load round-trip%s, fuzz/corrupt safe, ASan/UBSan clean)\n",
           reals ? " + real app binaries" : "");
    return 0;
}
