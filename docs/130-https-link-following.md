# Milestone 130 — clicking through a real HTTPS site

**Goal:** milestone 127 made the browser *fetch* one HTTPS page. Actually
*browsing* a site means following links — and trying that on `gnu.org` surfaced
two bugs that, fixed, turn "fetch a page" into "navigate a site".

## Bug 1 — relative links dropped to plain HTTP

`goto_href` resolves a relative link (`/philosophy/philosophy.html`) against the
current page by concatenating `host + path`. But it dropped the **scheme**, and
`worker_fetch` selects TLS purely by the `https://` prefix — so following any
relative link on an HTTPS page silently fetched it over **plain HTTP**. Fixed by
carrying the current page's scheme into the resolved URL (and, as a bonus,
handling protocol-relative `//host/path` links). A link on an HTTPS page now
stays HTTPS.

## Bug 2 — only the *first* network request per boot worked

After the fix, following the link produced the right URL
(`https://www.gnu.org/philosophy/philosophy.html`) but the fetch **failed** —
deterministically, only on the *second* request in a session. Logging pinned it to
`dns_resolve` failing on the second call.

The cause: `arp_resolve`, `ping`, and `dns_resolve` waited for their reply with a
**fixed try count** (`for tries < 8/20`), each try calling `recv_timeout`. After a
15 KB TLS transfer the NIC's RX ring is full of stale TCP packets (trailing data,
the server's FIN, the close_notify response). `recv_timeout` returns each of those
*instantly*, so the fixed budget is burned skipping stale packets before the real
ARP/DNS reply arrives — and `dns_resolve` calls `arp_resolve` first, so the 8-try
ARP was the first to starve.

Fixed by making all three loops **deadline-bounded** (the pattern `tcp_connect`
already used): keep draining-and-matching until a wall-clock deadline, so any
number of stale packets are skipped while we still wait for the reply.

## Verified

Live, in one session: `browse https://www.gnu.org` (15228 B, rendered), then
`n`-select the **PHILOSOPHY** nav link and press Enter → the URL bar shows
`https://www.gnu.org/philosophy/philosophy.html` (scheme preserved) and the page
renders (17248 B, title "Philosophy of the GNU Project", nav shows `= PHILOSOPHY =`
as current). Two successful TLS handshakes back-to-back, no panics. The boot-time
ICMP ping and HTTP path still work (regression-clean).

## Review hardening (subagent review #25)

A review of the milestone-128–130 changes confirmed the browser text/URL changes
(`copy_decoded`, `decode_utf8`, the `parse_html` fold, `goto_href`'s `newurl`
writes incl. the new protocol-relative branch) are all bounds-safe, and that the
deadline loops terminate correctly. It found one MEDIUM, pre-existing but more
reachable now that we drain more packets: `dns_resolve`'s answer parser walked
the name/RR offsets and `memcpy`'d the address using packet-controlled offsets
with **no bound against the received length** — an out-of-bounds stack read on a
malformed reply. Fixed by bounding every offset against `dmax = len - 42` before
indexing `d[o]` or reading a fixed-size field (the "every length off the network
is adversarial" rule).

## Files
- `kernel/browser.c` — `goto_href` preserves the current scheme on relative links
  + protocol-relative `//host` handling
- `kernel/net.c` — `arp_resolve` / `ping` / `dns_resolve` receive loops are now
  deadline-bounded instead of fixed-try; `dns_resolve` answer parsing is bounded
  against the received length
