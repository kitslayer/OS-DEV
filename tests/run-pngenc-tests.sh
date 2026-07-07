#!/bin/sh
# Build the from-scratch PNG encoder (kernel/png_encode.c) for the host with
# ASan+UBSan and round-trip it through the decoder (kernel/png.c, which uses
# kernel/inflate.c) over solid/gradient/noise/odd-size images — the encoded PNG
# must decode back to the exact original RGB. Also checks outcap/scratchcap
# enforcement (returns -1, no overflow). Reuses the DEFLATE compressor
# (kernel/deflate.c) for the IDAT body. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host PNG encoder + decoder (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -Ikernel/include -DDEFLATE_HOST \
    tests/png/png_encode_test.c kernel/png_encode.c kernel/png.c kernel/inflate.c kernel/deflate.c \
    -o /tmp/osdev_pngenc_test
echo "running PNG encoder round-trip + bounds test..."
if /tmp/osdev_pngenc_test; then
    echo "PASS: PNG encoder (round-trips exact RGB, bounds enforced, ASan/UBSan clean)"
else
    echo "FAIL: PNG encoder test aborted (round-trip mismatch or ASan/UBSan memory error)"; exit 1
fi

# Interop: the round-trip above only proves the encoder agrees with OUR decoder.
# The test wrote /tmp/x.png "for the external interop loader" but nothing checked
# it. Prove it's a STANDARD PNG a real decoder must accept — validate the magic +
# every chunk CRC + that the IDAT is a genuine zlib stream of the right size,
# using only the Python stdlib. Skipped cleanly if python3 is unavailable.
if command -v python3 >/dev/null 2>&1 && [ -f /tmp/x.png ]; then
    echo "interop check: standard-PNG validation of /tmp/x.png (png_encode output)..."
    if python3 - <<'PY'
import zlib, struct, binascii
d = open("/tmp/x.png","rb").read()
assert d[:8]==b'\x89PNG\r\n\x1a\n', "bad PNG magic"
i=8; idat=b''; W=H=0; bitdepth=color=0
while i < len(d):
    ln = struct.unpack('>I', d[i:i+4])[0]; t = d[i+4:i+8]; body = d[i+8:i+8+ln]
    crc = struct.unpack('>I', d[i+8+ln:i+12+ln])[0]
    assert binascii.crc32(t+body)&0xFFFFFFFF == crc, "bad CRC for chunk "+t.decode('latin1')
    if t==b'IHDR': W,H,bitdepth,color = struct.unpack('>IIBB', body[:10])
    elif t==b'IDAT': idat += body
    elif t==b'IEND': break
    i += 12+ln
bpp = {0:1,2:3,3:1,4:2,6:4}[color]                # bytes/pixel by colour type
raw = zlib.decompress(idat)                       # a non-standard IDAT raises here
assert len(raw) == (1+W*bpp)*H, "raw size %d != %d"%(len(raw),(1+W*bpp)*H)
print("  OK: %dx%d colour=%d, magic+all chunk CRCs valid, IDAT is standard zlib, size exact"%(W,H,color))
PY
    then
        echo "PASS: png_encode output is a standard PNG (CRCs + zlib IDAT validated by Python stdlib)"
    else
        echo "FAIL: png_encode output is not a standard PNG"; exit 1
    fi
else
    echo "skip: python3 unavailable (or /tmp/x.png missing), PNG interop check omitted"
fi
