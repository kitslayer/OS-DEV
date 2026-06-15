# 341–344: rounding out the networking toolkit

By this point the OS already browsed the real HTTPS web. This arc added the
*diagnostic* tools that make a network stack usable from a shell — and, while
auditing the stack to build them, corrected a stale belief about the TCP layer.

## What the TCP layer actually does (the "minimal TCP" note was stale)

The project notes long carried "robust out-of-order TCP" as a pending item. A
read of `kernel/net.c` showed it was already done: `tcp_read` buffers
post-gap segments in a 96 KB reorder window (`ooo_store`/`ooo_drain`), uses
wraparound-safe 32-bit sequence comparisons, handles a FIN that arrives out of
order, and re-ACKs the last in-order byte on every gap — a duplicate ACK that
triggers the peer's fast retransmit. `tcp_connect` retransmits the SYN up to
four times. Our *send* side isn't retransmitted, but our sends are tiny request
lines, so that's a low-value gap, not the "minimal TCP" the note implied.

## 342: a deeper NIC RX ring

The real fast-path weakness was below TCP. A CDN can DMA a dozen back-to-back
full-size segments into the e1000 RX ring faster than the single-threaded poll
loop drains them; once the 32-descriptor ring fills, the card drops further
segments and the reorder logic has to wait for a retransmit. Doubling
`RX_COUNT` to 64 doubles that burst headroom. It's purely additive:

- `64 * sizeof(struct rx_desc)` = `64 * 16` = 1024 B, still one 4 KB frame;
- `RDLEN` = 1024 stays 128-byte aligned (the e1000 requirement);
- `rx_buf[RX_COUNT]`, the init loops, `RDT = RX_COUNT-1`, and the
  `rx_cur = (i+1) % RX_COUNT` wrap all derive from the macro.

Cost: 64 × 4 KB = 256 KB of receive buffers (up from 128 KB).

## 341 / 343 / 344: the shell tools

- **`headers <url>`** — a `curl -I`. It reuses the existing `sys_http`/`sys_https`
  fetch but prints only the response header block (up to the blank line), so you
  can see a status code, `Content-Type`, `Server`, or a `Location:` redirect
  that the page-rendering `browse` hides. A 2 KB buffer suffices because headers
  lead the body, which keeps the fetch cheap.

- **`ping <host>`** — `net_ping_host()` DNS-resolves the name and ICMP-echoes the
  address through the **gateway's** MAC (the next hop for any off-LAN target),
  reusing the same `ping()` helper the gateway ping already used. The shell
  resolves first (to print the IP and validate DNS); the DNS cache makes the
  echo's second resolve free.

- **`ifconfig`** — `SYS_netinfo` formats our IP / MAC / gateway / DNS resolver
  into aligned columns on demand (it was previously printed only once at boot).
  A background review rated it ship-shape but flagged that its separator writes
  were unguarded; they now check `n + 1 < max` per write like `SYS_resolve`, so
  the formatter is memory-safe for any buffer size, not just the 128-byte caller.

The result is a coherent story from one shell: `ifconfig` (who am I) → `resolve`
(name → IP) → `ping` (reachable?) → `headers` (what does the server say) →
`get`/`wget` (fetch/download) → `browse` (render it).
