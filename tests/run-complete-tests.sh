#!/bin/sh
# Build the terminal's Tab-completion core (kernel/complete.h) for the host with
# ASan+UBSan and run its regression test. The logic is pure (it works over a
# plain array of name strings), so it's unit-testable off-target like the shell's
# shgrep/shmath extractions and the browser's cssprop/url/htmlentity ones.
# Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'building Tab-completion core (ASan+UBSan)...'
$CC -std=gnu11 -O1 -Wall -Wextra $SAN -Ikernel tests/complete/complete_test.c -o /tmp/osdev_complete_test
echo "running Tab-completion regression..."
if /tmp/osdev_complete_test; then
    echo "PASS: Tab-completion core (prefix match, longest-common-prefix, dir '/' handling, ASan/UBSan clean)"
else
    echo "FAIL: complete test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
