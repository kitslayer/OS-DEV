# Milestone 56 — `df` + input fixes (review round)

This milestone adds a small feature and applies fixes from a code review of the
recent input/history work.

![df reporting disk usage, with command history intact above](osdev-df.png)

## `df` — disk free

`df` reports free and total space on the FAT32 volume (here, `4014 KiB free /
4016 KiB total`). The driver computes the volume's data-cluster count at mount
(from the BPB) and `df` walks the FAT counting unallocated entries, scaling by
the cluster size. A new `df` VFS op + `SYS_df` + shell command.

## Review fixes

A review of the arrow-key/history/compositor code surfaced two real bugs:

- **Critical — history recall could erase past the prompt.** `grid_erase` only
  stopped at the top-left of the window, so recalling commands (especially after
  several ↑/↓ or a wrapped line) could blank the prompt and *earlier output*.
  Fixed by recording where the input starts (`cx0,cy0`) when the line read
  begins and clamping the erase to never go before it. Verified: after several
  commands, recalling with ↑/↓ replaces only the current line — the scrollback
  above is untouched.

- **Should-fix — lost-wakeup in the blocking read.** Between "the input queue is
  empty" and actually blocking, the window manager could deliver a key whose
  wake-up was a no-op (the task wasn't blocked *yet*), leaving the task asleep
  with a key — even Enter — queued until the *next* key. Rare (a few-instruction
  window) and self-healing, but real. Fixed by doing the check-and-block with
  interrupts disabled, so no key can slip into that gap.

The review also flagged a possible `input_push` data race (keyboard vs serial
IRQ); that one doesn't apply here because IRQ handlers run on interrupt gates
(interrupts disabled), so the two can't nest — noted and confirmed safe.

## Files
- `kernel/fat32.c` — `fat32_df`, `total_clusters` at mount; `vfs.c`/`vfs.h`,
  `syscall.c`, `user/ulib.c`, `user/shell.c` — `df`
- `kernel/app.c` — `grid_erase` input-start clamp; interrupts-off check-and-block
