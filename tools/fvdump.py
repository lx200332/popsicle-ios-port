#!/usr/bin/env python3
"""Dump FV/FFS layout of an image for diagnosis."""
import sys
import struct
from pathlib import Path


def dump(path):
    d = Path(path).read_bytes()
    print(f"\n=== {Path(path).name} {len(d)} bytes")
    pos = 0
    while True:
        pos = d.find(b'_FVH', pos)
        if pos < 0:
            break
        base = pos - 0x28
        print(f"  _FVH at 0x{pos:x} -> FV base 0x{base:x}")
        if base < 0:
            pos += 4
            continue
        guid = d[base + 0x10:base + 0x20]
        fv_len = struct.unpack_from('<Q', d, base + 0x20)[0]
        attrs = struct.unpack_from('<I', d, base + 0x2C)[0]
        hdr_len = struct.unpack_from('<H', d, base + 0x30)[0]
        csum = struct.unpack_from('<H', d, base + 0x32)[0]
        ext_off = struct.unpack_from('<H', d, base + 0x34)[0]
        rev = d[base + 0x37]
        print(f"    FvLength=0x{fv_len:x} ({fv_len})  Attrs=0x{attrs:x}  "
              f"HeaderLength=0x{hdr_len:x}  Checksum=0x{csum:x}  "
              f"ExtHeaderOffset=0x{ext_off:x}  Revision={rev}")
        print(f"    FileSystemGuid={guid.hex()}")
        # walk FFS
        off = base + hdr_len
        end = min(base + fv_len, len(d))
        n = 0
        while off + 24 <= end:
            name_guid = d[off:off + 16]
            if name_guid == b'\xff' * 16:
                print(f"    [free space at 0x{off:x}]")
                break
            ftype = d[off + 18]
            attrib = d[off + 19]
            size = d[off + 20] | (d[off + 21] << 8) | (d[off + 22] << 16)
            state = d[off + 23]
            hdr = 24
            if attrib & 0x01:
                size = struct.unpack_from('<Q', d, off + 24)[0]
                hdr = 32
            if size < hdr or off + size > end:
                print(f"    !! bad FFS at 0x{off:x} type={ftype:02x} size={size}")
                break
            n += 1
            sec = d[off + hdr:off + size]
            stype = sec[3] if len(sec) > 3 else -1
            ssize = (sec[0] | (sec[1] << 8) | (sec[2] << 16)) if len(sec) > 3 else 0
            print(f"    FFS#{n} @0x{off:x} type=0x{ftype:02x} attrib=0x{attrib:02x} "
                  f"size=0x{size:x} state=0x{state:02x} | sec type=0x{stype:02x} size=0x{ssize:x}")
            off = (off + size + 7) & ~7
        print(f"    FFS files: {n}")
        pos += 4


if __name__ == "__main__":
    for p in sys.argv[1:]:
        dump(p)
