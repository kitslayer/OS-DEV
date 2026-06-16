#!/bin/sh
# Build the from-scratch DEFLATE/gzip compressor (kernel/deflate.c) for the host
# with ASan+UBSan and round-trip it through the decoder (kernel/inflate.c) over a
# wide range of inputs (empty/all-same/repetitive/random/text, sizes 0..~200KB).
# Also checks outcap enforcement and, if `gzip` is present, that the real system
# `gunzip -t` accepts a gz_deflate output (validates CRC32 + gzip format). Exit
# 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host deflate compressor + inflate decoder (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -Ikernel/include \
    tests/deflate/deflate_test.c kernel/deflate.c kernel/inflate.c \
    -o /tmp/osdev_deflate_test
echo "running compressor round-trip + bounds test..."
if /tmp/osdev_deflate_test; then
    echo "PASS: deflate compressor (round-trips exact, bounds enforced, ASan/UBSan clean)"
else
    echo "FAIL: deflate test aborted (round-trip mismatch or ASan/UBSan memory error)"; exit 1
fi

# Optional interop: prove the gzip framing/CRC are real by feeding a gz_deflate
# stream to the system gzip. Skipped cleanly if gzip is unavailable.
if command -v gunzip >/dev/null 2>&1; then
    echo "interop check: system 'gunzip -t' on a gz_deflate stream..."
    cat > /tmp/osdev_mkgz.c <<'EOF'
#include <stdint.h>
#include <stdio.h>
int gz_deflate(const uint8_t *, int, uint8_t *, int);
static uint8_t in[200000], out[300000];
int main(void){
    const char *t="DEFLATE interop: the quick brown fox. ";
    int n=0; while(n<80000){const char *q=t; while(*q&&n<80000) in[n++]=*q++;}
    int gz=gz_deflate(in,n,out,sizeof out);
    if(gz<0) return 1;
    FILE *f=fopen("/tmp/osdev_interop.gz","wb"); fwrite(out,1,gz,f); fclose(f);
    return 0;
}
EOF
    $CC -std=gnu11 -O2 -Ikernel/include /tmp/osdev_mkgz.c kernel/deflate.c -o /tmp/osdev_mkgz
    /tmp/osdev_mkgz
    if gunzip -t /tmp/osdev_interop.gz; then
        echo "PASS: system gunzip -t accepts gz_deflate output (CRC32 + format OK)"
    else
        echo "FAIL: system gunzip rejected gz_deflate output"; exit 1
    fi
else
    echo "skip: gzip not installed, interop check omitted"
fi
