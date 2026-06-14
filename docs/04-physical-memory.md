# Milestone 4 — Physical memory manager

**Goal:** find out how much RAM the machine has, and be able to hand out and
reclaim physical memory one 4 KiB **frame** at a time. Every later subsystem
(paging, the heap, processes) gets its memory from here.

## Getting the memory map (`boot/boot.asm`, `kernel/include/multiboot.h`)

We can't just assume how much RAM exists or which ranges are usable (some are
reserved for hardware/firmware). The bootloader knows, and Multiboot is how it
tells us:

1. We set **bit 1** in the Multiboot header flags to request a memory map.
2. The bootloader leaves a pointer to a `multiboot_info` structure in a
   register; `boot.asm` forwards it to `kmain` as the first argument (`rdi`).
3. That struct contains `mmap_addr`/`mmap_length` — a list of regions, each
   with a base, length, and type (type 1 = available RAM).

One quirk: each map entry's `size` field excludes itself, so you advance by
`size + 4` — the entries are not a fixed stride.

QEMU reported ~127 MiB available (128 MiB minus reserved holes).

## The allocator: a bitmap (`kernel/pmm.c`)

We track frames with a **bitmap** — one bit per 4 KiB frame, `1` = used. It's
tiny: 128 MiB of RAM needs only a 4 KiB bitmap. Setup is a three-step dance:

1. **Size it.** Scan the map for the highest available address → number of
   frames → bitmap size. Park the bitmap in memory right after the kernel
   (the linker exports `kernel_end`).
2. **Mark everything used**, then **free** exactly the frames inside
   available-RAM regions. (Starting from "all used" means anything the firmware
   didn't explicitly mark available — holes, MMIO — stays off-limits by
   default.)
3. **Re-reserve** address 0 through the end of the bitmap. That covers the low
   BIOS area, the kernel image itself, and the bitmap's own storage — none of
   which we may ever hand out.

`pmm_alloc_frame()` scans for a clear bit (with a "next free" hint so we don't
rescan from 0 every time); `pmm_free_frame()` clears the bit. We proved reuse
works: freeing the middle of three frames and reallocating returned that exact
frame.

## Why frames, not bytes?

The CPU's paging hardware works in 4 KiB pages, so physical memory is naturally
managed in 4 KiB units. Sub-page allocations (`kmalloc(40)`) are the job of the
**heap** (M6), which will be built *on top of* this allocator.

## Files
- `kernel/include/multiboot.h` — the info + mmap structures
- `kernel/pmm.c` — the bitmap frame allocator
- `boot/boot.asm` — requests the map, forwards the pointer to `kmain`
