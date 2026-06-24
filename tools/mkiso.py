#!/usr/bin/env python3
"""mkiso.py — build a tiny but spec-valid ISO 9660 image to exercise the
read-only iso9660.c driver (M1131). We have no mkisofs/genisoimage in this
environment, so we lay out the volume by hand: a 16-sector system area, a
Primary Volume Descriptor, a descriptor-set terminator, a root directory and a
"DOCS" subdirectory, and two file extents. 2048-byte logical sectors.

    tools/mkiso.py build/osdev.iso
"""
import struct, sys

SECTOR = 2048

def both32(n): return struct.pack('<I', n) + struct.pack('>I', n)   # both-endian uint32
def both16(n): return struct.pack('<H', n) + struct.pack('>H', n)   # both-endian uint16

def dir_record(name: bytes, extent: int, length: int, is_dir: bool) -> bytes:
    fi = len(name)
    rec_len = 33 + fi
    if rec_len % 2:           # records are padded to an even length
        rec_len += 1
    r = bytearray(rec_len)
    r[0] = rec_len
    r[1] = 0                  # extended attribute record length
    r[2:10] = both32(extent)
    r[10:18] = both32(length)
    r[18:25] = bytes([120, 1, 1, 0, 0, 0, 0])   # 2020-01-01 00:00:00 GMT
    r[25] = 0x02 if is_dir else 0x00            # file flags (bit1 = directory)
    r[28:32] = both16(1)      # volume sequence number
    r[32] = fi
    r[33:33 + fi] = name
    return bytes(r)

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "build/osdev.iso"

    ROOT, DOCS, HELLO, INFO = 18, 19, 20, 21      # logical sector of each extent
    hello = b"Hello from a real ISO 9660 filesystem!\nMounted read-only in OS-DEV.\n"
    info  = b"This file lives in a subdirectory on the CD image.\n"

    # --- root directory (sector 18): ".", "..", DOCS/, HELLO.TXT;1 ---
    root = bytearray(SECTOR)
    o = 0
    for rec in (dir_record(b'\x00', ROOT, SECTOR, True),
                dir_record(b'\x01', ROOT, SECTOR, True),
                dir_record(b'DOCS', DOCS, SECTOR, True),
                dir_record(b'HELLO.TXT;1', HELLO, len(hello), False)):
        root[o:o + len(rec)] = rec; o += len(rec)

    # --- DOCS subdirectory (sector 19): ".", "..", INFO.TXT;1 ---
    docs = bytearray(SECTOR)
    o = 0
    for rec in (dir_record(b'\x00', DOCS, SECTOR, True),
                dir_record(b'\x01', ROOT, SECTOR, True),
                dir_record(b'INFO.TXT;1', INFO, len(info), False)):
        docs[o:o + len(rec)] = rec; o += len(rec)

    # --- Primary Volume Descriptor (sector 16) ---
    pvd = bytearray(SECTOR)
    pvd[0] = 1                      # type: PVD
    pvd[1:6] = b'CD001'             # standard identifier
    pvd[6] = 1                      # version
    pvd[40:72] = b'OSDEV_CD'.ljust(32, b' ')      # volume identifier
    pvd[80:88] = both32(22)         # volume space size (total logical blocks)
    pvd[120:124] = both16(1)        # volume set size
    pvd[124:128] = both16(1)        # volume sequence number
    pvd[128:132] = both16(SECTOR)   # logical block size
    pvd[156:156 + 34] = dir_record(b'\x00', ROOT, SECTOR, True)   # root directory record

    # --- Volume Descriptor Set Terminator (sector 17) ---
    vdst = bytearray(SECTOR)
    vdst[0] = 255
    vdst[1:6] = b'CD001'
    vdst[6] = 1

    def filled(data):
        s = bytearray(SECTOR); s[:len(data)] = data; return bytes(s)

    img = bytes(16 * SECTOR) + bytes(pvd) + bytes(vdst) + bytes(root) + bytes(docs) \
        + filled(hello) + filled(info)
    with open(out, "wb") as f:
        f.write(img)
    print("wrote %s (%d bytes, %d sectors)" % (out, len(img), len(img) // SECTOR))

if __name__ == "__main__":
    main()
