#!/bin/sh
# Host-side regression + fuzz test of the AML namespace parser (kernel/acpi_aml.c),
# under AddressSanitizer + UndefinedBehaviorSanitizer.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

$CC -O2 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
    -Ikernel/include kernel/acpi_aml.c tests/acpiaml/test_acpiaml.c -o "$OUT/acpiamltest"
"$OUT/acpiamltest"
echo "acpiamltest: OK"
