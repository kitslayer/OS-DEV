# Milestone 115 — Tab completion of filenames

**Goal:** a real shell convenience — press **Tab** to complete a filename from
the current directory instead of typing it out. Every interactive command that
reads a line gets it, because it lives in the kernel's line discipline (the same
`app_sys_read` that already does echo, backspace, and up/down history recall).

![typing `cat read`+Tab completes to `cat README.TXT`, which then prints the file](osdev-tab-complete.png)

## How it works

When `app_sys_read` receives a Tab (`\t`):

1. It finds the **current word** (back to the last space) as the partial.
2. If the partial is non-empty and contains no `/`, it lists the current
   directory (`vfs_list`) and finds entries whose name **starts with the
   partial, case-insensitively** (you can type `read` and match `README.TXT`).
3. On a **unique** match it erases the typed partial and writes the entry's
   **canonical** name in its place (so the case is corrected to the real on-disk
   8.3 name, and any directory marker `/` is dropped), echoing the new
   characters. Zero or multiple matches do nothing.

Because completion runs against the kernel's current FAT directory — which the
shell's `cd` keeps in sync via `SYS_chdir` — it always reflects where you are.

## Verified

Typed `cat read`, pressed **Tab** → the line became `cat README.TXT`; pressing
Enter then ran it and printed the file's contents. Case-insensitive matching and
canonical-name substitution both confirmed. No panics.

The shell line editor now has: echo, backspace, up/down-arrow **history**
(milestone 109 exposes it; the ring has always driven recall), and **Tab
completion**.

## Files
- `kernel/app.c` — the Tab case in `app_sys_read`
