#!/bin/bash
U=<BUILD>/mu-silicium
echo "=== popsicle MemoryMapLib.c: all Display* entries ==="
grep -n -i "display" "$U/Platforms/Xiaomi/popsiclePkg/Library/MemoryMapLib/MemoryMapLib.c"
echo
echo "=== popsicle MemoryMapLib.c: entries whose name contains Reserved ==="
grep -n -i "reserved" "$U/Platforms/Xiaomi/popsiclePkg/Library/MemoryMapLib/MemoryMapLib.c" | head -20
echo
echo "=== sheng MemoryMapLib.c: all Display* entries (reference) ==="
grep -n -i "display" "$U/Platforms/Xiaomi/shengPkg/Library/MemoryMapLib/MemoryMapLib.c" 2>/dev/null || echo "shengPkg not in mu-silicium tree"
echo
echo "=== LocateMemoryRegionByName implementation ==="
grep -rn "LocateMemoryRegionByName" "$U/Silicon/Silicium/SiliciumPkg/Library/MemoryMapHelperLib/" 2>/dev/null | head
echo
echo "=== how names are matched (search for strcmp in MemoryMapHelperLib) ==="
find "$U/Silicon/Silicium" -ipath "*MemoryMapHelperLib*" -name "*.c" -exec grep -n "AsciiStrCmp\|strcmp\|RegionName\|Name" {} \; 2>/dev/null | head -20
