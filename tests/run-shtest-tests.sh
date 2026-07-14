#!/bin/sh
# Build the shell's test / [ ] conditional evaluator (user/shtest.h) for the host
# with ASan+UBSan and run its regression test. The evaluator is pure (the
# sh_test_file file-probe hook is mocked, shmath's sh_var stubbed), so it's
# unit-testable off-target like the other extracted shell parsers (shmath/shsplit/
# shquote/…). Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'building shell test/[ ] evaluator (ASan+UBSan)...'
$CC -std=gnu11 -O1 $SAN -Iuser tests/shtest/shtest_test.c -o /tmp/osdev_shtest_test
echo "running test/[ ] regression..."
if /tmp/osdev_shtest_test; then
    echo "PASS: shell test / [ ] evaluator (=/==/!=, -eq..-ge, -z/-n, file tests, ! , -a/-o, ASan/UBSan clean)"
else
    echo "FAIL: shtest test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
