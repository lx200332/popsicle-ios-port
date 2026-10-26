#!/bin/bash
U=/root/build/mu-silicium
echo "=== DXE.inc 55-95 ==="
sed -n '55,95p' "$U/Platforms/Xiaomi/popsiclePkg/Include/DXE.inc"
echo
echo "=== APRIORI.inc 85-115 ==="
sed -n '85,115p' "$U/Platforms/Xiaomi/popsiclePkg/Include/APRIORI.inc"
echo
echo "=== DisplayDxe.inf content ==="
cat "$U/Binaries/popsicle/QcomPkg/Drivers/DisplayDxe/DisplayDxe.inf"
echo
echo "=== DisplayDxe.efi size / strings for panel xml names ==="
ls -l "$U/Binaries/popsicle/QcomPkg/Drivers/DisplayDxe/DisplayDxe.efi"
strings -a "$U/Binaries/popsicle/QcomPkg/Drivers/DisplayDxe/DisplayDxe.efi" 2>/dev/null | grep -i -E '\.xml|Panel_|RawFiles' | head -20
echo
echo "=== other DisplayDxe refs anywhere in the tree (non-Binaries) ==="
grep -rn "DisplayDxe" "$U/Platforms/Xiaomi/popsiclePkg/" "$U/Silicon/Qualcomm/QcomPkg/QcomPkg.dsc.inc" 2>/dev/null | head -20
