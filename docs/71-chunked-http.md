# Milestone 71 — HTTP chunked transfer-encoding decode

**Goal:** make the browser robust to `Transfer-Encoding: chunked` responses so
chunk-size hex lines never leak into a rendered page.

## The problem

`http_get` returns the raw response and the browser strips headers at the first
`\r\n\r\n`, then parses the body. That's correct for the common framing
(`Content-Length`, or read-until-close). But an HTTP/1.1 server — and many CDNs —
can answer with a **chunked** body even when we ask in HTTP/1.0:

```
1a4\r\n            <- chunk size in hex
<420 bytes>\r\n
2f\r\n             <- next chunk
<47 bytes>\r\n
0\r\n\r\n           <- terminating zero-size chunk
```

Without decoding, those `1a4` / `2f` / `0` size lines render as stray text
sprinkled through the page.

## The fix

Two small static helpers in `kernel/browser.c`, applied in `browser_poll`
*before* `parse_html`:

- `is_chunked(raw, hdr_end)` — case-insensitive scan of the header region for a
  `Transfer-Encoding:` line whose value contains `chunked`.
- `dechunk(body, len)` — decodes in place: parse the hex size (ignoring any
  `;ext` chunk extension), copy that many bytes, skip the trailing CRLF, repeat
  until the zero-size chunk. It compacts toward the front (`memmove`, dest ≤ src)
  and is tolerant of truncation (we may stop mid-stream on the time budget).

The body is decoded only when the headers actually say chunked, so the common
`Content-Length` path is byte-for-byte unchanged.

## Verifying

I pulled the two functions out verbatim and unit-tested them on the host against
the RFC's classic example and a few edge cases — **all pass**:

| case | input | decoded |
|------|-------|---------|
| RFC example | `4\r\nWiki\r\n5\r\npedia\r\nE\r\n in\r\n\r\nchunks.\r\n0\r\n\r\n` | `Wikipedia in\r\n\r\nchunks.` |
| chunk extension | `5;foo=bar\r\nhello\r\n0\r\n\r\n` | `hello` |
| uppercase hex + multi | `A\r\n0123456789\r\n3\r\nabc\r\n0\r\n\r\n` | `0123456789abc` |
| truncated mid-data | `10\r\nonlyfivech` (no terminator) | `onlyfivech` |
| detection | true / false / mixed-case headers | correct |

**Honest note:** every plain-HTTP site I probed (info.cern.ch, neverssl.com,
httpforever.com, gnu.org) correctly returns `Content-Length` for our HTTP/1.0
request — servers are *supposed* to avoid chunked for 1.0 clients. So this is
defensive code for the misbehaving-CDN case; the unit test is the verification,
not a live fetch. It also makes a future HTTP/1.1 upgrade safe, since 1.1 servers
chunk far more often.

## Files
- `kernel/browser.c` — `is_chunked`, `dechunk`, and the `browser_poll` hook
