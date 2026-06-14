# Milestone 135 — TCP partial-segment fix + real-web validation

**Goal:** validate the HTTPS browser against real sites and fix what that turns up.

## Real-web validation

Browsed live over the from-scratch TLS 1.3 client:
- **example.com** (Cloudflare, AES-128-GCM) — renders.
- **www.gnu.org** (Let's Encrypt, multi-record cert flight) — renders, and its
  relative nav links follow correctly while staying HTTPS (milestone 130).
- **text.npr.org** — a **live news site renders beautifully**: the "Text-Only
  Version" header and real current headlines as clickable links. Notably its CDN
  negotiated **ChaCha20-Poly1305** (suite 0x1303), exercising that AEAD end-to-end
  against a real server, and the smart-quotes in headlines like "California's"
  fold to ASCII via the milestone-128/129 work.

Known limitation found: very large/fast CDN responses (e.g. `lite.cnn.com`) can
defeat our **minimal TCP** (in-order only, no out-of-order buffering / SACK /
retransmit). A robust TCP is the remaining frontier for "any site"; NPR's
equally-large cert chain works, so normal pages and text-oriented sites are fine.

## The TCP bug this surfaced

Reviewing `tcp_read` for the CNN failure exposed a real latent bug: when an
incoming segment doesn't fully fit the caller's buffer, it copied only `n` bytes
but advanced `theirseq` (i.e. ACKed) by the **full** `dlen`. The unaccepted tail
`(dlen - n)` was thus acknowledged but never delivered — silent data loss / stream
desync whenever a read buffer fills mid-segment.

Fix: advance `theirseq` (and ACK) by **`n`** — the bytes actually delivered — and
leave the remainder unacknowledged so the peer resends it on the next read. Full
segments (`n == dlen`, the normal case) are unchanged, so the working sites are
unaffected (re-verified: NPR still renders). The peer-FIN logic already keys off
`seq + dlen == theirseq`, which now correctly stays false after a partial accept.

## Review hardening (subagent review #27)

A review confirmed the partial-accept fix is correct (no loss/dup, forward
progress holds) but found a related **MEDIUM**: the FIN branch tore the connection
down *regardless* of whether the segment's data fully fit — so a FIN-bearing
segment that only partially fit (buffer full mid-segment) dropped its tail, and an
*out-of-order* FIN segment tore down prematurely. Fixed by gating the whole FIN
teardown on `seq + dlen == theirseq` (all of this segment's data consumed; also
covers a pure FIN with `dlen == 0`); a partial or out-of-order FIN now leaves the
connection up so the tail + FIN are redelivered and handled on the next read.

## Files
- `kernel/net.c` — `tcp_read` acknowledges only the bytes it actually accepted;
  the FIN is acted on only once the segment's data is fully consumed
