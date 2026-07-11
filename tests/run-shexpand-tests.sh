#!/bin/sh
# Build the shell's parameter/variable expander (user/shexpand.h: expand_vars) for
# the host with ASan+UBSan and run its regression + fuzz test. The pass is pure apart
# from stubbed hooks (vget/sh_laststatus/sh_var), so it's unit-testable off-target
# like shbrace/shgrep/shmath/shsplit. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'building shell parameter expansion (ASan+UBSan)...'
$CC -std=gnu11 -O1 $SAN -Iuser tests/shexpand/shexpand_test.c -o /tmp/osdev_shexpand_test
echo "running parameter-expansion regression + fuzz..."
if /tmp/osdev_shexpand_test; then
    echo "PASS: shell parameter expansion (\$NAME/\${VAR:-}/\${VAR#pat}/\${#VAR}/\$((...)), ASan/UBSan clean)"
else
    echo "FAIL: shexpand test aborted (expansion mismatch or ASan/UBSan memory error)"; exit 1
fi
