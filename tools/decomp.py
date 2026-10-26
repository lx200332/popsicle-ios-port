#!/usr/bin/env python3
"""Decompress the LZMA guided sections and dump the resulting blobs for inspection."""
import sys
import lzma
import struct
from pathlib import Path

OUT = Path(sys.argv[1])
OUT.mkdir(parents=True, exist_ok=True)

GUID_LZMA = "EE4E5898-3914-4259-9D6E-DC7BD79403CF"


def gs(b):
    d1, d2, d3 = struct.unpack_from("<IHH", b, 0)
    return (f"{d1:08X}-{d2:04X}-{d3:04X}-"
            f"{b[8]:02X}{b[9]:02X}-{''.join(f'{x:02X}' for x in b[10:16])}")


for path in sys.argv[2:]:
    d = Path(path).read_bytes()
    name = Path(path).stem
    pos = 0
    idx = 0
    while True:
        pos = d.find(b'_FVH', pos)
        if pos < 0:
            break
        base = pos - 0x28
        if base < 0:
            pos += 4
            continue
        fv_len = struct.unpack_from('<Q', d, base + 0x20)[0]
        hdr_len = struct.unpack_from('<H', d, base + 0x30)[0]
        off = base + hdr_len
        end = min(base + fv_len, len(d))
        while off + 24 <= end:
            if d[off:off + 16] == b'\xff' * 16:
                break
            attrib = d[off + 19]
            size = d[off + 20] | (d[off + 21] << 8) | (d[off + 22] << 16)
            fhdr = 24
            if attrib & 0x01:
                size = struct.unpack_from('<Q', d, off + 24)[0]
                fhdr = 32
            if size < fhdr or off + size > end:
                break
            s = off + fhdr
            ssize = d[s] | (d[s + 1] << 8) | (d[s + 2] << 16)
            stype = d[s + 3]
            if stype == 0x02 and gs(d[s + 4:s + 20]) == GUID_LZMA:
                shl = struct.unpack_from('<H', d, s + 20)[0]
                payload = d[s + shl:s + ssize]
                try:
                    dec = lzma.decompress(payload, format=lzma.FORMAT_ALONE)
                    fn = OUT / f"{name}_fv{idx}.dec"
                    fn.write_bytes(dec)
                    print(f"{name}: FFS@0x{off:x} -> {fn.name}  {len(dec)} bytes")
                    print(f"   head: {dec[:48].hex()}")
                    print(f"   _FVH in dec: {dec.count(b'_FVH')}  "
                          f"'PE': {dec.count(b'PE\x00\x00')}  'MZ': {dec.count(b'MZ')}")
                    idx += 1
                except Exception as e:
                    print(f"{name}: LZMA fail {e}")
            off = (off + size + 7) & ~7
        pos += 4
