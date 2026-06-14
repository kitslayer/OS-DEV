# Milestone 126 — a reusable TCP stream API (foundation for HTTPS)

**Goal:** the TLS crypto + certificate toolkit (milestones 119–125) is complete
and verified; turning it into HTTPS needs the TLS protocol to run over TCP. But
the TCP code was **embedded inside `http_get`** (connection state inline, the
receive filter hardcoded to port 80). So this refactors it into a small reusable
stream API that both HTTP and a future TLS layer can drive.

## The API

A `tcp_conn` holds the connection state (peer IP + gateway MAC, ports, sequence
numbers, an `up` flag), and four functions operate on it:

- **`tcp_connect(c, ip, port)`** — the SYN → SYN-ACK → ACK handshake to any port.
- **`tcp_write(c, data, len)`** — send, segmenting to ≤1400-byte chunks.
- **`tcp_read(c, out, max, ticks)`** — read in-order stream data (ACKing,
  handling dup-ACKs and the peer's FIN), returning bytes (0 on timeout, −1 once
  closed).
- **`tcp_close(c)`** — send FIN.

The receive filter is now parameterized by port, so a TLS connection on **443**
works the same as HTTP on 80. `http_get` is reimplemented on top of these four
calls (connect → write the request → loop `tcp_read` → close), which is both a
simplification and the regression test for the API.

## Verified

Booted and **fetched `example.com` over the live internet** through the new API:
the browser rendered the "Example Domain" page (heading, paragraph, "Learn more"
link, 797 bytes). No panics — so the refactor preserves the working HTTP path
while exposing the reusable TCP stream the TLS handshake will sit on.

## Next

With the verified crypto/cert toolkit and now a TCP stream API, the remaining
work for HTTPS is the **TLS 1.3 record layer + handshake** itself: ClientHello,
parse ServerHello, run the X25519 + HKDF key schedule, decrypt the server's
handshake flight (AES-GCM/ChaCha20), validate the certificate chain (X.509 + RSA
/ECDSA verify), send Finished, then exchange application data — all over a
`tcp_conn`.

## Files
- `kernel/net.c`, `kernel/include/net.h` — `tcp_conn` + `tcp_connect`/`tcp_write`/
  `tcp_read`/`tcp_close`; `http_get` reimplemented on them
