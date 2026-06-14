# Milestone 132 — DNS + ARP caches (faster multi-page browsing)

**Goal:** optimizations for the HTTPS browsing just built. Every page fetch and
every followed link calls `dns_resolve(host)` (a UDP query + reply) and, inside
`tcp_connect`, `arp_resolve(gateway)` (a broadcast + reply). So clicking through a
site re-resolves the *same* host and re-ARPs the *same* gateway on every
connection. Two small caches remove those round-trips on repeat visits.

## How

A fixed 8-entry cache in `net.c` maps `host → IP` with an expiry tick:

- `dns_resolve` first scans the cache and, on a **fresh** matching entry, returns
  the cached IP immediately — no ARP, no query, no wait.
- On a successful real lookup it stores the answer with a 60 s TTL
  (`DNS_TTL = 6000` ticks at 100 Hz), round-robin-evicting the oldest slot.
- Host comparison is exact full-string (`dns_streq` requires both strings to end
  together, so a truncated/over-long name can never produce a false hit); names
  longer than the 63-char slot are simply not cached.

A cache miss or an expired entry just falls through to the normal query, so the
cache is purely an accelerator — correctness is unchanged.

An identical 4-entry **ARP cache** (`ip → MAC`, 60 s TTL, round-robin evict) sits
in `arp_resolve` the same way: a fresh hit returns the cached MAC with no
broadcast, and `tcp_connect`'s per-connection gateway ARP becomes free after the
first. Same fall-through-on-miss safety.

## Verified

In one session: `browse https://www.gnu.org` (resolves + caches `www.gnu.org`),
then follow the **PHILOSOPHY** link to `https://www.gnu.org/philosophy/philosophy.html`
— the second fetch's DNS is served from cache and the page renders (17248 B), no
regression, no panics. The boot-time ping/HTTP/DNS paths still work.

## Files
- `kernel/net.c` — `dns_cache` + `dns_cache_put`/`dns_streq`; `dns_resolve` checks
  the cache first and stores successful answers. `arp_cache`; `arp_resolve` checks
  it first and stores successful replies.
