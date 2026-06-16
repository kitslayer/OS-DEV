# Tests

Host-side regression + fuzz tests for the from-scratch parsers that run
**kernel-side on untrusted input** (a malicious web server, an on-path attacker,
or page scripts) on a **16 KB stack with no guard page** — where an out-of-bounds
access is silent kernel memory corruption. Each suite compiles the *real* kernel
source on the host under **ASan + UBSan** and exercises it with crafted edge
cases + deterministic fuzzing.

## Running

```sh
make check       # run all five suites (~5s total)
make jstest      # JS engine     — tests/js/suite.js vs the golden output
make imgtest     # image decoders — tests/img/img_test.c  (jpeg/png/gif/inflate)
make x509test    # X.509 parser   — tests/x509/x509_test.c
make nettest     # TCP/IP stack   — tests/net/net_test.c  (packet parse + reassembly)
make fstest      # FAT32 read path — tests/fs/fs_test.c   (corrupt/cyclic on-disk structures)
```

`make test` is a *different* target — the headless QEMU boot smoke test.

You can also run the JS suite inside the OS: `js suite.js` (if copied onto the
disk), or the baked-in demos `js`, `js showcase.js`, `js sample.js`.

## What each suite covers

| Suite | Source under test | What it checks |
|-------|-------------------|----------------|
| `jstest`  | `kernel/js.c` | A golden-output regression of ~60 language/stdlib features (core operators, closures/recursion, arrows, default params, arrays + higher-order methods, strings, objects, `JSON`, template literals, `switch`/`for-of`/`do-while`, `try/catch/finally`); ASan/UBSan-clean, ran-to-completion, matches `tests/js/suite.expected`. Includes the M419 `instanceof` + M420 `+` ToPrimitive fixes and the integer-arithmetic / div-by-zero-guard contract. |
| `imgtest` | `jpeg.c` `png.c` `gif.c` `inflate.c` | The M422 JPEG DRI out-of-bounds-read PoC, truncated/bare-magic headers, a 120k-iteration random-bytes fuzz through all three decoders, and a direct 120k DEFLATE fuzz (huffman + LZ77). |
| `x509test`| `kernel/x509.c` | Crafted adversarial DER (4 GB length claim, truncated/indefinite-length, nested headers) + a 200k-iteration fuzz of the `tlv` reader. |
| `nettest` | `kernel/net.c` | 150k random Ethernet/IPv4/TCP frames through `tcp_recv_seg`, and 150k crafted `seq`/`dlen` through the 96 KB `ooo_store` reassembly buffer (far-future/past/wraparound). Stubs the NIC + timer. |
| `fstest`  | `kernel/fat32.c` | **Read path:** a valid minimal FAT32 image then 12k corrupted copies (BPB/FAT/root-dir bytes randomized) through `mount`/`list`/`read`/`find`/`tree` — locks the M435 `cluster_in_range` guard, the cluster-chain cycle guard, and the dir-recursion depth caps (a corrupt/cyclic FAT must never OOB or hang). **Write path:** 8k accumulating `write`/`delete`/`mkdir` ops (the "heavy repeated writes" scenario) — confirms `alloc_cluster`/`add_entry`/`write_fat`/chain-extension are memory-safe (its known fragility is logical/persistence, not OOB). `#include`s fat32.c and stubs the disk (`ata_read`/`ata_write` → an in-memory image) + `vfs_register`. |

## Validated to catch regressions

Each fuzz harness is **verified to fail** when its guard is removed:

- `imgtest` aborts (ASan) if the JPEG DRI `seglen >= 2` guard is removed.
- `x509test` aborts if `tlv`'s `len > end-p` bound is removed.
- `nettest` aborts if `ooo_store`'s `off > OOO_CAP - dlen` bound is removed.
- `fstest` aborts (ASan stack-overflow) if fat32's dir-recursion depth caps are removed (a cyclic directory recurses unbounded).
- `jstest` diffs against the golden, so any output change fails.

## Not covered here

`tls.c`'s handshake-message parsers and the crypto/bignum are intentionally
*not* fuzzed by a committed harness: a naive random fuzz fast-fails at
`x509_parse` / bails without a seeded leaf key (so it exercises nothing), and a
meaningful one needs valid-DER + valid-key seeds — disproportionate for code
that the manual security review covered thoroughly and whose cert DER parser is
already deeply fuzzed by `x509test`. See
[../docs/422-untrusted-input-security-audit.md](../docs/422-untrusted-input-security-audit.md).
