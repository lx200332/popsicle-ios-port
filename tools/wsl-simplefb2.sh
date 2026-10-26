#!/bin/bash
U=/root/build/mu-silicium
F="$U/Silicon/Silicium/SiliciumPkg/Drivers/SimpleFbDxe/SimpleFb.c"
echo "=== SimpleFb.c lines 120-190 ==="
sed -n '120,190p' "$F"
echo
echo "=== all FixedPcdGet / PcdGet in SimpleFb.c ==="
grep -n "PcdGet\|PcdSet" "$F"
echo
echo "=== search tree for PcdFrameBufferBase definitions ==="
grep -rn "PcdFrameBufferBase\|PcdFrameBufferSize" "$U/Silicon/Silicium/SiliciumPkg/" 2>/dev/null | head -20
