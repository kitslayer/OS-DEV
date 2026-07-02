#!/bin/sh
# Build kernel/lib/string.c's mem* primitives on the host with ASan+UBSan
# (#include'd directly, like tests/url/url_test.c does for url.c) and fuzz
# each against a naive, obviously-correct reference implementation --
# especially memmove, whose word-at-a-time fast path has a copy direction to
# get right. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host string.c mem* primitives (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -fno-stack-protector -Ikernel/include \
    tests/string/string_test.c -o /tmp/osdev_string_test
echo "running mem* fuzz vs reference..."
if /tmp/osdev_string_test; then
    :
else
    echo "FAIL: string.c test aborted (ASan/UBSan caught a memory error) or a mismatch"; exit 1
fi
