#!/usr/bin/env python3
"""Extract PE32 EFI drivers from Qualcomm firmware images.

Strategy: scan the raw bytes for an MZ header immediately followed (within the
PE header's e_lfanew) by a valid "PE\0\0" signature, then compute the true
image length from the section table.  This works regardless of whether the
container is a plain FV, an ELF-wrapped FV, or a flat blob.
"""
import struct
import sys
import os
import json
import hashlib
from pathlib import Path


def parse_pe(data: bytes, off: int):
    """Return (size, machine, subsystem, name_from_export_or_empty) or None."""
    if data[off:off + 2] != b'MZ':
        return None
    if off + 0x40 > len(data):
        return None
    e_lfanew = struct.unpack_from('<I', data, off + 0x3C)[0]
    if e_lfanew < 0x40 or e_lfanew > 0x1000:
        return None
    pe_off = off + e_lfanew
    if data[pe_off:pe_off + 4] != b'PE\x00\x00':
        return None
    try:
        machine, num_sections = struct.unpack_from('<HH', data, pe_off + 4)
        opt_size = struct.unpack_from('<H', data, pe_off + 20)[0]
        characteristics = struct.unpack_from('<H', data, pe_off + 22)[0]
    except struct.error:
        return None
    opt_off = pe_off + 24
    # IMAGE_OPTIONAL_HEADER magic: 0x10b = PE32, 0x20b = PE32+
    if off + e_lfanew + 24 + 2 > len(data):
        return None
    magic = struct.unpack_from('<H', data, opt_off)[0]
    if magic not in (0x10b, 0x20b):
        return None
    subsystem = 0
    if opt_size >= 0x44:
        subsystem = struct.unpack_from('<H', data, opt_off + 0x44)[0]
    # Section table follows the optional header
    sect_off = opt_off + opt_size
    max_end = 0
    for i in range(num_sections):
        s = sect_off + i * 40
        if s + 40 > len(data):
            return None
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', data, s + 8)
        end = rawptr + rawsize
        if end > max_end:
            max_end = end
    size = ((sect_off - off) + max_end + 0x1FF) & ~0x1FF
    if size < 0x200 or off + size > len(data) + 0x2000:
        return None
    return {
        "off": off,
        "size": min(size, len(data) - off),
        "machine": machine,
        "subsystem": subsystem,
        "pe32plus": magic == 0x20b,
    }


def scan(data: bytes):
    """Find aligned MZ/PE32 candidates."""
    out = []
    pos = 0
    while True:
        pos = data.find(b'MZ', pos)
        if pos < 0:
            break
        # EFI images are 4-byte aligned at minimum
        if pos % 4 == 0:
            r = parse_pe(data, pos)
            if r:
                out.append(r)
                pos += max(r["size"], 4)
                continue
        pos += 2
    return out


def main():
    if len(sys.argv) < 3:
        print("usage: extract-efi.py <outdir> <image> [image...]")
        return 1
    outdir = Path(sys.argv[1])
    outdir.mkdir(parents=True, exist_ok=True)

    all_found = []
    for img in sys.argv[2:]:
        p = Path(img)
        data = p.read_bytes()
        found = scan(data)
        print(f"=== {p.name}: {len(data)} bytes, {len(found)} PE32 images")
        for i, r in enumerate(found):
            blob = data[r["off"]:r["off"] + r["size"]]
            sha = hashlib.sha256(blob).hexdigest()[:16]
            name = f"{p.stem}_{i:03d}_{sha}.efi"
            (outdir / name).write_bytes(blob)
            r["file"] = name
            r["source"] = p.name
            all_found.append(r)

    (outdir / "manifest.json").write_text(json.dumps(all_found, indent=2))
    print(f"\ntotal: {len(all_found)} images -> {outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
