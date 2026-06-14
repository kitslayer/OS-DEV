# Milestone 70 — review-driven syscall/crypto hardening

**Goal:** act on the 7th review of the crypto + syscall surface. No new
features here — just fixing real safety bugs the review found, because a
"lock this file" tool that silently *eats* your data is worse than no tool.

## What the review caught

**C1 (critical) — `crypt` truncated files larger than 16 KB.** `SYS_crypt`
reads the file into a fixed `cbuf[16384]`, encrypts, and writes it back. For a
file ≥16 KB, `vfs_read` fills the buffer, we encrypt those 16 KB, and
`vfs_write` writes *only* 16 KB back — permanently discarding the rest, while
returning success. Silent data loss. Fix: detect `cn >= sizeof(cbuf)` and
refuse (`-2`) without writing, rather than truncate. (Chunked streaming would
let it handle big files, but CTR with a file-wide counter needs care across
chunks; refusing is the safe, honest minimum.)

**S1 — `sha256` ignored the caller's buffer length.** The handler always wrote
65 bytes (64 hex + NUL) to the user pointer regardless of how big that buffer
was. A caller passing a smaller buffer would get a 65-byte write into it. Fix:
the handler now requires `rdx >= 65`; `sys_sha256()` gained a `max` argument and
the shell passes `sizeof(hex)`.

**S2 — `sappend`/`snum` underflowed when `max == 0`.** Both looped on
`n < max - 1`; with `max == 0` (an `int`), `max - 1` is `-1` and the comparison
ran away. Fix: loop on `n + 1 < max`, which is equivalent for sane sizes and
safe for `max <= 0`. These two helpers back `sysinfo`/`df`/`ps`/`resolve`, so
the fix hardens all of them at once.

**S3 — `resolve` wrote 1–2 bytes past the buffer.** It did
`buf[n++] = '.'/'\n'` then `buf[n] = 0` with no bound check. Fix: guard the
separator write with `n + 1 < max` and bail early if `max <= 0`.

## Verifying it didn't break the good path

The `crypt` round-trip is still byte-exact for normal files: from an earlier
integration test, `x.txt` ("integration test data", 21 bytes,
`sha256 = d7340cd5…6550bc`) → `crypt x.txt pw` scrambles it
(`sha256 = 77f828b0…`) → `crypt x.txt pw` again restores it
(`sha256 = d7340cd5…6550bc`). All four fixes are guard conditions that change
*only* the out-of-bounds / overflow / oversize cases; the in-bounds path
produces identical output. Clean build, clean headless boot (networking,
FAT32, HTTP GET, USB all up, zero panics).

## Why this matters

Six prior reviews fixed concurrency, recursion, and input bugs; this one is the
first to catch **silent data loss** in a tool a user would actually trust with
a file. The lesson that keeps repeating: every fixed-size kernel buffer that
touches a user pointer or a file is a bounds bug waiting to happen — read into
it *and* write out of it with the same care.

## Files
- `kernel/syscall.c` — `SYS_crypt` oversize guard, `SYS_sha256` length check,
  `sappend`/`snum` underflow fix, `SYS_resolve` bounds
- `user/ulib.c`, `user/ulib.h`, `user/shell.c` — `sys_sha256` gains a `max` arg
