# Milestone 5 — Virtual memory + higher-half

**Goal:** take control of the page tables. Be able to map any virtual address to
any physical frame with chosen permissions, translate addresses, and tear
mappings down. This is what makes per-process memory, memory protection, and
the heap possible.

## How x86_64 paging works

A 48-bit virtual address is sliced into four 9-bit indices and a 12-bit offset:

```
[ 47..39 | 38..30 | 29..21 | 20..12 | 11..0 ]
   PML4     PDPT      PD       PT     offset
```

`CR3` points at the PML4. Each table has 512 eight-byte entries; an entry holds
a physical frame address plus flag bits (present, writable, user, huge, NX).
To translate, the CPU walks PML4 → PDPT → PD → PT and adds the offset. To *map*
a page, we do the same walk in software, creating any missing table on the way,
and write the final entry. (`kernel/vmm.c`)

## Reaching the tables themselves

Editing a page table means writing to its frame — but we only have *virtual*
addressing. We use the low-1 GiB **identity map** from boot: every table frame
comes from the PMM, which only returns low physical RAM, so its physical
address is already a valid virtual address. (`phys_to_table()` is just a cast.)

## `invlpg` — the TLB

The CPU caches recent translations in the TLB. After changing an entry we must
`invlpg` that address, or the stale cached translation wins. Forgetting this is
a classic "I changed the mapping but nothing happened" bug.

## The higher-half direct map (HHDM)

We map **all** physical RAM a second time at `HHDM_BASE`
(`0xFFFF8000_00000000`), using cheap 2 MiB pages. Then any frame is reachable
at `hhdm(phys)` regardless of whether it's in the low identity window. We
proved it: writing through the HHDM alias and reading through the identity
address showed the same physical frame.

This region lives in the upper ("higher") half of the canonical address space —
the half conventionally reserved for the kernel, separate from the lower half
where user programs will live (M8).

## A deliberate simplification

A "true" higher-half kernel also **relocates the running kernel's code** into
the upper half (so even `kmain`'s instruction pointer is a high address). Doing
that means rewriting the early boot assembly to run position-independently and
jumping into the high mapping — and a single wrong address there is a silent
triple-fault reboot with no output to debug. To keep momentum, our kernel keeps
*executing* in its identity map while we add higher-half *mappings* on top. The
trade-off shows up in M8: cleanly isolating each process's address space is
easier with a relocated kernel; we'll use a single-address-space model instead
and note what full isolation would require.

## API (`kernel/vmm.c`)
- `vmm_map(virt, phys, flags)` / `vmm_map_huge(...)` — create a mapping
- `vmm_unmap(virt)` — remove one
- `vmm_translate(virt)` — resolve to a physical address (or 0)
- `hhdm(phys)` — the higher-half alias of a physical frame
