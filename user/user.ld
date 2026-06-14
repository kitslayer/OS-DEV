/*
 * user.ld — link a userspace program.
 *
 * Base 0x40000000 (1 GiB): deliberately above the kernel's low identity map
 * (which only covers 0..1 GiB with supervisor-only huge pages), so user pages
 * don't collide with it and can be mapped with the USER permission bit.
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x40000000;
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
}
