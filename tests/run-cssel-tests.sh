#!/bin/sh
# Build the CSS simple-selector parser (kernel/include/cssel.h) for the host with
# ASan+UBSan and run its regression + fuzz test. sel_parse is pure (no kernel deps),
# extracted from browser.c so the bounds-safety of its scan over untrusted <style>
# selectors can be unit-tested off-target — like the codebase's other extracted
# parsers (cssprop/url/htmlentity/shgrep). Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building CSS selector parser (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -Ikernel/include tests/cssel/cssel_test.c -o /tmp/osdev_cssel_test
echo "running sel_parse regression + fuzz..."
if /tmp/osdev_cssel_test; then
    echo "PASS: CSS selector parser sel_parse (tag/.class/#id/[attr], fail-closed, bounds; ASan/UBSan clean)"
else
    echo "FAIL: cssel test aborted (parse contract mismatch or ASan/UBSan memory error)"; exit 1
fi
