# Milestone 39 — `browse` from the shell

**Goal:** connect the two halves of the OS that hadn't met yet — the **userspace
shell** and the **kernel-side browser**. Type `browse example.com` at the shell
and a browser window opens on that page.

![typing `browse example.com` in the shell opened a browser on that page](osdev-browse-cmd.png)

## Why this is more than a convenience

The shell is a **ring-3 userspace process**; the browser is a **kernel window**
owned by the window manager. A userspace program can't just reach in and create
a WM window — that's the kernel's job. So this needed a proper request path
across the privilege boundary, the same shape the OS already uses elsewhere:

```
shell (ring 3): browse example.com
   └─ sys_browse("example.com")  →  int 0x80
        kernel: SYS_browse → app_browse() enqueues the URL
   window manager (each frame): app_take_browse() → spawn_browser(url)
        → a new KIND_BROWSER window loads the page (async)
```

`app_browse()`/`app_take_browse()` are a tiny ring buffer (mirroring the
existing app-spawn queue): the syscall, running in the shell's context, copies
the URL into kernel memory and queues it; the window manager drains the queue on
its own thread and opens the window. Nothing blocks, and the privilege boundary
stays clean.

## Tested for real

Unlike the link-click path (which the headless monitor can't drive), this one is
fully scriptable: the test types `browse example.com` into the focused shell via
emulated keystrokes, and a browser window duly appears rendering the page — the
shell's own "opening browser:" output is visible behind it.

## Files
- `kernel/app.c` / `app.h` — `app_browse` / `app_take_browse` URL queue
- `kernel/syscall.c`, `kernel/include/syscall.h` — `SYS_browse`
- `user/ulib.c` / `ulib.h`, `user/shell.c` — `sys_browse` + the `browse` command
- `kernel/desktop.c` — drains the queue, `spawn_browser(url)` helper
