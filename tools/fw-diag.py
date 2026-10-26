#!/usr/bin/env python3
"""Diagnostic: locate PE signatures / FV headers / compression inside a firmware image."""
import sys
import struct
from pathlib import Path


def main():
    for img in sys.argv[1:]:
        p = Path(img)
        d = p.read_bytes()
        print(f"\n=== {p.name}  {len(d)} bytes")
        # FV headers
        fvs = []
        pos = 0
        while True:
            pos = d.find(b'_FVH', pos)
            if pos < 0:
                break
            fvs.append(pos - 40)
            pos += 4
        print(f"  _FVH count: {len(fvs)}  first offsets: {fvs[:8]}")
        # LZMA / compression markers
        for sig, label in [
            (b'\x5d\x00\x00\x00', 'LZMA-alone hdr'),
            (b'\x5d\x00\x00\x01\x00', 'LZMA2'),
            (b'\x1f\x8b\x08', 'gzip'),
            (b'\x89LZO', 'lzo'),
            (b'LZMA', 'LZMA-magic'),
        ]:
            c = d.count(sig)
            if c:
                print(f"  {label}: {c} occurrences")
        # raw PE signatures
        n_pe = d.count(b'PE\x00\x00')
        n_mz = d.count(b'MZ')
        print(f"  'PE\\0\\0' count: {n_pe}   'MZ' count: {n_mz}")
        # check for EFI section compression GUID (common in DXE FV)
        for guid_hex, label in [
            ("a31280ad481e4195 8e127f4e1f39c0a0".replace(' ', ''), 'EFI_GUIDED_SECTION_COMPRESSION'),
        ]:
            pass
        # Tiano/Efi compression GUIDs (mixed-endian on disk)
        guids = {
            "Compression (LZ77/LZ78)": "05000809A2EB40114CD28F2A7611E466",
            "Guided/CRC32": "FC1BCDB70D454B9A9975B6C22794A4E9",
            "LZMA custom": "EE4E5898A39241469DDE7DC7043BDC59",
        }
        for label, g in guids.items():
            b = bytes.fromhex(g)
            # try all natural orderings
            variants = [b, b[::-1]]
            hits = sum(d.count(v) for v in variants)
            if hits:
                print(f"  {label}: {hits} hits (raw/reversed)")


if __name__ == "__main__":
    main()
