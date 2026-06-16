#!/bin/sh
# Host-side regression + fuzz test of the WAV header parser (kernel/wav.c),
# under AddressSanitizer + UndefinedBehaviorSanitizer.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

$CC -O2 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
    -Ikernel/include kernel/wav.c tests/wav/test_wav.c -o "$OUT/wavtest"
"$OUT/wavtest"
echo "wavtest: OK"
