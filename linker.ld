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
        *(.text*)
    }

    .rodata : ALIGN(4K) {
        *(.rodata*)
    }

    .data : ALIGN(4K) {
        *(.data*)
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
