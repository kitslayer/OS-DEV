/*
 * user.ld — link a userspace program.
 *
 * Base 0x40000000 (1 GiB): deliberately above the kernel's low identity map
 * (which only covers 0..1 GiB with supervisor-only huge pages), so user pages
 * don't collide with it and can be mapped with the USER permission bit.
 *
 * Two loadable segments so the kernel can enforce W^X (M796): code+constants
 * go in a read-only + executable segment, mutable data in a writable +
 * non-executable one. The data segment is page-aligned so no page is shared
 * between the two and the loader can set per-page permissions unambiguously.
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(0x5);   /* PF_R | PF_X */
    data PT_LOAD FLAGS(0x6);   /* PF_R | PF_W */
}

SECTIONS
{
    . = 0x40000000;
    .text   : { *(.text*) }            :text
    .rodata : { *(.rodata*) }          :text
    . = ALIGN(0x1000);
    .data   : { *(.data*) }            :data
    .bss    : { *(.bss*) *(COMMON) }   :data

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
