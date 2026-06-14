# Milestone 136 — network info in the System Monitor

**Goal:** small dashboard polish. The OS is now genuinely networked (browses the
real HTTPS web), so the System Monitor — which showed memory, tasks, and uptime —
gains a **Network** section showing the machine's IPv4 address and gateway. A
quick, at-a-glance "am I online and what's my address" readout for a connected OS.

## How

Two trivial accessors in `net.c` (`net_ip()`, `net_gateway()`) expose the
addresses (previously file-local `static const`s), declared in `net.h`. The
`KIND_SYSMON` draw path formats them as `IP a.b.c.d  gw a.b.c.d` below the
uptime line; the window grew from 190→240 px tall to fit. No new state, no
per-frame cost beyond formatting two dotted-quads.

## Verified

Booted with the Monitor open (temporary auto-open for the headless screenshot,
then reverted): it shows **Memory** (8/255 MiB bar), **Tasks** (2), **Uptime**
(6s), and **Network — `IP 10.0.2.15  gw 10.0.2.2`**, all laid out cleanly. No
panics.

## Files
- `kernel/net.c`, `kernel/include/net.h` — `net_ip()` / `net_gateway()`
- `kernel/desktop.c` — `KIND_SYSMON` draws the Network section; taller window
