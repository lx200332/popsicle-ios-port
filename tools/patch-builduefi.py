#!/usr/bin/env python3
"""Make build_uefi.py pass every [boot_image] key through to mkbootimg, including the falsy ones.

Two defects, both silent:

* the argument loop tested `if value:`, so base = 0 -- which is exactly what the UniXNU boot step
  uses -- was dropped and mkbootimg fell back to its 0x10000000 default. The image then claims
  kernel_addr 0x10008000, an address below the DRAM base of this SoC.
* "pagesize" was missing from the argument map entirely, so the header always got mkbootimg's
  default 2048 no matter what the device config said, while scripts/unixnu/bootimg.py passes
  --pagesize and then verifies the result against it.
"""
from pathlib import Path

path = Path('<BUILD>/mu-silicium/build_uefi.py')
source = path.read_text()

if 'build_uefi.py.orig' not in source and Path('<BUILD>/mu-silicium/build_uefi.py.orig').exists():
    pass
if not Path('<BUILD>/mu-silicium/build_uefi.py.orig').exists():
    Path('<BUILD>/mu-silicium/build_uefi.py.orig').write_text(source)

old_map = '''        "tags_offset":    "--tags_offset"
    }'''
new_map = '''        "tags_offset":    "--tags_offset",
        "pagesize":       "--pagesize"
    }'''
if old_map in source:
    source = source.replace(old_map, new_map)

old_if = """        # Append Argument Value
        if value:"""
new_if = """        # Append Argument Value. The test is "is not None" rather than truthiness: base = 0 is a real
        # value (the kernel loads at kernel_offset in physical address space), and dropping it silently
        # falls back to mkbootimg's 0x10000000 default.
        if value is not None:"""
if old_if in source:
    source = source.replace(old_if, new_if)

path.write_text(source)
print('patched build_uefi.py')
