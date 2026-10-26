#!/bin/bash
U=/root/build/mu-silicium
F="$U/Silicon/Silicium/SiliciumPkg/Drivers/SimpleFbDxe/SimpleFb.c"
echo "=== SimpleFb.c lines 60-125 ==="
sed -n '60,125p' "$F"
echo
echo "=== SimpleFbDxe.inf full ==="
cat "$U/Silicon/Silicium/SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf"
