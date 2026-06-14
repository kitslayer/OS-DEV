# Milestone 6 — Kernel heap (`kmalloc`/`kfree`)

**Goal:** let kernel code allocate arbitrary-sized objects — a 40-byte struct, a
256 KiB buffer — instead of only whole 4 KiB frames. This is the `malloc`/`free`
the rest of the kernel will lean on constantly (task structs, file handles,
driver buffers).

## The gap it fills

- **PMM** gives out whole physical frames (4 KiB).
- **VMM** maps frames to virtual addresses.
- **Heap** sits on top: it maps some pages, then carves them into
  variable-sized blocks.

## Design (`kernel/kheap.c`)

A **singly linked list of contiguous blocks**, each with a tiny header
`{ size, next, free }`. The payload follows the header.

- **`kmalloc`** does **first-fit**: walk the list for a free block big enough.
  If the block is much bigger than requested, **split** it (carve off the
  remainder as a new free block). If nothing fits, **grow** the heap and retry.
- **`kfree`** marks the block free and **coalesces** with the following block if
  it's also free and physically adjacent — this fights fragmentation.
- **Growing**: map more pages (via the VMM, backed by PMM frames) at the end of
  the heap region and append a free block.

The heap lives in its own higher-half virtual window (`0xFFFF9000_…`), distinct
from the HHDM and identity regions, so it can grow without colliding.

## What we proved
- Three allocations returned distinct, increasing addresses; a string and a
  100-int array stored and read back correctly (`arr[99] == 9801`).
- Freeing a block and allocating a smaller one **reused the same address**.
- `kzalloc(256 KiB)` succeeded — forcing the heap to **grow** past its initial
  64 KiB — and returned zeroed memory.

## Limitations (fine for now)
First-fit with a linear scan is O(n) and the headers waste a little space. Real
kernels use slab/buddy allocators for speed and locality. Our version is chosen
for readability; it can be swapped out later without changing callers.

## Files
- `kernel/kheap.c` — the allocator
- builds on `kernel/vmm.c` (mapping) and `kernel/pmm.c` (frames)
