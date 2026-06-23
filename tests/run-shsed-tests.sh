#!/bin/sh
# Build the shell's sed substitution engine (user/shsed.h) for the host with
# ASan+UBSan and run its regression + fuzz test. sed_sub is pure (no syscalls)
# and shares the grep regex matcher (shgrep.h), so it's unit-testable off-target
# like the other extracted shell parsers (shgrep/shmath/shquote). Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building shell sed engine (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -Iuser tests/shsed/shsed_test.c -o /tmp/osdev_shsed_test
echo "running sed-engine regression + fuzz..."
if /tmp/osdev_shsed_test; then
    echo "PASS: shell sed engine (s/RE/REPL/g, &/escapes, ASan/UBSan clean)"
else
    echo "FAIL: shsed test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
