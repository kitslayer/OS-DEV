# Milestone 46 — Filesystem write hardening

**Goal:** fix two real `fat32_write` bugs a code review caught — both about what
happens when you write a file that already exists or when the disk is full.

## Bug 1 — overwriting duplicated the entry and leaked clusters

`fat32_write` allocated a fresh cluster chain and then added a *new* directory
entry, using "the first free **or deleted** slot." It never looked for an
existing entry of the same name. So writing `PAGE.TXT` (or any file) a second
time created a **second** directory entry with the same name pointing at a new
chain — and the old chain was orphaned (still marked allocated, referenced by
nothing). Repeated saves slowly leaked the disk and left duplicate names.

This wasn't hypothetical: the browser's save-page (milestone 42) writes
`PAGE.TXT` every time you press `s`, so each save duplicated it.

**Fix:** `fat32_write` now deletes any existing entry for the name first
(reusing `fat32_delete`, which frees the old chain and frees the slot), so a
write *replaces* rather than *duplicates*. Verified: writing `t.txt` three times
leaves exactly **one** `t.txt` entry (confirmed both in `ls` and in the raw disk
image), holding the latest contents.

## Bug 2 — leaked clusters on a failed write

If `alloc_cluster` ran out mid-chain (disk full), or `add_entry` failed (the
directory's clusters were packed), `fat32_write` returned `-1` having already
allocated clusters — and never freed them. A new `free_chain()` helper now
releases the partial chain on every failure path, mirroring what `fat32_mkdir`
already did.

## Why it matters

These are the kind of bugs that don't crash anything — they silently corrupt
free-space accounting and the directory over time. Catching them needed someone
(here, a review subagent) to reason about the *failure* and *overwrite* paths,
not the happy path that obviously worked. The header comment that still claimed
the driver was "read-only" was corrected too.

## Files
- `kernel/fat32.c` — `fat32_write` deletes-then-writes; `free_chain` on failure
