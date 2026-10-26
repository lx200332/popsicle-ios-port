#!/bin/bash
U=<BUILD>/mu-silicium
echo "=== DisplayDxe in Binaries/popsicle ==="
find "$U/Binaries/popsicle" -ipath "*isplay*" | head -30
echo
echo "=== USE_CUSTOM_DISPLAY_DRIVER refs ==="
grep -rn "USE_CUSTOM_DISPLAY_DRIVER" "$U/Platforms/Xiaomi/popsiclePkg/" 2>/dev/null | head -20
echo
echo "=== display lines in DXE.inc ==="
grep -n -i "display" "$U/Platforms/Xiaomi/popsiclePkg/Include/DXE.inc" | head -20
echo
echo "=== DisplayDxe in popsicle.dsc ==="
grep -rn -i "DisplayDxe" "$U/Platforms/Xiaomi/popsiclePkg/popsicle.dsc" | head -20
echo
echo "=== RAW.inc !if / !endif structure ==="
grep -n '^!' "$U/Platforms/Xiaomi/popsiclePkg/Include/RAW.inc"
echo
echo "=== rawfiles referenced count ==="
grep -c 'SECTION RAW = Binaries/popsicle/RawFiles/' "$U/Platforms/Xiaomi/popsiclePkg/Include/RAW.inc"
