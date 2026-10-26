#!/usr/bin/env python3
"""Extract EFI drivers with the uefi_firmware package (handles FV, FFS, LZMA, Tiano)."""
import sys
import os
from pathlib import Path

import uefi_firmware
from uefi_firmware import AutoParser

OUT = Path(sys.argv[1])
OUT.mkdir(parents=True, exist_ok=True)
count = 0
names = []

for path in sys.argv[2:]:
    data = Path(path).read_bytes()
    print(f"\n=== {Path(path).name} ({len(data)} bytes)")
    try:
        parser = AutoParser(data)
        objs = parser.parse()
    except Exception as e:
        print(f"  parse error: {type(e).__name__}: {e}")
        continue
    if objs is None:
        print("  no objects")
        continue
    if not isinstance(objs, list):
        objs = [objs]

    stack = list(objs)
    while stack:
        o = stack.pop()
        if isinstance(o, list):
            stack.extend(o)
            continue
        cls = type(o).__name__
        proc = getattr(o, 'process', None)
        if proc and cls in ('CompressedSection', 'GuidDefinedSection'):
            try:
                inner = proc()
                if inner:
                    stack.extend(inner if isinstance(inner, list) else [inner])
                    continue
            except Exception as e:
                print(f"    {cls} process fail: {e}")
        if cls in ('UefiCompression', 'TianoCompressedSection'):
            try:
                inner = proc()
                if inner:
                    stack.extend(inner if isinstance(inner, list) else [inner])
                    continue
            except Exception:
                pass
        if cls in ('UefiFile', 'FirmwareFileSystem', 'FirmwareVolume',
                   'Instruction', 'Section'):
            if hasattr(o, 'objects') and o.objects:
                stack.extend(o.objects)
            continue
        if cls not in ('UefiCompression',):
            if hasattr(o, 'objects') and o.objects:
                stack.extend(o.objects)
        # PE32 payload
        if hasattr(o, 'data') and o.data and o.data[:2] == b'MZ':
            name = getattr(o, 'name', None) or f"drv{count:03d}"
            fn = OUT / f"{name}.efi"
            n = 1
            while fn.exists():
                fn = OUT / f"{name}_{n}.efi"
                n += 1
            fn.write_bytes(o.data)
            names.append((name, len(o.data)))
            count += 1

print(f"\ntotal PE32 extracted: {count}")
for n, s in names:
    print(f"  {n:40s} {s}")
