# M419–423 — A comprehensive untrusted-input security audit (and 2 real kernel fixes)

This arc stepped back from adding features and instead **audited the whole
untrusted-input attack surface** of the OS for memory-safety. The threat model:
the browser, TLS client, and network stack all run **kernel-side** on a
**16 KB stack with NO guard page**, parsing bytes that a malicious web server or
on-path attacker fully controls. An out-of-bounds read/write there isn't a
crash — it's silent kernel memory corruption. So every parser that touches
attacker bytes was reviewed (subagent reviews + ASan/UBSan fuzzing of the real
kernel sources) against adversarial input.

## The two attack-surface chains

**Network → TLS → X.509 → crypto:**
- `net.c` — Ethernet/ARP/IP/ICMP/TCP parsing + the 96 KB out-of-order TCP
  reassembly reorder buffer. **Clean** (the e1000 driver clamps the frame length
  so every `len` is the true byte count; IP total-length is clamped to the real
  frame; TCP `dlen` is floored at 0; the reassembly offset bound is written to
  dodge the `off+dlen` overflow trap; all receive loops are deadline-bounded).
- `tls.c` — TLS 1.3 record + handshake parsing. **Clean** (every wire length is
  validated against its enclosing buffer before any index/copy; copies into
  fixed buffers are bounded by the *destination* size; the big transcript/record
  buffers live in BSS, not on the guard-page-less stack).
- `x509.c` — the `tlv()` ASN.1 DER reader. **Clean**, and the reason is subtle:
  DER lengths accumulate into a **`size_t`** (64-bit), which defeats the classic
  from-scratch-parser bug (a 32-bit `int` length would let `0x84 80 00 00 00`
  overflow negative and bypass the `len > remaining` check). It's `size_t`, so
  it can't.
- `bignum.c`/`rsa.c`/`ecdsa.c` — `bn_from_bytes` rejects any input
  `> BN_LIMBS*4` bytes (`return -1`, checked by callers), so a malicious cert
  with an oversized modulus is rejected, never overflowing the fixed limb array.

**Web content → HTML/CSS → images → JS:**
- `browser.c` HTML tokenizer (`parse_html`/`handle_tag`/`decode_entity`) + CSS.
  **Clean** — the key insight: tokens are `(off,len)` slices into the bounded
  `b->text` pool (49 KB, gated on every write), **not** into the raw HTML, so the
  `uint16_t` off/len can't truncate-to-OOB even on a >64 KB page; all
  tag/attribute/entity copies are destination-length-capped; numeric entity refs
  are clamped to `0x10FFFF`; the cursor always advances (no hang).
- `inflate.c`/`png.c`/`gif.c` — DEFLATE / PNG / LZW. **Clean** under 16M+
  ASan/UBSan fuzz iterations (random, mutated, oversized, adversarial): every
  output write bounded to `rgba_sz`/`scr_sz`, every input read bounded to
  `data+len`, size math in `long` (no overflow-to-undersized-buffer), all
  malformed-input loops terminate.
- `js.c` — the JS engine. Verified correct across ~60 features and fails safe on
  all three exhaustion axes (stack depth, heap/arena, malformed syntax). See
  [376-jsrandom-and-browser-sims.md](376-jsrandom-and-browser-sims.md).

## The two real bugs (both kernel-side, both web-triggerable, both ASan-proven)

1. **JPEG DRI out-of-bounds READ (M422, `jpeg.c`).** The DRI (`0xDD`) restart
   marker was handled inline as `j->ri = rd16(j)` — a 2-byte read — but the
   segment-length guard passes when an attacker sets the DRI length field to 2
   or 3 (`seglen` 0 or 1). A JPEG ending in `FF DD 00 02` reads up to 2 bytes
   past `data+len`. Fix: require `seglen >= 2` before `rd16` (a valid DRI is
   always length 4). ASan confirmed both ways: PoC overflows without the fix,
   clean with it; valid DRI still parses.

2. **DNS query-builder kernel-stack OOB WRITE (M423, `net.c`).** `dns_resolve`
   built its query into a stack `uint8_t q[256]` by appending length-prefixed
   labels from the hostname with **no bound on the write index**. A hostname
   > ~238 chars (from a `get`/`wget`/`ping`/browser URL — a userspace→kernel
   path) overflows the stack buffer. Fix: bound each label read to 64 and reject
   (`return -1`) any label that would overflow `q[]`. ASan/UBSan confirmed both
   ways: a 399-char host overflows `q[256]` ("index 256 out of bounds") without
   the fix, returns -1 cleanly with it; `example.com` still builds.

## The lesson (again)

Both bugs are in code that has browsed the real HTTPS web for many sessions —
because real JPEGs carry a valid DRI length and real hostnames are short. The
vulnerability only appears under *adversarial* input, which routine use never
produces. **Adversarial bounds-review of untrusted-input parsers finds what
live testing cannot** — the same lesson the JS-engine reviews taught (a review
caught 3 CRITICAL kernel bugs there), now extended to the whole attack surface.
Nine subagent reviews across this arc; the audit is recorded in the memory file
so the surface isn't needlessly re-reviewed.
