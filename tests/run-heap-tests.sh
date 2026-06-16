#!/bin/sh
# Host-side regression test of the userspace allocator (user/umalloc.c) over a
# mock sbrk, under AddressSanitizer + UndefinedBehaviorSanitizer.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
# Rename the allocator entry points so they don't collide with libc's malloc
# (still used by the harness + the sanitizer runtime), and point sbrk at the mock.
$CC -O2 -g -std=gnu11 -Wall -Wextra $SAN \
    -Dsbrk=test_sbrk -Dmalloc=t_malloc -Dfree=t_free -Dcalloc=t_calloc -Drealloc=t_realloc \
    -c user/umalloc.c -o "$OUT/umalloc.o"
$CC -O2 -g -std=gnu11 -Wall -Wextra $SAN \
    -c tests/heap/test_heap.c -o "$OUT/test_heap.o"
$CC $SAN "$OUT/umalloc.o" "$OUT/test_heap.o" -o "$OUT/heaptest"

"$OUT/heaptest"
echo "heaptest: OK"
