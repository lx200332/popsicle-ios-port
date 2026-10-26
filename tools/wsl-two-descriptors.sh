#!/bin/bash
M=<BUILD>/mu-silicium/Platforms/Xiaomi/popsiclePkg/Library/MemoryMapLib/MemoryMapLib.c
echo "=== DBI_Dump / DXE_Heap descriptors ==="
grep -n 'DBI_Dump\|DXE_Heap' "$M"
echo
echo "=== all descriptor lines with Conv ==="
grep -n 'Conv' "$M" | head -20
echo
echo "=== tag legend in xbl strings (search for the tag chars) ==="
grep -o 'memorymap[^"]\{0,120\}' /mnt<WORKSPACE>/popsicle-port/extract/xblcfg.strings.txt | head -10
