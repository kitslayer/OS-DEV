# Milestone 131 — HTTPS from the shell (`get`/`wget` over TLS)

**Goal:** the TLS 1.3 client (milestone 127) lived only inside the browser's
kernel-side fetch worker. This exposes it to **userspace** so the shell's `get`
and `wget` can fetch and download over HTTPS, not just plain HTTP — tying the TLS
work to the command line.

## What changed

- **`SYS_https` (syscall 30)** — `(host, path, buf, max)`, same shape as
  `SYS_http`. The handler calls `tls_get(...)` with an RNG seed taken from
  `timer_ticks()` (so userspace needn't supply one) and `sti` so the timer runs
  during the handshake.
- **`sys_https` ulib wrapper** — identical to `sys_http` (4th arg via `r10`).
- **`get` / `wget`** now parse an optional `http://` or `https://` scheme: a
  bare host is HTTP (unchanged), `https://host/path` routes through `sys_https`.
  So `get https://example.com` prints the TLS-fetched response, and
  `wget https://host/path out` saves the body to the FAT32 disk over TLS.

## A concurrency fix this forced

`tls_get` uses large shared `static` buffers, safe only because the single fetch
worker called it serially. Exposing it via `SYS_https` means the **shell** can now
enter `tls_get` while the **browser worker** is also inside it — two concurrent
calls would corrupt the shared buffers. So `tls_get` is now a thin wrapper that
**serializes** with an interrupt-guarded `busy` flag (a second concurrent caller
gets `-1`), wrapping the real work in `tls_get_inner`.

## Verified

`get https://example.com` from the shell printed the live TLS response:
Cloudflare headers (`cf-cache-status: HIT`, `CF-RAY: …`) followed by the full
`<!doctype html> … Example Domain … <a href="https://iana.org/domains/example">
Learn more</a>` HTML, then a fresh prompt. No panics. (The browser HTTPS path is
unchanged and still works.)

## Files
- `kernel/include/syscall.h` — `SYS_https` (30)
- `kernel/syscall.c` — dispatch to `tls_get`
- `kernel/tls.c` — `tls_get` serialization wrapper around `tls_get_inner`
- `user/ulib.c`, `user/ulib.h` — `sys_https`
- `user/shell.c` — `get`/`wget` scheme parsing + HTTPS routing
