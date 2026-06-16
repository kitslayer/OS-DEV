# M435 — FAT32 read-path robustness: reject out-of-range cluster numbers

The untrusted-input security audit ([docs/422](422-untrusted-input-security-audit.md),
[docs/434](434-browser-parser-security-audit.md)) covered everything reachable from the
**network** — HTML/CSS, TLS, X.509, image decoders, DNS. This pass extends the same
discipline to a different trust boundary: the **disk**. It matters here because the
test harness mounts an arbitrary `fat.img`, and disk corruption is a *known* scenario —
heavy repeated writes can corrupt the persisted image over many boots (see WHATS-NEXT).

## What was already safe

Reading `kernel/fat32.c` + `kernel/ata.c`, the cluster-chain read path is already
**memory-safe** against a corrupt/cyclic FAT:

- **No infinite loop:** `fat_step` carries a step counter and bails (returns `EOC`)
  after `total_clusters + 2` steps — a cyclic chain can't hang the kernel inside a
  syscall.
- **No kernel-memory OOB:** every cluster read targets a fixed `sec[SECSZ]` (512-byte)
  buffer, regardless of the cluster number. `ata_read` doesn't bound the LBA, but
  `select_lba` masks it to 28 bits and QEMU's IDE errors on an out-of-range LBA (so
  `ata_read` returns −1, which the loops handle). The read can go to the *wrong* sector,
  but never outside the 512-byte buffer.

## The gap, and the fix

The loops gated only on `cl < EOC` (`0x0FFFFFF8`). A corrupt directory entry can point a
chain at a cluster that is large but still `< EOC` (e.g. `0x5000000`) — past the end of
the volume. That isn't a memory bug, but it makes the driver **silently read wrapped /
garbage in-range data** instead of recognizing the chain as broken.

Added a predicate and used it on the two **read-path** loops (`walk_dir`, `fat32_read`):

```c
/* A legitimate data cluster is in [2, total_clusters+2). */
static int cluster_in_range(uint32_t cl) {
    return cl >= 2 && cl < EOC && (!total_clusters || cl < total_clusters + 2);
}
```

`while (cl < EOC)` → `while (cluster_in_range(cl))`. Now an out-of-range cluster cleanly
ends the chain (the file/dir reads as truncated/empty) rather than fetching garbage. The
`cl >= 2` half also closes a latent `cluster_to_sector(0)` underflow
(`data_start + (0-2)*sec_per_clus`) for a first-cluster-0 entry.

The **write path** (`add_entry`) was deliberately left unchanged — the FAT32 write path
is fragile and protected (see the memory/WHATS-NEXT notes); the read guard makes no
assumption the writer must uphold.

## Why it's safe (strictly additive)

A review confirmed the kernel's `total_clusters` (`(tot_sec − meta) / sec_per_clus`,
the FAT-spec count) equals the `mkfatfs` builder's cluster count **byte-for-byte** — no
off-by-one, so the highest legitimate cluster (`total_clusters + 1`) passes the guard.
Therefore on a healthy disk every in-chain cluster satisfies `cluster_in_range`, making
the loops behave **identically** to the old `cl < EOC` test; only an out-of-range
cluster (which a healthy volume never produces) is now rejected. The predicate is pure
(no new read/write) and can only make a loop exit *earlier*, so termination and the
existing cycle guard are unaffected.

Reviewed SHIP. Verified live: the Files panel still lists every file and a file's
contents still read/​render correctly (no regression) — clean build + boot.
