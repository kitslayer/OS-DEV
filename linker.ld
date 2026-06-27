/*
 * linker.ld — lay out the kernel ELF for the multiboot loader.
 *
 * The kernel is loaded at physical 1 MiB (0x100000), the conventional spot
 * for a multiboot kernel (below that is BIOS/legacy junk). The .multiboot
 * section is placed first so the magic header lands within the first 8 KiB
 * of the file, which the loader requires.
 *
 * For now virtual == physical (we run identity-mapped). When we move to a
 * proper higher-half kernel later, this is the file that changes.
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

KERNEL_LOAD_ADDR = 1M;

SECTIONS
{
    . = KERNEL_LOAD_ADDR;

    .boot : {
        *(.multiboot)
    }

    .text : ALIGN(4K) {
        _kimage_start = .;          /* start of the read-only code+rodata image (measured-boot, M1096) */
        *(.text*)
    }

    .rodata : ALIGN(4K) {
        _srodata = .;               /* W^X: page-aligned start of read-only data (vmm_harden_kernel) */
        *(.rodata*)
        _kimage_end = .;            /* end of .text+.rodata: a deterministic span covering kernel code + the embedded app ELFs */
    }

    .data : ALIGN(4K) {
        _sdata = .;                 /* W^X: page-aligned start of writable data */
        *(.data*)
    }

    /* Executable scratch for the eBPF JIT (kernel/bpf.c) and the loadable-module
     * loader (kernel/module.c): both emit/relocate machine code into these buffers
     * and then jump to it, so this region must stay executable. W^X
     * (vmm_harden_kernel) marks the REST of .data/.bss no-execute but re-marks
     * [_jitexec_start, _jitexec_end) RWX. Page-aligned both ends so it is a whole
     * number of pages. NOLOAD (like .bss): the buffers are written before use. */
    .jitexec (NOLOAD) : ALIGN(4K) {
        _jitexec_start = .;
        *(.jitexec*)
        . = ALIGN(4K);
        _jitexec_end = .;
    }

    /* Low-physical, page-aligned BSS placed BEFORE the big .bss arenas, so any
     * buffer here is guaranteed to sit within the low 16 MiB of physical RAM.
     * This is for ISA DMA (the 8237 / floppy), whose DMA controller can only
     * address 24 bits (16 MiB): the kernel image's huge static arenas (the JS
     * arena alone is 44 MiB) push kernel_end past 16 MiB, so NO free PMM frame
     * is low enough — a reserved low buffer in the image is the way to get one.
     * Kept tiny (a couple of pages) so it stays comfortably under 16 MiB even as
     * .text/.rodata/.data grow. NOLOAD: zero-initialised at runtime like .bss. */
    .lowbss (NOLOAD) : ALIGN(4K) {
        *(.lowbss*)
    }

    .bss : ALIGN(4K) {
        *(COMMON)
        *(.bss*)
    }

    kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.note*)
        *(.comment)
    }
}
