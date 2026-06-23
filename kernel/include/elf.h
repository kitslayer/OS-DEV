/* elf.h — minimal ELF64 loader. */
#pragma once
#include <stdint.h>

/* Load the PT_LOAD segments of an ELF64 image (already in memory) into the
 * current address space as user pages, and return the entry point address.
 * `maxsz` bounds the readable image (file offsets/sizes are validated against
 * it; pass ~0ull for a fully trusted in-kernel image). Returns 0 on a bad or
 * out-of-bounds image. */
uint64_t elf_load(const void *image, uint64_t maxsz);

/* The on-disk byte extent of an ELF image (max p_offset+p_filesz over PT_LOAD,
 * capped at maxsz; 0 on a bad header). Lets measured-boot hash the exact image
 * bytes of an app even when it was spawned with maxsz = ~0 (trusted embedded). */
uint64_t elf_image_size(const void *image, uint64_t maxsz);
