#!/bin/sh
# Host-side crash-consistency proof for the write-ahead journal (kernel/journal.c,
# M1864), under AddressSanitizer + UndefinedBehaviorSanitizer. Injects a power
# loss at every write of a transaction (a small txn, a full 62-block txn, and
# 4000 random txns) and asserts the recovered on-disk state is always all-old or
# all-new -- never torn. journal_test.c #includes kernel/journal.c directly, so
# the exact kernel code is what's exercised.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
$CC -O2 -g -std=gnu11 -Wall -Wextra $SAN -Ikernel/include \
    tests/journal_test.c -o "$OUT/journaltest"

"$OUT/journaltest"
echo "journaltest: OK"
