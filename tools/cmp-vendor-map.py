#!/usr/bin/env python3
"""Compare our MemoryMapLib descriptors against the vendor's own memorymap blob."""
import json
import re
import sys
from pathlib import Path

MM = Path('<BUILD>/mu-silicium/Platforms/Xiaomi/popsiclePkg/Library/MemoryMapLib/MemoryMapLib.c')
VENDOR = Path('/mnt<WORKSPACE>/popsicle-port/extract/uefi_memmap.json')

source = MM.read_text()
rows = [(m[1], int(m[2], 16), int(m[3], 16), m[4]) for m in re.finditer(
    r'\{"([^"]+)"\s*,\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),([^}]+)\}', source)]
vendor = json.loads(VENDOR.read_text())

print(f'our descriptors: {len(rows)}   vendor regions: {len(vendor)}')
print()

def covering(base, size):
    hits = []
    for name, a, s, attrs in rows:
        if a <= base and base + size <= a + s:
            hits.append((name, a, s, attrs))
    return hits

uncovered, mismatched = [], []
for region in vendor:
    name, base, size = region['name'], region['base'], region['size']
    tag = region['tags'][1]
    hits = covering(base, size)
    if not hits:
        uncovered.append((tag, name, base, size))
        continue
    # The vendor's carveout has to land on memory the firmware will not hand out.
    ok = any(not re.search(r'\bConv\b', attrs) for _, _, _, attrs in hits)
    if not ok:
        mismatched.append((tag, name, base, size, [h[0] for h in hits]))

print(f'UNCOVERED vendor regions ({len(uncovered)}):')
for tag, name, base, size in uncovered:
    print(f'  {tag:<16} {base:#012x} + {size:#010x}   ({name})')
print()
print(f'COVERED-BUT-CONVENTIONAL ({len(mismatched)}):')
for tag, name, base, size, hits in mismatched:
    print(f'  {tag:<16} {base:#012x} + {size:#010x}   by {hits}')
print()
print(f'OK: {len(vendor) - len(uncovered) - len(mismatched)} / {len(vendor)}')
sys.exit(1 if (uncovered or mismatched) else 0)
