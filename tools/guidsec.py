#!/usr/bin/env python3
"""Dump the GUID_DEFINED section headers of each FFS file (to identify the codec)."""
import sys
import struct
from pathlib import Path


def gs(b):
    d1, d2, d3 = struct.unpack_from("<IHH", b, 0)
    return (f"{d1:08X}-{d2:04X}-{d3:04X}-"
            f"{b[8]:02X}{b[9]:02X}-{''.join(f'{x:02X}' for x in b[10:16])}")


KNOWN = {
    "EE4E5898-A392-4146-9DDE-7DC7043BDC59": "LZMA (custom EFI)",
    "A31280AD-481E-4195-8E12-7F4E1F39C0A0": "Tiano compression",
    "FC1BCDB7-0D45-4B9A-9975-B6C22794A4E9": "CRC32 guided",
    "1D301FE9-BE79-4353-91C2-D23C0DB7A2B8": "Brotli guided",
    "3D532050-5CDA-4FD0-879E-0F7F630D5AFB": "LZMA (EDK2 LzmaCustomDecompress)",
    "D42AE6BD-1352-4BFB-909A-CA72A6EAE889": "LZ4 guided",
    "9A21A84D-1D81-4E20-8DDF-6B4F2B4C9E36": "BrotliCustom",
}

for path in sys.argv[1:]:
    d = Path(path).read_bytes()
    print(f"\n=== {Path(path).name}")
    pos = 0
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
            ftype = d[off + 18]
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
            print(f"  FFS type=0x{ftype:02x} size=0x{size:x} | section type=0x{stype:02x} size=0x{ssize:x}")
            if stype == 0x02:
                g = gs(d[s + 4:s + 20])
                shl = struct.unpack_from('<H', d, s + 20)[0]
                sattr = struct.unpack_from('<H', d, s + 22)[0]
                print(f"    GUID_DEFINED guid={g}  ({KNOWN.get(g, 'unknown')})")
                print(f"    DataOffset=0x{shl:x}  Attributes=0x{sattr:x}")
                payload = d[s + shl:s + ssize]
                print(f"    payload starts {payload[:16].hex()}  len={len(payload)}")
                # try LZMA-alone
                import lzma
                for fmt, nm in ((lzma.FORMAT_ALONE, 'ALONE'),):
                    try:
                        dec = lzma.decompress(payload, format=fmt)
                        print(f"    LZMA-{nm} decompress OK -> {len(dec)} bytes, "
                              f"first {dec[:16].hex()}")
                    except Exception as e:
                        print(f"    LZMA-{nm} fail: {type(e).__name__}: {e}")
            off = (off + size + 7) & ~7
        pos += 4
